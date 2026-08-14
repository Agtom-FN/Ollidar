#include "scanengine/gnss/ntrip_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scanengine/core/log.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/nmea.h"
#include "scanengine/timesync/clock.h"
#include "socket_compat.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "ntrip";

std::string base64(const std::string& in) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 2 < in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8) |
                       static_cast<unsigned char>(in[i + 2]);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back(tbl[v & 63]);
    i += 3;
  }
  if (i + 1 == in.size()) {
    const unsigned v = static_cast<unsigned char>(in[i]) << 16;
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.append("==");
  } else if (i + 2 == in.size()) {
    const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                       (static_cast<unsigned char>(in[i + 1]) << 8);
    out.push_back(tbl[(v >> 18) & 63]);
    out.push_back(tbl[(v >> 12) & 63]);
    out.push_back(tbl[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::int64_t now_ns() { return SteadyClock::now().nanos; }

bool starts_with(const std::string& s, const char* p) {
  const std::size_t n = std::strlen(p);
  return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

// How the caster answered. Deliberately a small closed set: everything the
// client does next (retry, give up, fall back to v1) hangs off it.
enum class Handshake {
  kOk,
  kUnauthorized,
  kNotFound,
  kProtocol,     // a response we could not classify
  kSourcetable,  // caster answered a stream request with its sourcetable
};

}  // namespace

const char* to_string(NtripState s) noexcept {
  switch (s) {
    case NtripState::kIdle: return "idle";
    case NtripState::kConnecting: return "connecting";
    case NtripState::kStreaming: return "streaming";
    case NtripState::kStalled: return "stalled";
    case NtripState::kReconnecting: return "reconnecting";
    case NtripState::kFailed: return "failed";
  }
  return "?";
}

double NtripSource::distance_km(double lat_deg_, double lon_deg_) const {
  // Spherical haversine: a mountpoint picker sorts on this, and the ellipsoid
  // correction is far below the granularity that matters for "which base is
  // nearest".
  constexpr double kR = 6371.0088;
  const double p1 = lat_deg * crs::kDeg, p2 = lat_deg_ * crs::kDeg;
  const double dp = (lat_deg_ - lat_deg) * crs::kDeg;
  const double dl = (lon_deg_ - lon_deg) * crs::kDeg;
  const double a = std::sin(dp / 2) * std::sin(dp / 2) +
                   std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
  return 2.0 * kR * std::asin(std::min(1.0, std::sqrt(a)));
}

// ===========================================================================

struct TcpNtripClient::Impl {
  mutable std::mutex m;
  std::condition_variable cv;

  NtripConfig cfg;
  NtripState state = NtripState::kIdle;
  NtripStats stats{};
  bool receiving = false;
  bool stop = false;
  bool thread_running = false;

  scan_socket_t fd = SCAN_INVALID_SOCKET;
  std::thread th;

  rtcm3::Rtcm3Framer framer;
  RtcmHandler handler;
  void (*c_cb)(ByteSpan, void*) = nullptr;
  void* c_user = nullptr;
  GgaProvider gga_provider;
  StateCallback state_cb;

  bool have_pos = false;
  double lat = 0.0, lon = 0.0, alt = 0.0;
  FixType pos_fix = FixType::kSingle;
  int pos_sats = 8;
  double pos_hdop = 1.0;

  std::int32_t backoff_ms = 0;
  int attempts = 0;

  Impl() {
    framer.set_handler([this](ByteSpan frame, const rtcm3::FrameInfo&, std::int64_t t) {
      forward_(frame, t);
    });
  }

  // Called on the worker thread, with `m` NOT held: the app's rover write is
  // a Bluetooth socket write and must not be able to block the client's own
  // state (DESIGN §2's callback rule).
  void forward_(ByteSpan frame, std::int64_t t) {
    RtcmHandler h;
    void (*c)(ByteSpan, void*) = nullptr;
    void* u = nullptr;
    {
      std::lock_guard<std::mutex> lock(m);
      stats.t_last_rtcm_ns = t;
      stats.rtcm = framer.stats();
      receiving = true;
      h = handler;
      c = c_cb;
      u = c_user;
    }
    if (h) h(frame);
    if (c) c(frame, u);
  }

  void set_state_(NtripState s, ScanError e) {
    StateCallback cb;
    {
      std::lock_guard<std::mutex> lock(m);
      if (state == s && stats.last_error == e) return;
      state = s;
      stats.last_error = e;
      cb = state_cb;
    }
    if (cb) cb(s, e);
  }

  std::string build_gga_() {
    GgaProvider p;
    bool hp;
    double la, lo, al, hd;
    FixType fx;
    int sa;
    {
      std::lock_guard<std::mutex> lock(m);
      p = gga_provider;
      hp = have_pos;
      la = lat; lo = lon; al = alt; hd = pos_hdop;
      fx = pos_fix; sa = pos_sats;
    }
    if (p) {
      std::string s;
      if (p(&s) && !s.empty()) {
        if (s.size() < 2 || s[s.size() - 1] != '\n') s += "\r\n";
        return s;
      }
    }
    if (!hp) return std::string();
    nmea::GgaBuilderInput in;
    // The caster only uses the position; a synthetic time-of-day is honest
    // (it is OUR clock, not the receiver's) and every caster tolerates it.
    const std::int64_t t = now_ns();
    in.utc_sod_s = static_cast<double>((t / 1000000000LL) % 86400LL);
    in.lat_deg = la;
    in.lon_deg = lo;
    in.alt_msl_m = al;
    in.quality = fx == FixType::kRtkFixed   ? 4
                 : fx == FixType::kRtkFloat ? 5
                 : fx == FixType::kDgps     ? 2
                 : fx == FixType::kSingle   ? 1
                                            : 0;
    in.satellites = sa;
    in.hdop = hd;
    return nmea::build_gga(in);
  }

  std::string request_(const NtripConfig& c, int version, const std::string& path) {
    std::string req = "GET " + path + (version >= 2 ? " HTTP/1.1\r\n" : " HTTP/1.0\r\n");
    if (version >= 2) {
      char hostline[320];
      std::snprintf(hostline, sizeof(hostline), "Host: %s:%u\r\n", c.host.c_str(),
                    static_cast<unsigned>(c.port));
      req += hostline;
      req += "Ntrip-Version: Ntrip/2.0\r\n";
    }
    // The NTRIP standard requires the User-Agent to start with "NTRIP";
    // several public casters reject anything else with a 400.
    req += "User-Agent: " +
           (starts_with(c.user_agent, "NTRIP") ? c.user_agent
                                               : ("NTRIP " + c.user_agent)) +
           "\r\n";
    if (!c.username.empty() || !c.password.empty()) {
      req += "Authorization: Basic " + base64(c.username + ":" + c.password) + "\r\n";
    }
    req += "Accept: */*\r\n";
    req += "Connection: close\r\n\r\n";
    return req;
  }

  // Reads the response header, classifies it, and returns any bytes that
  // arrived after it (the caster starts streaming RTCM immediately, so those
  // bytes are real corrections and must not be dropped).
  Handshake read_response_(scan_socket_t s, int timeout_ms, std::string* leftover,
                           int* http_status) {
    std::string buf;
    const std::int64_t deadline = now_ns() + static_cast<std::int64_t>(timeout_ms) * 1000000LL;
    char tmp[1024];
    std::size_t header_end = std::string::npos;
    while (now_ns() < deadline) {
#if defined(_WIN32)
      const int r = ::recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
      const ssize_t r = ::recv(s, tmp, sizeof(tmp), 0);
#endif
      if (r > 0) {
        buf.append(tmp, static_cast<std::size_t>(r));
      } else if (r == 0) {
        break;  // caster closed: whatever we have is the whole answer
      } else if (!gnss_net::would_block()) {
        break;
      }
      const std::size_t p = buf.find("\r\n\r\n");
      if (p != std::string::npos) {
        header_end = p + 4;
        break;
      }
      // NTRIP v1's "ICY 200 OK" is sometimes followed by ONE CRLF and then
      // straight into binary RTCM. Detect that: a 0xD3 in the first 64 bytes
      // after a lone CRLF means the header is already over.
      if (starts_with(buf, "ICY 200 OK")) {
        const std::size_t nl = buf.find("\r\n");
        if (nl != std::string::npos && buf.size() > nl + 2 &&
            static_cast<unsigned char>(buf[nl + 2]) == rtcm3::kPreamble) {
          header_end = nl + 2;
          break;
        }
      }
      if (buf.size() > 65536) break;
    }

    const std::string head = buf.substr(0, header_end == std::string::npos ? buf.size()
                                                                           : header_end);
    if (header_end != std::string::npos && leftover) *leftover = buf.substr(header_end);

    if (starts_with(head, "ICY 200 OK")) {
      if (http_status) *http_status = 200;
      return Handshake::kOk;
    }
    if (starts_with(head, "SOURCETABLE")) {
      if (http_status) *http_status = 200;
      return Handshake::kSourcetable;
    }
    if (starts_with(head, "HTTP/1.")) {
      int code = 0;
      const std::size_t sp = head.find(' ');
      if (sp != std::string::npos) code = std::atoi(head.c_str() + sp + 1);
      if (http_status) *http_status = code;
      if (code == 200) return Handshake::kOk;
      if (code == 401 || code == 403) return Handshake::kUnauthorized;
      if (code == 404) return Handshake::kNotFound;
      return Handshake::kProtocol;
    }
    // NTRIP v1 plaintext errors.
    if (head.find("Bad Password") != std::string::npos ||
        head.find("Bad Username") != std::string::npos) {
      if (http_status) *http_status = 401;
      return Handshake::kUnauthorized;
    }
    if (head.find("Bad Mountpoint") != std::string::npos ||
        head.find("Mount Point") != std::string::npos) {
      if (http_status) *http_status = 404;
      return Handshake::kNotFound;
    }
    if (http_status) *http_status = 0;
    return Handshake::kProtocol;
  }

  // One full connect + handshake. Returns the socket on success.
  scan_socket_t connect_once_(const NtripConfig& c, ScanError* err, std::string* leftover,
                              int* version_used) {
    *err = ScanError::kOk;
    int neterr = 0;
    {
      std::lock_guard<std::mutex> lock(m);
      ++stats.connect_attempts;
    }
    scan_socket_t s = gnss_net::tcp_connect(c.host, c.port, c.connect_timeout_ms, &neterr);
    if (s == SCAN_INVALID_SOCKET) {
      std::lock_guard<std::mutex> lock(m);
      ++stats.socket_failures;
      *err = (neterr == 3) ? ScanError::kTimeout : ScanError::kNetworkError;
      return SCAN_INVALID_SOCKET;
    }
    gnss_net::set_recv_timeout(s, std::max(100, c.read_timeout_ms));
    gnss_net::set_send_timeout(s, std::max(1000, c.connect_timeout_ms));

    const std::string path = "/" + c.mountpoint;
    int version = c.ntrip_version >= 2 ? 2 : 1;
    for (int attempt = 0; attempt < 2; ++attempt) {
      const std::string req = request_(c, version, path);
      if (!gnss_net::send_all(s, req.data(), req.size())) {
        scan_close_socket(s);
        std::lock_guard<std::mutex> lock(m);
        ++stats.socket_failures;
        *err = ScanError::kNetworkError;
        return SCAN_INVALID_SOCKET;
      }
      int status = 0;
      leftover->clear();
      const Handshake h = read_response_(s, c.connect_timeout_ms, leftover, &status);
      {
        std::lock_guard<std::mutex> lock(m);
        stats.http_status = status;
      }
      if (h == Handshake::kOk) {
        if (version_used) *version_used = version;
        return s;
      }
      // A v2 request that a v1-only caster did not understand: reconnect and
      // try the ICY form. RTK2go and most community casters need this.
      if (version == 2 && c.allow_v1_fallback &&
          (h == Handshake::kProtocol || h == Handshake::kSourcetable)) {
        scan_close_socket(s);
        s = gnss_net::tcp_connect(c.host, c.port, c.connect_timeout_ms, &neterr);
        if (s == SCAN_INVALID_SOCKET) {
          std::lock_guard<std::mutex> lock(m);
          ++stats.socket_failures;
          *err = ScanError::kNetworkError;
          return SCAN_INVALID_SOCKET;
        }
        gnss_net::set_recv_timeout(s, std::max(100, c.read_timeout_ms));
        gnss_net::set_send_timeout(s, std::max(1000, c.connect_timeout_ms));
        version = 1;
        continue;
      }
      scan_close_socket(s);
      std::lock_guard<std::mutex> lock(m);
      ++stats.handshake_failures;
      switch (h) {
        case Handshake::kUnauthorized: *err = ScanError::kPermissionDenied; break;
        case Handshake::kNotFound:
        case Handshake::kSourcetable: *err = ScanError::kNotFound; break;
        default: *err = ScanError::kProtocolError; break;
      }
      return SCAN_INVALID_SOCKET;
    }
    scan_close_socket(s);
    *err = ScanError::kProtocolError;
    return SCAN_INVALID_SOCKET;
  }

  bool sleep_backoff_(std::int32_t ms) {
    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stop; });
    return !stop;
  }

  void run_() {
    NtripConfig c;
    {
      std::lock_guard<std::mutex> lock(m);
      c = cfg;
    }
    std::int64_t t_last_gga = 0;
    bool gga_pending = c.send_gga_on_connect;

    for (;;) {
      {
        std::lock_guard<std::mutex> lock(m);
        if (stop) break;
      }

      scan_socket_t s;
      {
        std::lock_guard<std::mutex> lock(m);
        s = fd;
      }

      if (s == SCAN_INVALID_SOCKET) {
        if (!c.auto_reconnect) break;
        set_state_(NtripState::kReconnecting, ScanError::kOk);
        std::int32_t wait_ms;
        {
          std::lock_guard<std::mutex> lock(m);
          if (backoff_ms <= 0) backoff_ms = c.reconnect_initial_ms;
          // ±25 % jitter: a fleet of rovers that all lost LTE in the same
          // tunnel must not hammer the caster in lockstep. Deterministic
          // pseudo-jitter from the attempt count — no <random>, whose
          // distributions are not specified across the five CI legs.
          const int j = static_cast<int>(
              ((static_cast<unsigned>(attempts) * 2654435761u) >> 24) % 51u);  // 0..50
          wait_ms = backoff_ms + backoff_ms * (j - 25) / 100;
          stats.backoff_ms = backoff_ms;
          ++attempts;
          if (c.max_reconnect_attempts > 0 && attempts > c.max_reconnect_attempts) {
            stats.last_error = ScanError::kDisconnected;
            wait_ms = -1;
          }
        }
        if (wait_ms < 0) {
          set_state_(NtripState::kFailed, ScanError::kDisconnected);
          break;
        }
        if (!sleep_backoff_(wait_ms)) break;

        ScanError err = ScanError::kOk;
        std::string leftover;
        int version = 0;
        set_state_(NtripState::kConnecting, ScanError::kOk);
        scan_socket_t ns = connect_once_(c, &err, &leftover, &version);
        if (ns == SCAN_INVALID_SOCKET) {
          if (err == ScanError::kPermissionDenied || err == ScanError::kNotFound) {
            SCAN_LOG_ERROR(kMod, "reconnect refused permanently: %s", error_str(err));
            set_state_(NtripState::kFailed, err);
            break;
          }
          std::lock_guard<std::mutex> lock(m);
          backoff_ms = std::min(backoff_ms * 2, c.reconnect_max_ms);
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(m);
          fd = ns;
          backoff_ms = c.reconnect_initial_ms;
          attempts = 0;
          receiving = false;
          ++stats.connects_ok;
          ++stats.reconnects;
          stats.t_connected_ns = now_ns();
          stats.t_last_rtcm_ns = stats.t_connected_ns;
          stats.ntrip_version_used = version;
        }
        // Buffer only: the session's correction counters must survive a
        // reconnect, or "we received 60 frames" becomes "we received 35"
        // the moment the caster hiccups.
        framer.clear_buffer();
        if (!leftover.empty()) {
          framer.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(leftover.data()),
                               leftover.size()),
                      now_ns());
        }
        gga_pending = c.send_gga_on_connect;
        set_state_(NtripState::kStreaming, ScanError::kOk);
        continue;
      }

      // ---- streaming ----------------------------------------------------
      char buf[4096];
#if defined(_WIN32)
      const int r = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
      const ssize_t r = ::recv(s, buf, sizeof(buf), 0);
#endif
      const std::int64_t t = now_ns();
      if (r > 0) {
        {
          std::lock_guard<std::mutex> lock(m);
          stats.bytes_received += static_cast<std::uint64_t>(r);
        }
        framer.push(ByteSpan(reinterpret_cast<const std::uint8_t*>(buf),
                             static_cast<std::size_t>(r)),
                    t);
        {
          std::lock_guard<std::mutex> lock(m);
          stats.rtcm = framer.stats();
        }
        set_state_(NtripState::kStreaming, ScanError::kOk);
      } else if (r == 0) {
        SCAN_LOG_WARN(kMod, "caster closed the stream after %llu bytes",
                      static_cast<unsigned long long>(stats.bytes_received));
        std::lock_guard<std::mutex> lock(m);
        scan_close_socket(s);
        fd = SCAN_INVALID_SOCKET;
        ++stats.disconnects;
        receiving = false;
        continue;
      } else if (!gnss_net::would_block()) {
        SCAN_LOG_WARN(kMod, "recv failed (errno %d)", scan_socket_errno);
        std::lock_guard<std::mutex> lock(m);
        scan_close_socket(s);
        fd = SCAN_INVALID_SOCKET;
        ++stats.disconnects;
        ++stats.socket_failures;
        receiving = false;
        continue;
      }

      // ---- stall detection ----------------------------------------------
      {
        bool stalled = false;
        {
          std::lock_guard<std::mutex> lock(m);
          if (c.stall_timeout_ms > 0 &&
              t - stats.t_last_rtcm_ns >
                  static_cast<std::int64_t>(c.stall_timeout_ms) * 1000000LL) {
            stalled = true;
            ++stats.stalls;
          }
        }
        if (stalled) {
          SCAN_LOG_WARN(kMod, "no corrections for %d ms — dropping the connection",
                        c.stall_timeout_ms);
          set_state_(NtripState::kStalled, ScanError::kTimeout);
          std::lock_guard<std::mutex> lock(m);
          scan_close_socket(s);
          fd = SCAN_INVALID_SOCKET;
          ++stats.disconnects;
          receiving = false;
          continue;
        }
      }

      // ---- GGA upload -----------------------------------------------------
      if (c.gga_interval_ms > 0 &&
          (gga_pending ||
           t - t_last_gga >= static_cast<std::int64_t>(c.gga_interval_ms) * 1000000LL)) {
        const std::string gga = build_gga_();
        if (!gga.empty()) {
          if (gnss_net::send_all(s, gga.data(), gga.size())) {
            std::lock_guard<std::mutex> lock(m);
            ++stats.gga_sent;
            stats.gga_bytes += gga.size();
          }
          t_last_gga = t;
          gga_pending = false;
        } else if (gga_pending) {
          // Nothing to send yet (no fix, no seeded position). Retry on the
          // next tick rather than burning the on-connect upload.
          t_last_gga = t;
        }
      }
    }

    std::lock_guard<std::mutex> lock(m);
    if (fd != SCAN_INVALID_SOCKET) {
      scan_close_socket(fd);
      fd = SCAN_INVALID_SOCKET;
    }
    thread_running = false;
  }
};

