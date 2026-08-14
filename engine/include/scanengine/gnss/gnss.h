// gnss.h — NTRIP client, NMEA/RTCM3 handling, georeferencing (§3.4).
//
// SEAM ONLY. Owner: A10 (depends on A4 and the S5 sim infrastructure:
// spikes/s5-rtk-sim has NMEA/NTRIP/RTCM3 simulation with 10/10 self-tests —
// build A10's unit tests against those fixtures before real rover data
// arrives).
//
// Fixed here: the fix-state vocabulary the capture UI gates on (Tech Spec
// §2.3 "RTK Fixed / Float / DGPS / Single / none"), because B9's status
// strip and A8's outdoor-trajectory gate both key off these values.
#ifndef SCANENGINE_GNSS_GNSS_H
#define SCANENGINE_GNSS_GNSS_H

#include <cstdint>
#include <string>

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
  std::int64_t t_mono_ns = 0;
  FixType fix = FixType::kNone;
  double lat_deg = 0.0, lon_deg = 0.0, alt_m = 0.0;
  float hdop = 0.f;
  float correction_age_s = 0.f;
  std::uint8_t satellites = 0;
};

// NMEA in (from the rover over BT/serial, pushed by the app), RTCM3 out
// (from the NTRIP caster, forwarded to the rover through the same link).
class GnssReceiver : public PoseSource {
 public:
  virtual Status push_nmea(ByteSpan sentence, std::int64_t t_mono_ns) = 0;
  virtual GnssFix last_fix() const = 0;
};

struct NtripConfig {
  std::string host;
  std::uint16_t port = 2101;
  std::string mountpoint;
  std::string username;
  std::string password;
};

class NtripClient {
 public:
  virtual ~NtripClient() = default;
  virtual Status connect(const NtripConfig& cfg) = 0;
  virtual Status disconnect() = 0;
  virtual Status list_mountpoints(std::vector<std::string>* out) = 0;
  // RTCM3 arrives here and must be forwarded to the rover by the app.
  virtual void set_rtcm_callback(void (*cb)(ByteSpan rtcm, void* user), void* user) = 0;
  virtual float correction_age_s() const = 0;
};

// CRS handling (§3.4): EPSG picker, WGS84/UTM defaults, coarse EGM96 geoid.
struct CrsConfig {
  std::string epsg;           // empty = local frame only
  bool apply_geoid = true;    // coarse EGM96 in Phase 1
};

}  // namespace scanengine

#endif  // SCANENGINE_GNSS_GNSS_H
