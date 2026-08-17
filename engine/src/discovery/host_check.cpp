// host_check.cpp — "the lidar expects host 192.168.1.5; this machine is not
// 192.168.1.5".
//
// THE FIELD FAILURE, in one paragraph. A Mid-360 persists the host address it
// was last configured to stream to. It does not ARP for it, does not answer
// ping, and does not complain when that host is absent — it simply streams
// into a subnet where nobody is listening, and the app shows "connected, 0
// points" forever. On 2026-08-17 the fix was a hand-typed
// `route add -host 192.168.1.159 -interface en7` plus an interface alias for
// 192.168.1.5. The operator cannot be expected to know that. This function
// turns it into a sentence the UI can print, and a suggestion it can act on.
//
// Everything here is pure arithmetic over an interface list, and the list is
// injectable — which is what makes it testable without a second NIC.
//
// Owner: A16.
#include "scanengine/discovery/discovery.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net_compat.h"
#include "scanengine/core/log.h"

namespace scanengine {
namespace discovery {
namespace {

constexpr const char* kMod = "discovery";

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i != 0) s += ", ";
    s += v[i];
  }
  return s;
}

}  // namespace

// --- IPv4 arithmetic --------------------------------------------------------

bool ParseIpv4(const std::string& text, std::uint32_t* out_host_order) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  // The %c catches "1.2.3.4.5" and "1.2.3.4x", which sscanf would otherwise
  // accept by ignoring the remainder.
  const int n = std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail);
  if (n != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  if (out_host_order) {
    *out_host_order = (a << 24) | (b << 16) | (c << 8) | d;
  }
  return true;
}

std::string Ipv4ToString(std::uint32_t host_order) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (host_order >> 24) & 0xFFu,
                (host_order >> 16) & 0xFFu, (host_order >> 8) & 0xFFu, host_order & 0xFFu);
  return buf;
}

bool SameSubnet(const std::string& a, const std::string& b, const std::string& netmask) {
  std::uint32_t ia = 0, ib = 0, im = 0;
  if (!ParseIpv4(a, &ia) || !ParseIpv4(b, &ib)) return false;
  // No mask (or a nonsense one): fall back to /24, which is what every
  // Mid-360 deployment this project has seen actually uses and is far more
  // useful than refusing to answer.
  if (!ParseIpv4(netmask, &im) || im == 0) im = 0xFFFFFF00u;
  return (ia & im) == (ib & im);
}

int PrefixLen(const std::string& netmask) {
  std::uint32_t m = 0;
  if (!ParseIpv4(netmask, &m)) return -1;
  const std::uint32_t inverted = ~m;
  if ((inverted & (inverted + 1u)) != 0u) return -1;  // not contiguous
  int bits = 0;
  while (m & 0x80000000u) {
    ++bits;
    m <<= 1;
  }
  return bits;
}

// --- interface enumeration --------------------------------------------------

Result<std::vector<LocalInterface>> EnumerateLocalInterfaces() {
  std::vector<LocalInterface> out;

#if defined(_WIN32)
  discovery_net::ensure_winsock();
  // GetAdaptersAddresses wants a buffer it will tell us the size of. Two
  // calls, and a generous first guess so the common case is one.
  ULONG size = 16384;
  std::vector<std::uint8_t> buf(size);
  ULONG rc = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                             GAA_FLAG_SKIP_DNS_SERVER,
                                nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                &size);
    if (rc != ERROR_BUFFER_OVERFLOW) break;
    buf.resize(size);
  }
  if (rc != NO_ERROR) {
    return set_last_error(ScanError::kIoError,
                          "interface enumeration: GetAdaptersAddresses failed (%lu)",
                          static_cast<unsigned long>(rc));
  }
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a != nullptr;
       a = a->Next) {
    for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
      if (u->Address.lpSockaddr == nullptr) continue;
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      char ip[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == nullptr) continue;
      LocalInterface li;
      // AdapterName is the GUID; FriendlyName is what the operator sees in
      // the network panel, so that is the one worth printing.
      char name[128] = {0};
      if (a->FriendlyName != nullptr) {
        ::WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name) - 1, nullptr,
                              nullptr);
      }
      li.name = name[0] != '\0' ? name : (a->AdapterName ? a->AdapterName : "");
      li.ipv4 = ip;
      li.netmask = Ipv4ToString(u->OnLinkPrefixLength >= 32
                                    ? 0xFFFFFFFFu
                                    : ~((1u << (32 - u->OnLinkPrefixLength)) - 1u));
      li.is_loopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
      li.is_up = (a->OperStatus == IfOperStatusUp);
      out.push_back(li);
    }
  }
  return out;

#elif defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) {
    return set_last_error(ScanError::kIoError, "interface enumeration: getifaddrs failed (%d)",
                          errno);
  }
  for (ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if (ifa->ifa_addr->sa_family != AF_INET) continue;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
    char ip[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == nullptr) continue;
    LocalInterface li;
    li.name = ifa->ifa_name != nullptr ? ifa->ifa_name : "";
    li.ipv4 = ip;
    if (ifa->ifa_netmask != nullptr && ifa->ifa_netmask->sa_family == AF_INET) {
      const auto* nm = reinterpret_cast<const sockaddr_in*>(ifa->ifa_netmask);
      char mask[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &nm->sin_addr, mask, sizeof(mask)) != nullptr) li.netmask = mask;
    }
    li.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
    li.is_up = (ifa->ifa_flags & IFF_UP) != 0;
    out.push_back(li);
  }
  ::freeifaddrs(list);
  return out;

#else
  return set_last_error(ScanError::kNotSupported,
                        "interface enumeration is not implemented on this platform");