// ===========================================================================

TcpNtripClient::TcpNtripClient() : impl_(new Impl()) {}

TcpNtripClient::~TcpNtripClient() { (void)disconnect(); }

Status TcpNtripClient::connect(const NtripConfig& cfg) {
  if (cfg.host.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "NtripClient::connect: empty host");
  }
  if (cfg.mountpoint.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "NtripClient::connect: empty mountpoint");
  }
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->thread_running || impl_->fd != SCAN_INVALID_SOCKET) {
      return set_last_error(ScanError::kInvalidState, "NtripClient: already connected");
    }
    impl_->cfg = cfg;
    impl_->stop = false;
    impl_->stats = NtripStats{};
    impl_->backoff_ms = cfg.reconnect_initial_ms;
    impl_->attempts = 0;
    impl_->receiving = false;
  }
  impl_->framer.reset();
  impl_->set_state_(NtripState::kConnecting, ScanError::kOk);

  // The FIRST handshake is synchronous: a wrong password or a mountpoint
  // that does not exist must surface as an error from connect(), not as an
  // infinite reconnect loop with a UI that says "connecting…" forever.
  ScanError err = ScanError::kOk;
  std::string leftover;
  int version = 0;
  const scan_socket_t s = impl_->connect_once_(cfg, &err, &leftover, &version);
  if (s == SCAN_INVALID_SOCKET) {
    impl_->set_state_(NtripState::kFailed, err);
    return set_last_error(err, "NtripClient: connect to %s:%u/%s failed (%s, http %d)",
                          cfg.host.c_str(), static_cast<unsigned>(cfg.port),
                          cfg.mountpoint.c_str(), error_str(err), impl_->stats.http_status);
  }
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->fd = s;
    ++impl_->stats.connects_ok;
    impl_->stats.t_connected_ns = now_ns();
    impl_->stats.t_last_rtcm_ns = impl_->stats.t_connected_ns;
    impl_->stats.ntrip_version_used = version;
    impl_->thread_running = true;
  }
  if (!leftover.empty()) {
    impl_->framer.push(
        ByteSpan(reinterpret_cast<const std::uint8_t*>(leftover.data()), leftover.size()),
        now_ns());
  }
  impl_->set_state_(NtripState::kStreaming, ScanError::kOk);
  impl_->th = std::thread([this] { impl_->run_(); });
  SCAN_LOG_INFO(kMod, "connected to %s:%u/%s (NTRIP v%d)", cfg.host.c_str(),
                static_cast<unsigned>(cfg.port), cfg.mountpoint.c_str(), version);
  return kOkStatus;
}

