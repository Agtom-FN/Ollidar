// net_compat.h — the minimum UDP + interface-enumeration surface A16 needs.
//
// INTERNAL to src/discovery/. Nothing outside this directory includes it.
//
// Why a third one of these (transport/udp_source.cpp has a UDP wrapper,
// src/gnss/socket_compat.h has a TCP one): both of those belong to another
// workstream and neither exposes a header. The three agree on
// spelling deliberately — one WSAStartup, SCAN_INVALID_SOCKET,
// scan_close_socket, scan_socket_errno — so folding them into one
// transport/net_compat.h later is mechanical. What is genuinely new here and
// absent from both: multi-socket select(), recvfrom() with the sender's
// address, and getifaddrs/GetAdaptersAddresses.
#ifndef SCANENGINE_SRC_DISCOVERY_NET_COMPAT_H
#define SCANENGINE_SRC_DISCOVERY_NET_COMPAT_H

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using scan_socket_t = SOCKET;
#define SCAN_INVALID_SOCKET INVALID_SOCKET
#define scan_close_socket closesocket
#define scan_socket_errno WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
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
namespace discovery_net {

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

// Bind a UDP socket to INADDR_ANY:port.
//
// ANY-BOUND IS NOT A DETAIL. The field session's finding, verbatim: "heartbeat
// broadcasts reach any-bound sockets only" on macOS/BSD. A socket bound to the
// interface address receives zero heartbeats even with the lidar two metres
// away on the same switch. Binding 0.0.0.0 is the whole reason discovery works.
//
// SO_REUSEADDR + SO_REUSEPORT so we can listen alongside Livox Viewer or the
// engine's own SDK2 backend instead of fighting them for the port. Where
// SO_REUSEPORT does not exist (Linux < 3.9, Windows — where SO_REUSEADDR
// already means "share"), we simply get the platform's behaviour.
inline scan_socket_t bind_any_udp(std::uint16_t port, int* out_errno) {
  ensure_winsock();
  scan_socket_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == SCAN_INVALID_SOCKET) {
    if (out_errno) *out_errno = scan_socket_errno;
    return SCAN_INVALID_SOCKET;
  }
  int one = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                     sizeof(one));
#if defined(SO_REUSEPORT)
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&one),
                     sizeof(one));
#endif
  (void)::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one),
                     sizeof(one));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // <- the field-proven part
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (out_errno) *out_errno = scan_socket_errno;
    scan_close_socket(fd);
    return SCAN_INVALID_SOCKET;
  }
  if (out_errno) *out_errno = 0;
  return fd;
}

}  // namespace discovery_net
}  // namespace scanengine

#endif  // SCANENGINE_SRC_DISCOVERY_NET_COMPAT_H
