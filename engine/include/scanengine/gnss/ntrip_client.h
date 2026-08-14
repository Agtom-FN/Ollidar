// ntrip_client.h — NTRIP 1.0/2.0 client: caster → RTCM3 → rover (§3.4).
//
// "NTRIP client in engine: caster connect, mountpoint list, RTCM3 forward to
// rover; reconnect logic; corrections-age surfaced."
//
// ### Why the engine owns this and not the app
//
// It is the only network client in the product that has to survive a walk:
// the operator carries a phone through a street canyon, LTE drops for eight
// seconds, and the RTK solution must come back Fixed without anybody
// touching the UI. That reconnect policy is the same on Android and on the
// Qt desktop, and DESIGN §3 key rule 1 already puts "opaque bytes in, status
// out" at the engine boundary. The app supplies exactly one thing the engine
// cannot have: the write function to the rover's Bluetooth socket.
//
// ### Shape
//
//   connect(cfg)            resolves, opens TCP, does the HTTP/ICY handshake
//                           SYNCHRONOUSLY, so a 401 or an unknown mountpoint
//                           is an immediate kPermissionDenied / kNotFound
//                           rather than an infinite reconnect loop. On
//                           success it hands the socket to one worker thread.
//   worker thread           recv → Rtcm3Framer → whole valid frames to the
//                           rover callback; periodic GGA upload; stall
//                           detection; reconnect with exponential backoff.
//   disconnect()            joins. Safe to call from any thread, twice, or
//                           on an object that never connected.
//
// ### Threading (DESIGN §2 — one new engine-owned thread)
//
// | Thread | Owner | What runs on it |
// | NTRIP receive | TcpNtripClient | blocking recv with a 1 s socket timeout
//   so disconnect() is prompt; RTCM framing; the rover-write callback; GGA
//   upload; reconnect/backoff. One per client. |
//
// The rover callback runs ON THIS THREAD and must be quick and must not
// re-enter the client (the general DESIGN §2 callback rule). It is where the
// app does `bluetoothSocket.write(bytes)`.
//
// ### Sockets
//
// `src/gnss/socket_compat.h` is a LOCAL BSD/Winsock wrapper, deliberately not
// an extension of `transport/udp_source.cpp`'s: that file is A3's and this
// task may read it but not edit it. The two share an approach (one
// `WSAStartup`, `SO_RCVTIMEO` for prompt shutdown, `SCAN_INVALID_SOCKET`
// spelling) so that a later consolidation is mechanical. What is different is
// genuinely different: TCP with a non-blocking `connect()` + `select()`
// timeout, and `getaddrinfo` name resolution, neither of which UDP needed.
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_NTRIP_CLIENT_H
#define SCANENGINE_GNSS_NTRIP_CLIENT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/gnss/rtcm3.h"

namespace scanengine {

enum class NtripState : std::uint8_t {
  kIdle = 0,
  kConnecting = 1,
  kStreaming = 2,
  kStalled = 3,        // socket open, no RTCM for stall_timeout_ms
  kReconnecting = 4,
  kFailed = 5,         // terminal: auth, bad mountpoint, or attempts exhausted
};

const char* to_string(NtripState s) noexcept;

// One parsed sourcetable STR record. The fields are the NTRIP 1.0 Appendix B
// order; anything the caster left blank stays empty/zero rather than being
// invented, and `raw` keeps the original line for a UI that wants to show
// what the caster actually said.
struct NtripSource {
  std::string mountpoint;
  std::string identifier;
  std::string format;          // "RTCM 3.3"
  std::string format_details;  // "1005(1),1077(1),…"
  int carrier = 0;             // 0 none, 1 L1, 2 L1+L2
  std::string nav_system;      // "GPS+GLO+GAL+BDS"
  std::string network;
  std::string country;         // ISO 3166 3-letter
  double lat_deg = 0.0, lon_deg = 0.0;
  bool needs_gga = false;      // "nmea" flag: a VRS mount that wants our GGA
  int solution = 0;            // 0 single base, 1 network
  std::string generator;
  std::string compression;
  std::string authentication;  // "N" none, "B" basic, "D" digest
  bool fee = false;
  int bitrate = 0;
  std::string misc;
  std::string raw;

