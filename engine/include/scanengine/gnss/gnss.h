// gnss.h — NTRIP client, NMEA/RTCM3 handling, georeferencing (§3.4).
//
// A1 shipped this file as a seam. A10 filled it in and split the
// implementation across four sibling headers, all of which include this one:
//
//   gnss/nmea.h          NMEA 0183 framing + GGA/RMC/GST/GSA/VTG decoding
//   gnss/rtcm3.h         RTCM3 transport framing + CRC-24Q (no message decode)
//   gnss/crs.h           WGS84 / ECEF / ENU / UTM, EPSG + WKT generation
//   gnss/gnss_source.h   GnssSource: NMEA in → fixes + PoseSource/Interpolator
//   gnss/ntrip_client.h  TcpNtripClient: caster → RTCM3 → rover
//   gnss/georef.h        GeorefFusion: local ↔ global similarity transform
//
// Fixed here (unchanged from A1): the fix-state vocabulary the capture UI
// gates on (Tech Spec §2.3 "RTK Fixed / Float / DGPS / Single / none"),
// because B9's status strip and A8's outdoor-trajectory gate both key off
// these values.
//
// `GnssFix` gained fields in A10. They are APPENDED, the A1 fields keep their
// meaning, and nothing mirrors this struct across the C ABI yet — see
// docs/A10-gnss.md §9 for the C-ABI surface B9 needs.
#ifndef SCANENGINE_GNSS_GNSS_H
#define SCANENGINE_GNSS_GNSS_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/poses/pose_source.h"

namespace scanengine {

enum class FixType : std::uint8_t {
  kNone = 0,
  kSingle = 1,
  kDgps = 2,
  kRtkFloat = 3,
  kRtkFixed = 4,
};

struct GnssFix {
  // --- A1 fields, unchanged --------------------------------------------
  std::int64_t t_mono_ns = 0;   // ENGINE time (A4-mapped), not UTC
  FixType fix = FixType::kNone;
  double lat_deg = 0.0, lon_deg = 0.0, alt_m = 0.0;  // alt_m is ORTHOMETRIC (MSL)
  float hdop = 0.f;
  float correction_age_s = 0.f;  // GGA field 13, as reported by the rover
  std::uint8_t satellites = 0;

  // --- A10 additions ----------------------------------------------------
  std::int64_t t_arrival_ns = 0;   // raw arrival stamp, before A4 mapping
  std::int64_t utc_unix_ns = 0;    // 0 when no RMC date has been seen yet
  double geoid_sep_m = 0.0;        // GGA field 11; ellipsoidal = alt_m + this
  double height_ellipsoid_m = 0.0; // what the geodesy actually uses
  bool has_geoid_sep = false;

  // 1-sigma, metres. From GST when the receiver sends it, else the
  // fix-quality table in GnssSourceConfig. `sigma_from_gst` says which.
  float sigma_east_m = 0.f, sigma_north_m = 0.f, sigma_up_m = 0.f;
  float sigma_horizontal_m = 0.f;  // sqrt(E² + N²) / sqrt(2) → per-axis RMS
  bool sigma_from_gst = false;

  float pdop = 0.f, vdop = 0.f;
  float speed_mps = 0.f;
  float course_deg = 0.f;
  bool has_course = false;
  std::uint8_t quality_raw = 0;    // GGA field 6 verbatim (3=PPS, 8=simulator…)
  std::uint16_t station_id = 0;    // GGA field 14: which base is correcting us
  std::uint8_t fix_dimension = 0;  // GSA field 2: 1 none, 2 = 2D, 3 = 3D

