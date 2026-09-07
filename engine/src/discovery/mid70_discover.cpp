// mid70_discover.cpp — listen for Livox SDK v1 broadcasts on UDP 55000 and
// merge them into one record per lidar.
//
// The socket half of the Mid-70's discovery. Structurally identical to
// mid360_discover.cpp — bind 0.0.0.0 (a broadcast reaches only an any-bound
// socket on macOS/BSD; see bind_any_udp() in net_compat.h), select() with a
// deadline, dedup map — and deliberately so: two listeners that drift apart
// are two bugs to find instead of one.
//
// The one behavioural difference worth knowing about is not in this file but
// in the device: a Mid-70 broadcasts ONLY while nothing is connected to it.
// The SDK's handshake stops the broadcast. So an empty result means either
// "no Mid-70 here" or "something already owns it" — which is why the kBusy
// message below names the SDK, Livox Viewer and livox_ros_driver rather than
// just saying the port is taken.
//
// Owner: A16 (Phase 5 of the Mid-70 plan).
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

// A broadcast is 50 bytes. 2048 matches the Mid-360 listener and every other
// UDP read path in the engine; anything longer than the declared length is
// rejected by ParseMid70Beacon's exact length check rather than believed.
constexpr std::size_t kRecvBufBytes = 2048;

struct BoundSocket {
  scan_socket_t fd = SCAN_INVALID_SOCKET;
  std::uint16_t port = 0;
};

std::int64_t now_ns() { return SteadyClock::now().nanos; }

// Which record a broadcast belongs to. The broadcast code is the identity:
// it is the only name SDK v1's AddLidarToConnect() accepts, it is printed on
// the device, and it never changes. A frame that somehow carried none falls
// back to its sender address so it still shows up exactly once.
std::string identity_of(const Mid70Beacon& b) {
  if (!b.broadcast_code.empty()) return "code:" + b.broadcast_code;
  if (!b.lidar_ip.empty()) return "ip:" + b.lidar_ip;
  return "src:" + b.source_ip;
}

}  // namespace

Result<std::vector<Mid70Beacon>> DiscoverMid70(int timeout_ms) {
  DiscoverOptions opt;
  opt.timeout_ms = timeout_ms;
  return DiscoverMid70(opt);
}

Result<std::vector<Mid70Beacon>> DiscoverMid70(const DiscoverOptions& opt) {
  std::vector<std::uint16_t> ports = opt.ports;
  if (ports.empty()) ports = {kMid70BroadcastPort};
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
      SCAN_LOG_WARN(kMod, "cannot bind udp/%u for mid-70 discovery (errno %d) — skipping", p,
                    e);
      continue;
    }
    socks.push_back(BoundSocket{fd, p});
  }
  if (socks.empty()) {
    return set_last_error(ScanError::kBusy,
                          "discovery: no listen port could be bound (%s) — the Livox SDK, "
                          "Livox Viewer or a livox_ros_driver node may already hold it",
                          bind_errors.c_str());
  }

  const std::int64_t deadline_ns =
      now_ns() + static_cast<std::int64_t>(opt.timeout_ms) * 1000000;
  std::map<std::string, Mid70Beacon> found;
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
    const std::int64_t wait_ns = remaining_ns < 250000000 ? remaining_ns : 250000000;
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

      if (opt.raw_sink != nullptr) {
        char src[INET_ADDRSTRLEN] = {0};
        (void)::inet_ntop(AF_INET, &from.sin_addr, src, sizeof(src));
        opt.raw_sink(buf.data(), static_cast<std::size_t>(n), src, s.port, opt.raw_sink_user);
      }

      Result<Mid70Beacon> parsed =
          ParseMid70Beacon(buf.data(), static_cast<std::size_t>(n));
      if (!parsed.ok()) {
        ++rejected;
        continue;
      }
      Mid70Beacon b = std::move(parsed).value();
      char ip[INET_ADDRSTRLEN] = {0};
      (void)::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      b.source_ip = ip;
      // The port we HEARD it on, not one the frame advertises — an SDK v1
      // broadcast advertises no port at all.
      b.push_port_seen = s.port;
      b.t_last_seen_ns = now_ns();
      b.beacons_seen = 1;

      const std::string id = identity_of(b);
      auto it = found.find(id);
      if (it == found.end()) {
        found.emplace(id, b);
        SCAN_LOG_INFO(kMod, "found %s", b.describe().c_str());
      } else {
        // Every broadcast from one lidar carries the same 35 bytes, so
        // "latest wins" is trivially safe here — unlike the Mid-360, there
        // is no partial record a later frame could erase. Only the counter
        // and the timestamp actually change.
        const std::uint32_t seen = it->second.beacons_seen + 1;
        it->second = b;
        it->second.beacons_seen = seen;
      }
    }
  }

  for (const BoundSocket& s : socks) scan_close_socket(s.fd);

  std::vector<Mid70Beacon> out;
  out.reserve(found.size());
  for (auto& kv : found) out.push_back(std::move(kv.second));
  SCAN_LOG_INFO(kMod,
                "mid-70 discovery finished: %zu lidar(s), %llu datagram(s), %llu rejected",
                out.size(), static_cast<unsigned long long>(datagrams),
                static_cast<unsigned long long>(rejected));
  return out;
}

}  // namespace discovery
}  // namespace scanengine