Status TcpNtripClient::disconnect() {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->stop = true;
  }
  impl_->cv.notify_all();
  if (impl_->th.joinable()) impl_->th.join();
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->fd != SCAN_INVALID_SOCKET) {
      scan_close_socket(impl_->fd);
      impl_->fd = SCAN_INVALID_SOCKET;
    }
    impl_->thread_running = false;
    impl_->receiving = false;
  }
  impl_->set_state_(NtripState::kIdle, ScanError::kOk);
  return kOkStatus;
}

void TcpNtripClient::set_rtcm_callback(void (*cb)(ByteSpan, void*), void* user) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->c_cb = cb;
  impl_->c_user = user;
  impl_->handler = nullptr;
}

void TcpNtripClient::set_rtcm_handler(RtcmHandler h) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->handler = std::move(h);
  impl_->c_cb = nullptr;
  impl_->c_user = nullptr;
}

void TcpNtripClient::set_gga_provider(GgaProvider p) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->gga_provider = std::move(p);
}

void TcpNtripClient::set_position(double lat_deg, double lon_deg, double alt_msl_m,
                                  FixType fix, int satellites, double hdop) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->have_pos = true;
  impl_->lat = lat_deg;
  impl_->lon = lon_deg;
  impl_->alt = alt_msl_m;
  impl_->pos_fix = fix;
  impl_->pos_sats = satellites;
  impl_->pos_hdop = hdop;
}