#endif
}

// --- the check ---------------------------------------------------------------

HostCheck CheckHostReachability(const Mid360Beacon& beacon,
                                const std::vector<LocalInterface>& interfaces) {
  HostCheck hc;

  const std::string& want = beacon.persisted_host_ip;
  const std::string& lidar = beacon.lidar_ip;
  const std::string mask = beacon.netmask.empty() ? std::string("255.255.255.0") : beacon.netmask;

  // Candidates: every non-loopback, up interface address on the LIDAR'S
  // subnet. Loopback is excluded because a lidar cannot reach it, and that is
  // precisely the mistake a naive "do I have this IP?" check would make on a
  // machine with 127.0.0.1 and nothing else.
  std::string want_iface;
  for (const LocalInterface& li : interfaces) {
    if (!want.empty() && li.ipv4 == want && !li.is_loopback) {
      hc.host_ip_is_local = true;
      if (li.is_up) want_iface = li.name;
      else if (want_iface.empty()) want_iface = li.name;
    }
    if (li.is_loopback || !li.is_up) continue;
    if (!lidar.empty() && SameSubnet(li.ipv4, lidar, mask)) {
      hc.local_candidates.push_back(li.ipv4);
      if (hc.suggested_interface.empty()) hc.suggested_interface = li.name;
    }
  }
  hc.on_lidar_subnet = !hc.local_candidates.empty();

  const int prefix = PrefixLen(mask) > 0 ? PrefixLen(mask) : 24;

  // Every non-loopback address this machine holds, for the "wrong network"
  // message — the operator needs to see what they DO have to recognize the
  // mistake ("oh, I'm on the wifi").
  std::vector<std::string> all_local;
  for (const LocalInterface& li : interfaces) {
    if (!li.is_loopback) all_local.push_back(li.ipv4);
  }
  const std::string all_local_text = all_local.empty() ? "none" : join(all_local);

  // A plausible host address on the lidar's subnet, for when we have to
  // invent one: <lidar network>.5, which is the address the field session's
  // lidar had persisted and a conventional choice besides.
  std::string example_host = "192.168.1.5";
  {
    std::uint32_t l = 0;
    if (ParseIpv4(lidar, &l)) example_host = Ipv4ToString((l & 0xFFFFFF00u) | 5u);
  }

  char note[512];
  if (want.empty()) {
    // A beacon with no persisted host is a factory-fresh or reset lidar: it
    // has to be TOLD a host, and any address on its subnet will do.
    hc.suggested_host_ip = hc.local_candidates.empty() ? std::string() : hc.local_candidates[0];
    if (hc.suggested_host_ip.empty()) {
      std::snprintf(note, sizeof(note),
                    "The lidar (%s) has no host address configured, and this machine has no "
                    "address on its subnet. Add one (e.g. %s/%d) and re-run discovery.",
                    lidar.empty() ? "unknown IP" : lidar.c_str(), example_host.c_str(), prefix);
    } else {
      std::snprintf(note, sizeof(note),
                    "The lidar (%s) has no host address configured. LidarScan will point it "
                    "at this machine (%s) when you connect.",
                    lidar.c_str(), hc.suggested_host_ip.c_str());
    }
  } else if (hc.host_ip_is_local) {
    hc.suggested_host_ip = want;
    if (hc.suggested_interface.empty()) hc.suggested_interface = want_iface;
    std::snprintf(note, sizeof(note),
                  "Ready: the lidar (%s) streams to %s and this machine holds that address%s%s.",
                  lidar.c_str(), want.c_str(), want_iface.empty() ? "" : " on ",
                  want_iface.empty() ? "" : want_iface.c_str());
  } else if (hc.on_lidar_subnet) {
    // The good failure: we are on the right wire, wrong address. An alias is
    // one command, and either side can move — so offer both.
    hc.suggested_host_ip = want;
    std::snprintf(note, sizeof(note),
                  "The lidar (%s) expects host %s, which this machine does not have. This "
                  "machine is on its subnet as %s%s%s — add the alias "
                  "(sudo ifconfig %s alias %s/%d) or let LidarScan reconfigure the lidar to "
                  "stream to %s.",
                  lidar.c_str(), want.c_str(), hc.local_candidates[0].c_str(),
                  hc.suggested_interface.empty() ? "" : " on ",
                  hc.suggested_interface.empty() ? "" : hc.suggested_interface.c_str(),
                  hc.suggested_interface.empty() ? "<iface>" : hc.suggested_interface.c_str(),
                  want.c_str(), prefix, hc.local_candidates[0].c_str());
  } else {
    // The bad failure: different wire entirely. Nothing to suggest but the
    // truth, and the truth is actionable — plug into the lidar's network.
    hc.suggested_host_ip = want;
    std::snprintf(note, sizeof(note),
                  "The lidar (%s) expects host %s, and this machine has no address on that "
                  "subnet (has: %s). Connect to the lidar's network, or set this machine's "
                  "interface to %s/%d.",
                  lidar.empty() ? "unknown IP" : lidar.c_str(), want.c_str(),
                  all_local_text.c_str(), want.c_str(), prefix);
  }
  hc.note = note;
  return hc;
}

HostCheck CheckHostReachability(const Mid360Beacon& beacon) {
  Result<std::vector<LocalInterface>> ifs = EnumerateLocalInterfaces();
  if (!ifs.ok()) {
    SCAN_LOG_WARN(kMod, "host check: interface enumeration failed (%s)",
                  error_str(ifs.error()));
    return CheckHostReachability(beacon, {});
  }
  return CheckHostReachability(beacon, std::move(ifs).value());
}

}  // namespace discovery
}  // namespace scanengine