  // Great-circle distance from a position, km. What a mountpoint picker
  // sorts on — RTK baseline length is the dominant term in Float-vs-Fixed.
  double distance_km(double lat_deg_, double lon_deg_) const;
};

struct NtripStats {
  std::uint64_t connect_attempts = 0;
  std::uint64_t connects_ok = 0;
  std::uint64_t disconnects = 0;       // link lost while streaming
  std::uint64_t reconnects = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t gga_sent = 0;
  std::uint64_t gga_bytes = 0;
  std::uint64_t stalls = 0;
  std::uint64_t handshake_failures = 0;
  std::uint64_t socket_failures = 0;
  std::int64_t t_connected_ns = 0;     // engine time of the current connection
  std::int64_t t_last_rtcm_ns = 0;
  std::int32_t backoff_ms = 0;         // what the next reconnect will wait
  int http_status = 0;                 // last handshake status (200/401/404/0)
  int ntrip_version_used = 0;          // 2 or 1 after fallback
  ScanError last_error = ScanError::kOk;
  rtcm3::Rtcm3Stats rtcm{};
};

class TcpNtripClient final : public NtripClient {
 public:
  TcpNtripClient();
  ~TcpNtripClient() override;

  TcpNtripClient(const TcpNtripClient&) = delete;
  TcpNtripClient& operator=(const TcpNtripClient&) = delete;

  // --- NtripClient ------------------------------------------------------
  //
  // kInvalidArgument  empty host/mountpoint
  // kInvalidState     already connected
  // kNetworkError     DNS/connect/socket
  // kTimeout          connect_timeout_ms elapsed
  // kPermissionDenied 401, or v1 "ERROR - Bad Password"
  // kNotFound         404, or v1 "ERROR - Bad Mountpoint"
  // kProtocolError    a response that is neither
  Status connect(const NtripConfig& cfg) override;
  Status disconnect() override;
  Status list_mountpoints(std::vector<std::string>* out) override;
  void set_rtcm_callback(void (*cb)(ByteSpan rtcm, void* user), void* user) override;
  float correction_age_s() const override;

  // --- A10 surface ------------------------------------------------------

  // Richer sourcetable fetch: a separate short-lived connection, so it works
  // before connect() and while streaming. Uses `cfg.host`/`cfg.port` only.
  Status fetch_sourcetable(const NtripConfig& cfg, std::vector<NtripSource>* out);

  // std::function form of the rover write. Prefer this in C++; the C-style
  // pair above exists because `NtripClient` is A1's interface and the C ABI
  // will mirror it. Setting one clears the other.
  using RtcmHandler = std::function<void(ByteSpan rtcm)>;
  void set_rtcm_handler(RtcmHandler h);

  // Where the GGA upload comes from. First choice: the rover's own last GGA
  // (`GnssSource::last_gga_sentence`), so the caster sees the receiver's own
  // words. Return false and nothing is sent for that tick.
  using GgaProvider = std::function<bool(std::string* out)>;
  void set_gga_provider(GgaProvider p);

  // Fallback when there is no provider: a synthesized GGA from the last
  // position handed in here. Also what a "connect to the caster before the
  // rover has a fix" flow uses, seeded from the project's approximate site.
  void set_position(double lat_deg, double lon_deg, double alt_msl_m,
                    FixType fix = FixType::kSingle, int satellites = 8,
                    double hdop = 1.0);

  // State transitions, on the worker thread (and on the caller's thread for
  // the initial connect()). For B9's status strip.
  using StateCallback = std::function<void(NtripState, ScanError)>;
  void set_state_callback(StateCallback cb);

  NtripState state() const;
  NtripStats stats() const;
  const NtripConfig& config() const;

  // True once at least one CRC-valid RTCM frame has arrived on the CURRENT
  // connection. "Connected" is not the same as "receiving corrections", and
  // the UI must not claim the latter from the former.
  bool receiving() const;

  // Sourcetable parsing, exposed because it is pure and worth testing on
  // captured caster output without a socket.
  static std::vector<NtripSource> parse_sourcetable(const std::string& body);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_GNSS_NTRIP_CLIENT_H