void TcpNtripClient::set_state_callback(StateCallback cb) {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->state_cb = std::move(cb);
}

NtripState TcpNtripClient::state() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->state;
}

NtripStats TcpNtripClient::stats() const {
  // Deliberately NOT `impl_->framer.stats()`: the framer is owned by the
  // receive thread and is not thread-safe. Its stats are copied into
  // `impl_->stats.rtcm` under the mutex after every push, so this snapshot is
  // consistent and race-free.
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->stats;
}

const NtripConfig& TcpNtripClient::config() const { return impl_->cfg; }

bool TcpNtripClient::receiving() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->receiving;
}

float TcpNtripClient::correction_age_s() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  // -1 until the first CRC-valid frame: "unknown" and "zero seconds old" are
  // not the same claim, and a UI that shows 0.0 s before any corrections have
  // arrived is lying in the most reassuring possible direction.
  if (impl_->stats.rtcm.frames_ok == 0) return -1.0f;
  const std::int64_t d = now_ns() - impl_->stats.t_last_rtcm_ns;
  return d > 0 ? static_cast<float>(static_cast<double>(d) * 1e-9) : 0.0f;
}

// --- sourcetable -----------------------------------------------------------

std::vector<NtripSource> TcpNtripClient::parse_sourcetable(const std::string& body) {
  std::vector<NtripSource> out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    std::size_t nl = body.find('\n', pos);
    std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? body.size() : nl + 1;
    line = trim(line);
    if (line == "ENDSOURCETABLE") break;
    if (!starts_with(line, "STR;")) continue;

    std::vector<std::string> f;
    std::size_t a = 0;
    while (a <= line.size()) {
      const std::size_t b = line.find(';', a);
      f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    NtripSource s;
    s.raw = line;
    auto at = [&f](std::size_t i) -> std::string { return i < f.size() ? f[i] : std::string(); };
    s.mountpoint = at(1);
    s.identifier = at(2);
    s.format = at(3);
    s.format_details = at(4);
    s.carrier = std::atoi(at(5).c_str());
    s.nav_system = at(6);
    s.network = at(7);
    s.country = at(8);
    s.lat_deg = std::atof(at(9).c_str());
    s.lon_deg = std::atof(at(10).c_str());
    s.needs_gga = at(11) == "1";
    s.solution = std::atoi(at(12).c_str());
    s.generator = at(13);
    s.compression = at(14);
    s.authentication = at(15);
    s.fee = (at(16) == "Y" || at(16) == "y");
    s.bitrate = std::atoi(at(17).c_str());
    s.misc = at(18);
    if (!s.mountpoint.empty()) out.push_back(s);
  }
  return out;
}

