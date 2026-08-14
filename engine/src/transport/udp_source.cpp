#include "scanengine/transport/udp_source.h"

#include <cstring>

#include "scanengine/core/log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using socklen_arg_t = int;
#define SCAN_INVALID_SOCKET INVALID_SOCKET
#define scan_close_socket closesocket
#define scan_socket_errno WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
using socklen_arg_t = socklen_t;
#define SCAN_INVALID_SOCKET (-1)
#define scan_close_socket ::close
#define scan_socket_errno errno
#endif

namespace scanengine {
namespace {

constexpr const char* kMod = "udp";

// The Mid-360's largest datagram is 1380 bytes; 2048 covers every stream it
// emits with room for a jumbo surprise, and anything bigger than the buffer
// is counted rather than silently truncated.
constexpr std::size_t kRecvBufBytes = 2048;

#if defined(_WIN32)
// One process-wide WSAStartup. The engine may be loaded into a host app that
// already did this — WSAStartup is refcounted, so an extra call is harmless
// and the matching cleanup is deliberately omitted (we do not know whether
// the host still needs Winsock when the last UdpSource dies).
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    (void)WSAStartup(MAKEWORD(2, 2), &d);
  }
};
void ensure_winsock() { static WinsockInit once; (void)once; }
#else
void ensure_winsock() {}
#endif

bool set_recv_timeout(socket_t fd, int ms) {
#if defined(_WIN32)
  DWORD tv = static_cast<DWORD>(ms);
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                      sizeof(tv)) == 0;
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

bool recv_would_block() {
#if defined(_WIN32)
  const int e = WSAGetLastError();
  return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

}  // namespace

UdpSource::UdpSource(const UdpConfig& cfg) : cfg_(cfg) { rx_buf_.resize(kRecvBufBytes); }

UdpSource::~UdpSource() { (void)stop(); }

std::uint16_t UdpSource::bound_port() const {
  std::lock_guard<std::mutex> lock(m_);
  return udp_stats_.bound_port;
}

Status UdpSource::start() {
  if (running_.load(std::memory_order_acquire)) return kOkStatus;
  if (!sink_) {
    return set_last_error(ScanError::kInvalidState,
                          "UdpSource::start(): set_sink() must be called first");
  }
  ensure_winsock();

  const std::uint16_t port = cfg_.bind_port != 0 ? cfg_.bind_port : cfg_.host_point_port;
  socket_t fd = SCAN_INVALID_SOCKET;
  bool owns = false;

  if (cfg_.prebound_fd >= 0) {
    // Android: the app bound this to the USB-Ethernet Network object for us
    // (the engine has no way to reach ConnectivityManager). We borrow it and
    // never close it.
    fd = static_cast<socket_t>(cfg_.prebound_fd);
  } else {
    fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == SCAN_INVALID_SOCKET) {
      return set_last_error(ScanError::kIoError, "UdpSource: socket() failed (errno %d)",
                            scan_socket_errno);
    }
    owns = true;

    int one = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                       sizeof(one));

    if (cfg_.recv_buffer_bytes > 0) {
      int want = cfg_.recv_buffer_bytes;
      (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&want),
                         sizeof(want));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Bind to the configured host IP when we have one, so a machine with
    // several interfaces receives the lidar's stream on the one the wizard
    // actually configured. INADDR_ANY otherwise.
    if (cfg_.host_ip.empty()) {
      addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
      addr.sin_addr.s_addr = ::inet_addr(cfg_.host_ip.c_str());
      if (addr.sin_addr.s_addr == INADDR_NONE) {
        scan_close_socket(fd);
        return set_last_error(ScanError::kInvalidArgument,
                             "UdpSource: host_ip '%s' is not a dotted-quad IPv4 address",
                             cfg_.host_ip.c_str());
      }
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      const int e = scan_socket_errno;
      scan_close_socket(fd);
      return set_last_error(ScanError::kPermissionDenied,
                            "UdpSource: bind(%s:%u) failed (errno %d)",
                            cfg_.host_ip.empty() ? "0.0.0.0" : cfg_.host_ip.c_str(),
                            static_cast<unsigned>(port), e);
    }
  }

  // A blocking recvfrom with a 100 ms timeout, rather than poll/select or a
  // self-pipe: it is the one construct that behaves identically on Darwin,
  // Linux, Android and Winsock, and 100 ms is the worst-case stop() latency.
  (void)set_recv_timeout(fd, 100);

  int granted = 0;
  socklen_arg_t glen = static_cast<socklen_arg_t>(sizeof(granted));
  (void)::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&granted), &glen);

  {
    std::lock_guard<std::mutex> lock(m_);
    udp_stats_ = UdpSourceStats{};
    udp_stats_.recv_buffer_granted = granted;
    udp_stats_.bound_port = port;
    stats_ = TransportStats{};
  }

  fd_ = static_cast<int>(fd);
  owns_fd_ = owns;
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  rx_ = std::thread(&UdpSource::receive_loop, this);

  SCAN_LOG_INFO(kMod, "listening on %s:%u (SO_RCVBUF %d bytes%s)",
                cfg_.host_ip.empty() ? "0.0.0.0" : cfg_.host_ip.c_str(),
                static_cast<unsigned>(port), granted, owns ? "" : ", app-provided fd");
  return kOkStatus;
}

