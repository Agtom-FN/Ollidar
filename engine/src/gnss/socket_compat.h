// socket_compat.h — the minimum BSD/Winsock TCP surface the NTRIP client
// needs. INTERNAL to src/gnss/; nothing outside this directory includes it.
//
// Why this is not `transport/udp_source.cpp`'s wrapper: that file is A3's and
// A10 may read it but not edit it. The two agree on the shape deliberately —
// one process-wide WSAStartup, `SO_RCVTIMEO` so a blocking receive still
// makes shutdown prompt, the same `SCAN_INVALID_SOCKET` / `scan_close_socket`
// spelling — so that folding them into one `transport/tcp_source.h` later is
// mechanical. What is genuinely new here and absent there: `getaddrinfo`
// name resolution (a caster is a hostname, a lidar is an IP) and a
// non-blocking `connect()` + `select()` so a dead caster fails in
// `connect_timeout_ms` instead of the OS's ~75 s SYN timeout.
#ifndef SCANENGINE_SRC_GNSS_SOCKET_COMPAT_H
#define SCANENGINE_SRC_GNSS_SOCKET_COMPAT_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
// windows.h's min/max macros break every std::min/std::max in this TU with
// "expected unqualified-id" (engine-ci #3) — suppress before any Win header.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using scan_socket_t = SOCKET;
#define SCAN_INVALID_SOCKET INVALID_SOCKET
#define scan_close_socket closesocket
#define scan_socket_errno WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using scan_socket_t = int;
#define SCAN_INVALID_SOCKET (-1)
#define scan_close_socket ::close
#define scan_socket_errno errno
#endif

namespace scanengine {
namespace gnss_net {

#if defined(_WIN32)
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    (void)WSAStartup(MAKEWORD(2, 2), &d);
  }
};
inline void ensure_winsock() {
  static WinsockInit once;
  (void)once;
}
#else
inline void ensure_winsock() {}
#endif

inline bool would_block() {
#if defined(_WIN32)
  const int e = WSAGetLastError();
  return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

inline bool connect_in_progress() {
#if defined(_WIN32)
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
  return errno == EINPROGRESS || errno == EINTR;
#endif
}

inline bool set_nonblocking(scan_socket_t fd, bool on) {
#if defined(_WIN32)
  u_long v = on ? 1u : 0u;
  return ::ioctlsocket(fd, FIONBIO, &v) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  const int want = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, want) == 0;
#endif
}

inline bool set_recv_timeout(scan_socket_t fd, int ms) {
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

inline bool set_send_timeout(scan_socket_t fd, int ms) {
#if defined(_WIN32)
  DWORD tv = static_cast<DWORD>(ms);
  return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                      sizeof(tv)) == 0;
#else
  struct timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

// Suppress SIGPIPE on a send to a closed peer. Linux has no SO_NOSIGPIPE and
// uses MSG_NOSIGNAL on the send() instead; macOS/BSD have the socket option.
inline void suppress_sigpipe(scan_socket_t fd) {
#if defined(SO_NOSIGPIPE)
  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)fd;
#endif
}

inline int send_flags() {
#if defined(MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

// Resolve + connect with a deadline. `err` gets a ScanError-worthy hint:
// 1 = resolve failed, 2 = socket()/connect() failed, 3 = timed out.
inline scan_socket_t tcp_connect(const std::string& host, std::uint16_t port,
                                 int timeout_ms, int* err) {
  ensure_winsock();
  if (err) *err = 0;

  char port_str[16];
  std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;  // v4 or v6: casters exist on both
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    if (err) *err = 1;
    return SCAN_INVALID_SOCKET;
  }

  scan_socket_t fd = SCAN_INVALID_SOCKET;
  int last = 2;
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == SCAN_INVALID_SOCKET) continue;
    suppress_sigpipe(fd);
    if (!set_nonblocking(fd, true)) {
      scan_close_socket(fd);
      fd = SCAN_INVALID_SOCKET;
      continue;
    }
    const int rc = ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (rc == 0) break;  // immediate (loopback)
    if (!connect_in_progress()) {
      scan_close_socket(fd);
      fd = SCAN_INVALID_SOCKET;
      last = 2;
      continue;
    }
    fd_set wset;
    FD_ZERO(&wset);
#if defined(_WIN32)
    FD_SET(fd, &wset);
#else
    FD_SET(static_cast<int>(fd), &wset);
#endif
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int sel = ::select(static_cast<int>(fd) + 1, nullptr, &wset, nullptr, &tv);
    if (sel <= 0) {
      scan_close_socket(fd);
      fd = SCAN_INVALID_SOCKET;
      last = 3;
      continue;
    }
    int soerr = 0;
#if defined(_WIN32)
    int len = sizeof(soerr);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
#else
    socklen_t len = sizeof(soerr);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
#endif
    if (soerr != 0) {
      scan_close_socket(fd);
      fd = SCAN_INVALID_SOCKET;
      last = 2;
      continue;
    }
    break;
  }
  ::freeaddrinfo(res);

  if (fd == SCAN_INVALID_SOCKET) {
    if (err) *err = last;
    return SCAN_INVALID_SOCKET;
  }
  set_nonblocking(fd, false);
  int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                     sizeof(one));
  return fd;
}

// Blocking send-all with the socket's send timeout. False on a short write
// that could not be completed.
inline bool send_all(scan_socket_t fd, const char* data, std::size_t n) {
  std::size_t sent = 0;
  while (sent < n) {
#if defined(_WIN32)
    const int r = ::send(fd, data + sent, static_cast<int>(n - sent), send_flags());
#else
    const ssize_t r = ::send(fd, data + sent, n - sent, send_flags());
#endif
    if (r > 0) {
      sent += static_cast<std::size_t>(r);
      continue;
    }
    if (r < 0 && would_block()) continue;
    return false;
  }
  return true;
}

}  // namespace gnss_net
}  // namespace scanengine

#endif  // SCANENGINE_SRC_GNSS_SOCKET_COMPAT_H