Status TcpNtripClient::fetch_sourcetable(const NtripConfig& cfg,
                                         std::vector<NtripSource>* out) {
  if (cfg.host.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "fetch_sourcetable: empty host");
  }
  int neterr = 0;
  scan_socket_t s = gnss_net::tcp_connect(cfg.host, cfg.port, cfg.connect_timeout_ms, &neterr);
  if (s == SCAN_INVALID_SOCKET) {
    return set_last_error(neterr == 3 ? ScanError::kTimeout : ScanError::kNetworkError,
                          "fetch_sourcetable: cannot reach %s:%u", cfg.host.c_str(),
                          static_cast<unsigned>(cfg.port));
  }
  gnss_net::set_recv_timeout(s, std::max(200, cfg.read_timeout_ms));
  gnss_net::set_send_timeout(s, std::max(1000, cfg.connect_timeout_ms));

  const std::string req = impl_->request_(cfg, cfg.ntrip_version >= 2 ? 2 : 1, "/");
  if (!gnss_net::send_all(s, req.data(), req.size())) {
    scan_close_socket(s);
    return set_last_error(ScanError::kNetworkError, "fetch_sourcetable: send failed");
  }

  std::string buf;
  char tmp[4096];
  const std::int64_t deadline =
      now_ns() + static_cast<std::int64_t>(cfg.connect_timeout_ms) * 1000000LL;
  while (now_ns() < deadline && buf.size() < (1u << 20)) {
#if defined(_WIN32)
    const int r = ::recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
#else
    const ssize_t r = ::recv(s, tmp, sizeof(tmp), 0);
#endif
    if (r > 0) {
      buf.append(tmp, static_cast<std::size_t>(r));
      if (buf.find("ENDSOURCETABLE") != std::string::npos) break;
      continue;
    }
    if (r == 0) break;
    if (!gnss_net::would_block()) break;
  }
  scan_close_socket(s);

  const std::size_t hdr = buf.find("\r\n\r\n");
  const std::string body = (hdr == std::string::npos) ? buf : buf.substr(hdr + 4);
  if (body.empty()) {
    return set_last_error(ScanError::kProtocolError,
                          "fetch_sourcetable: %s:%u returned no sourcetable",
                          cfg.host.c_str(), static_cast<unsigned>(cfg.port));
  }
  std::vector<NtripSource> parsed = parse_sourcetable(body);
  if (parsed.empty()) {
    return set_last_error(ScanError::kProtocolError,
                          "fetch_sourcetable: no STR records in %zu bytes", body.size());
  }
  if (out) out->swap(parsed);
  return kOkStatus;
}

Status TcpNtripClient::list_mountpoints(std::vector<std::string>* out) {
  if (out == nullptr) return set_last_error(ScanError::kInvalidArgument, "null out");
  NtripConfig cfg;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    cfg = impl_->cfg;
  }
  if (cfg.host.empty()) {
    return set_last_error(ScanError::kInvalidState,
                          "list_mountpoints: no host configured; call connect() or "
                          "fetch_sourcetable(cfg, …) first");
  }
  std::vector<NtripSource> sources;
  SCAN_TRY(fetch_sourcetable(cfg, &sources));
  out->clear();
  out->reserve(sources.size());
  for (const NtripSource& s : sources) out->push_back(s.mountpoint);
  return kOkStatus;
}

}  // namespace scanengine