Status UdpSource::stop() {
  if (!running_.load(std::memory_order_acquire)) return kOkStatus;
  stop_requested_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  if (rx_.joinable()) rx_.join();
  if (fd_ >= 0) {
    if (owns_fd_) scan_close_socket(static_cast<socket_t>(fd_));
    fd_ = -1;
  }
  owns_fd_ = false;
  return kOkStatus;
}

void UdpSource::receive_loop() {
  const socket_t fd = static_cast<socket_t>(fd_);
  while (!stop_requested_.load(std::memory_order_acquire)) {
    sockaddr_in from{};
    socklen_arg_t from_len = static_cast<socklen_arg_t>(sizeof(from));
#if defined(_WIN32)
    const int n = ::recvfrom(fd, reinterpret_cast<char*>(rx_buf_.data()),
                             static_cast<int>(rx_buf_.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
#else
    const ssize_t n = ::recvfrom(fd, rx_buf_.data(), rx_buf_.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
    if (n < 0) {
      if (recv_would_block()) continue;  // the 100 ms tick, or a signal
      std::lock_guard<std::mutex> lock(m_);
      ++udp_stats_.recv_errors;
      continue;
    }
    if (n == 0) continue;

    const std::size_t len = static_cast<std::size_t>(n);
    const TimePoint t = SteadyClock::now();
    {
      std::lock_guard<std::mutex> lock(m_);
      ++udp_stats_.datagrams;
      udp_stats_.bytes += len;
      if (len == rx_buf_.size()) ++udp_stats_.oversize;  // possibly truncated
      ++stats_.chunks_in;
      stats_.bytes_in += len;
      stats_.t_last_rx_ns = t.nanos;
    }
    // Outside the lock: the sink runs the whole decode path.
    sink_(ByteSpan(rx_buf_.data(), len), t);
  }
}

Status UdpSource::send(ByteSpan datagram) {
  if (!running_.load(std::memory_order_acquire) || fd_ < 0) {
    return set_last_error(ScanError::kInvalidState, "UdpSource::send(): not started");
  }
  if (cfg_.lidar_ip.empty()) {
    return set_last_error(ScanError::kInvalidArgument,
                          "UdpSource::send(): no lidar_ip configured (explicit IP is required "
                          "on every platform, and mandatory on macOS — there is no broadcast "
                          "discovery there)");
  }
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(cfg_.cmd_port);
  to.sin_addr.s_addr = ::inet_addr(cfg_.lidar_ip.c_str());
  if (to.sin_addr.s_addr == INADDR_NONE) {
    return set_last_error(ScanError::kInvalidArgument, "UdpSource::send(): bad lidar_ip '%s'",
                          cfg_.lidar_ip.c_str());
  }
#if defined(_WIN32)
  const int n = ::sendto(static_cast<socket_t>(fd_),
                         reinterpret_cast<const char*>(datagram.data()),
                         static_cast<int>(datagram.size()), 0,
                         reinterpret_cast<sockaddr*>(&to), sizeof(to));
#else
  const ssize_t n = ::sendto(fd_, datagram.data(), datagram.size(), 0,
                             reinterpret_cast<sockaddr*>(&to), sizeof(to));
#endif
  if (n < 0) {
    std::lock_guard<std::mutex> lock(m_);
    ++stats_.write_errors;
    return set_last_error(ScanError::kNetworkError, "UdpSource::send(): sendto failed (errno %d)",
                          scan_socket_errno);
  }
  std::lock_guard<std::mutex> lock(m_);
  stats_.bytes_out += static_cast<std::uint64_t>(n);
  return kOkStatus;
}

TransportStats UdpSource::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return stats_;
}

UdpSourceStats UdpSource::udp_stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return udp_stats_;
}

}  // namespace scanengine
