// mid360_discover.cpp — listen for Mid-360 heartbeats and merge them into one
// record per lidar.
//
// The socket half of A16. All of the interesting decisions are documented at
// bind_any_udp() in net_compat.h (any-bound, SO_REUSEPORT) and in
// mid360_beacon.cpp (the frame layout); what is left here is a select() loop
// with a deadline and a dedup map.
//
// Owner: A16.
#include "scanengine/discovery/discovery.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "net_compat.h"
#include "scanengine/core/log.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {
namespace discovery {
namespace {

constexpr const char* kMod = "discovery";

// The largest heartbeat observed is 430 bytes. 2048 matches what
// transport/udp_source.cpp uses for the point stream and leaves room for a
// firmware that grows the frame; anything longer is truncated by recvfrom and
// will simply fail its payload CRC rather than be silently believed.
constexpr std::size_t kRecvBufBytes = 2048;

struct BoundSocket {
  scan_socket_t fd = SCAN_INVALID_SOCKET;
  std::uint16_t port = 0;
};

std::int64_t now_ns() { return SteadyClock::now().nanos; }

// Which record a heartbeat belongs to. The serial number is the identity —
// two lidars on one subnet is a real configuration (a survey rig with a
// second head) and IP is not stable across a DHCP lease. A frame that
// somehow carried no SN falls back to its sender address so it still shows up
// exactly once.
std::string identity_of(const Mid360Beacon& b) {
  if (!b.sn.empty()) return "sn:" + b.sn;
  if (!b.lidar_ip.empty()) return "ip:" + b.lidar_ip;
  return "src:" + b.source_ip;
}

// Later heartbeats win, EXCEPT that a fully-parsed record is never replaced
// by a heuristic one — a clipped datagram must not erase good data we already
// have.
void merge(Mid360Beacon* into, const Mid360Beacon& fresh) {
  const std::uint32_t seen = into->beacons_seen + 1;
  if (!(fresh.heuristic && !into->heuristic)) {
    *into = fresh;
  } else {
    into->t_last_seen_ns = fresh.t_last_seen_ns;
  }
  into->beacons_seen = seen;
}

}  // namespace

Result<std::vector<Mid360Beacon>> DiscoverMid360(int timeout_ms) {
  DiscoverOptions opt;
  opt.timeout_ms = timeout_ms;
  return DiscoverMid360(opt);
}

Result<std::vector<Mid360Beacon>> DiscoverMid360(const DiscoverOptions& opt) {
  std::vector<std::uint16_t> ports = opt.ports;
  if (ports.empty()) ports = {kMid360PushPort, kMid360PushPortAlt};
  if (opt.timeout_ms < 0) {
    return set_last_error(ScanError::kInvalidArgument, "discovery: negative timeout");
  }

  discovery_net::ensure_winsock();

  std::vector<BoundSocket> socks;
  std::string bind_errors;
  for (const std::uint16_t p : ports) {
    int e = 0;
    const scan_socket_t fd = discovery_net::bind_any_udp(p, &e);
    if (fd == SCAN_INVALID_SOCKET) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%u (errno %d)", bind_errors.empty() ? "" : ", ", p, e);
      bind_errors += buf;
      SCAN_LOG_WARN(kMod, "cannot bind udp/%u for discovery (errno %d) — skipping", p, e);
      continue;
    }
    socks.push_back(BoundSocket{fd, p});
  }
  if (socks.empty()) {
    // The overwhelmingly likely cause, and the one the field session hit:
    // Livox Viewer 2 or a second LidarScan already owns the port.
    return set_last_error(ScanError::kBusy,
                          "discovery: no listen port could be bound (%s) — another "
                          "LidarScan or Livox Viewer may be running",
                          bind_errors.c_str());
  }

  const std::int64_t deadline_ns = now_ns() + static_cast<std::int64_t>(opt.timeout_ms) * 1000000;
  std::map<std::string, Mid360Beacon> found;
  std::vector<std::uint8_t> buf(kRecvBufBytes);
  std::uint64_t datagrams = 0, rejected = 0;

  for (;;) {
    const std::int64_t remaining_ns = deadline_ns - now_ns();
    if (remaining_ns <= 0) break;
    if (opt.stop_after_devices != 0 && found.size() >= opt.stop_after_devices) break;

    fd_set rset;
    FD_ZERO(&rset);
    scan_socket_t maxfd = 0;
    for (const BoundSocket& s : socks) {
      FD_SET(s.fd, &rset);
      if (s.fd > maxfd) maxfd = s.fd;
    }
    timeval tv;
    // Cap the per-iteration wait at 250 ms so stop_after_devices and the
    // deadline are both honoured promptly regardless of traffic.
    std::int64_t wait_ns = remaining_ns < 250000000 ? remaining_ns : 250000000;
    tv.tv_sec = static_cast<long>(wait_ns / 1000000000);
    tv.tv_usec = static_cast<int>((wait_ns % 1000000000) / 1000);

    const int rc = ::select(static_cast<int>(maxfd) + 1, &rset, nullptr, nullptr, &tv);
    if (rc < 0) {
      if (discovery_net::would_block()) continue;
      const int e = scan_socket_errno;
      for (const BoundSocket& s : socks) scan_close_socket(s.fd);
      return set_last_error(ScanError::kIoError, "discovery: select() failed (errno %d)", e);
    }
    if (rc == 0) continue;

    for (const BoundSocket& s : socks) {
      if (!FD_ISSET(s.fd, &rset)) continue;
      sockaddr_in from;
      std::memset(&from, 0, sizeof(from));
#if defined(_WIN32)
      int fromlen = sizeof(from);
#else
      socklen_t fromlen = sizeof(from);
#endif
      const auto n = ::recvfrom(s.fd, reinterpret_cast<char*>(buf.data()),
                                static_cast<int>(buf.size()), 0,
                                reinterpret_cast<sockaddr*>(&from), &fromlen);
      if (n <= 0) continue;
      ++datagrams;

      Result<Mid360Beacon> parsed =
          ParseMid360Beacon(buf.data(), static_cast<std::size_t>(n), opt.allow_heuristic);
      if (!parsed.ok()) {
        ++rejected;
        continue;
      }
      Mid360Beacon b = std::move(parsed).value();
      if (opt.require_crc && !b.crc_ok) {
        ++rejected;
        continue;
      }
      char ip[INET_ADDRSTRLEN] = {0};
      (void)::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      b.source_ip = ip;
      // The port we HEARD it on, not the one the frame advertises: that is
      // what a caller has to bind next time, and they can differ if a
      // firmware is mid-reconfiguration.
      b.push_port_seen = s.port;
      b.t_last_seen_ns = now_ns();
      b.beacons_seen = 1;

      const std::string id = identity_of(b);
      auto it = found.find(id);
      if (it == found.end()) {
        found.emplace(id, b);
        SCAN_LOG_INFO(kMod, "found %s", b.describe().c_str());
      } else {
        merge(&it->second, b);
      }
    }
  }

  for (const BoundSocket& s : socks) scan_close_socket(s.fd);

  std::vector<Mid360Beacon> out;
  out.reserve(found.size());
  for (auto& kv : found) out.push_back(std::move(kv.second));
  SCAN_LOG_INFO(kMod, "discovery finished: %zu lidar(s), %llu datagram(s), %llu rejected",
                out.size(), static_cast<unsigned long long>(datagrams),
                static_cast<unsigned long long>(rejected));
  return out;
}

}  // namespace discovery
}  // namespace scanengine