  bool has_position() const { return fix != FixType::kNone; }
};

const char* to_string(FixType f) noexcept;

// Ordering: kNone < kSingle < kDgps < kRtkFloat < kRtkFixed, which is the
// enum order, so the comparison is on the underlying value. Every gate in
// gnss/ and B9's capture gating go through this rather than open-coding the
// cast, so a future fix state inserted in the middle breaks one function.
inline bool fix_at_least(FixType have, FixType want) noexcept {
  return static_cast<std::uint8_t>(have) >= static_cast<std::uint8_t>(want);
}

// 1-sigma horizontal accuracy to EXPECT from a fix state, metres, when the
// receiver does not send GST. The §3.4 numbers (Fixed 2 cm / Float 30 cm /
// Single 2 m) with DGPS interpolated at 0.5 m, which is also the noise the
// S5 simulator injects — so a fusion weight derived from this table and one
// derived from the simulator's own GST agree.
double default_sigma_for_fix(FixType f) noexcept;

// NMEA in (from the rover over BT/serial, pushed by the app), RTCM3 out
// (from the NTRIP caster, forwarded to the rover through the same link).
class GnssReceiver : public PoseSource {
 public:
  // `sentence` may be ANY chunk of the byte stream — a single sentence, a
  // fragment, or several. The implementation frames it (see gnss/nmea.h).
  virtual Status push_nmea(ByteSpan sentence, std::int64_t t_mono_ns) = 0;
  virtual GnssFix last_fix() const = 0;
};

// How the rover is reached, and how the client behaves when the caster or
// the network misbehaves. A1 fixed the first five fields; the rest are A10's
// and all have defaults, so an A1-era `NtripConfig{host, port, mount, …}`
// still compiles and still means the same thing.
struct NtripConfig {
  std::string host;
  std::uint16_t port = 2101;
  std::string mountpoint;
  std::string username;
  std::string password;

  // 2 sends `Ntrip-Version: Ntrip/2.0` and expects an HTTP response; 1 uses
  // the bare "ICY 200 OK" form. Default 2 with automatic fallback to 1 —
  // RTK2go and most community casters are v1-only in practice, and a client
  // that cannot fall back looks like a broken client to the user.
  int ntrip_version = 2;
  bool allow_v1_fallback = true;

  std::string user_agent = "NTRIP LidarScan/0.1.0";

  std::int32_t connect_timeout_ms = 8000;
  std::int32_t read_timeout_ms = 1000;   // socket poll granularity, not a failure

  // No RTCM byte for this long ⇒ the connection is dead even though the
  // socket is open. Casters do go quiet; 30 s of silence with a 1 Hz
  // correction stream is not a quiet caster, it is a black hole.
  std::int32_t stall_timeout_ms = 30000;

  // Reconnect backoff: 1 s, 2 s, 4 s … capped, with ±25 % jitter so a fleet
  // of rovers does not synchronise onto one caster.
  bool auto_reconnect = true;
  std::int32_t reconnect_initial_ms = 1000;
  std::int32_t reconnect_max_ms = 30000;
  int max_reconnect_attempts = 0;  // 0 = forever

  // GGA upload cadence (0 disables). Every VRS/Network-RTK caster needs it;
  // a single-base caster ignores it. 10 s is the interval the NTRIP 2.0
  // standard suggests and what u-center defaults to.
  std::int32_t gga_interval_ms = 10000;
  bool send_gga_on_connect = true;
};

class NtripClient {
 public:
  virtual ~NtripClient() = default;
  virtual Status connect(const NtripConfig& cfg) = 0;
  virtual Status disconnect() = 0;
  virtual Status list_mountpoints(std::vector<std::string>* out) = 0;
  // RTCM3 arrives here and must be forwarded to the rover by the app.
  // Whole, CRC-valid frames only (see gnss/rtcm3.h for why).
  virtual void set_rtcm_callback(void (*cb)(ByteSpan rtcm, void* user), void* user) = 0;
  virtual float correction_age_s() const = 0;
};

// CRS handling (§3.4): EPSG picker, WGS84/UTM defaults, coarse EGM96 geoid.
struct CrsConfig {
  std::string epsg;           // empty = local frame only
  bool apply_geoid = true;    // coarse EGM96 in Phase 1

  // Empty + auto_utm ⇒ the UTM zone containing the session origin is chosen
  // and `epsg` is filled in once the origin is known. This is the "WGS84/UTM
  // default" of §3.4; a survey profile overrides it by setting `epsg`.
  bool auto_utm = true;
};

}  // namespace scanengine

#endif  // SCANENGINE_GNSS_GNSS_H
