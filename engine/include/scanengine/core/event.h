// event.h — the engine's outbound event vocabulary.
//
// THE RULE THAT SHAPES THIS FILE: events are small, fixed-size, trivially
// copyable control messages. Bulk data NEVER travels in an event. Points go
// to cloud/PageStore and the event carries only (page_id, first, count);
// raw bytes go to record/; camera frames go to the .lscan frame stream.
// That is what lets the same struct cross the C ABI by value, be queued in a
// fixed-size ring, and be dropped under backpressure without leaking.
//
// Payloads live in a union of PODs. Adding an event type: append an
// EventType value, append a payload struct to the union, mirror both in
// capi/scanengine_c.h and extend the converter in capi/scanengine_c.cpp
// (which static_asserts sizes). Never renumber.
//
// Owner: A1.
#ifndef SCANENGINE_CORE_EVENT_H
#define SCANENGINE_CORE_EVENT_H

#include <cstdint>

#include "scanengine/core/types.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

enum class EventType : std::uint16_t {
  kNone = 0,

  // Bus meta. Emitted by the bus itself, not by a module: it tells a
  // subscriber that N events for it were dropped before the next one it is
  // about to receive (see EventBus overflow policy).
  kEventsDropped = 1,

  kEngineState = 10,   // payload.engine_state
  kSessionState = 11,  // payload.session   — A5 extends with .lscan path info
  kDeviceState = 20,   // payload.device
  kDeviceHealth = 21,  // payload.health_summary (throttled snapshot)
  kPointsAvailable = 30, // payload.points — the render-facing signal
  kRotation = 31,      // payload.rotation — D6 revolution completed
  kPoseUpdate = 40,    // payload.pose_update  — A4/A8/A10 fill
  kGnssFix = 50,       // payload.gnss        — A10, published per closed epoch
  kNtripState = 51,    // payload.ntrip       — A10, the corrections link
  kGeorefConverged = 52, // payload.georef    — A10, the session became exportable
  kJobProgress = 60,   // payload.job          — A15 fills
  kError = 90,         // payload.error — non-fatal, already logged
};

const char* to_string(EventType t) noexcept;

struct EventsDroppedPayload {
  std::uint64_t count;      // events dropped since the previous delivery
  std::uint64_t total;      // dropped since subscription
};

struct EngineStatePayload {
  EngineState state;
  EngineState previous;
};

struct SessionStatePayload {
  std::uint8_t recording;   // 0/1
  std::uint64_t session_id;
  std::uint64_t bytes_written;
};

struct DeviceStatePayload {
  DeviceId device;
  DeviceKind kind;
  DeviceState state;
  DeviceState previous;
  ScanError error;          // kOk unless state == kFault/kDegraded
};

struct DeviceHealthPayload {
  DeviceId device;
  DeviceState state;
  std::uint64_t points_out;
  double points_per_sec;
  double checksum_pass_rate;
};

// The render-facing event. `page` + [first, first+count) is exactly the
// range a renderer must upload; see cloud/page_store.h for the memory
// contract that makes reading it lock-free.
struct PointsAvailablePayload {
  PageId page;
  std::uint32_t first;
  std::uint32_t count;
  StreamId stream;
  std::uint8_t page_created;  // 0/1 — renderer must allocate a GPU buffer
};

struct RotationPayload {
  DeviceId device;
  std::uint64_t rotation_index;
  std::uint32_t points_in_rotation;
  double rotation_hz;
};

struct PoseUpdatePayload {  // A4/A8/A10
  StreamId source;
  float position[3];
  float quaternion[4];  // x, y, z, w
  std::uint8_t quality; // 0 = invalid .. 255 = best
};

struct GnssFixPayload {  // A10
  // FixType, not the GGA quality digit: 0 none, 1 single, 2 dgps,
  // 3 rtk-float, 4 rtk-fixed. `GnssFix::quality_raw` keeps the wire digit for
  // anyone who needs to tell a PPS fix from a simulator one; this field is the
  // ordered vocabulary fix_at_least() gates on.
  std::uint8_t fix_type;
  std::uint8_t satellites;
  float hdop;
  float correction_age_s;   // GGA field 13 — the ROVER's corrections age
  // 1-sigma horizontal, metres. From GST when the receiver sends it, else the
  // fix-quality fallback table. B9's status strip shows this, not the DOP.
  float sigma_h_m;
  double lat_deg, lon_deg, alt_m;  // alt_m is ORTHOMETRIC, as GGA reports it
};

struct NtripStatePayload {  // A10
  std::uint8_t state;            // NtripState
  ScanError error;               // kOk unless state == kFailed/kReconnecting
  std::int32_t backoff_ms;       // what the next reconnect will wait
  std::uint64_t bytes_received;  // this session, across reconnects
  // Time since the last CRC-valid RTCM3 frame off the CASTER. −1 means "no
  // frame yet on this connection" — "unknown" and "fresh" are different
  // claims (docs/A10-gnss.md §3) and only one of them is reassuring.
  float correction_age_s;
};

struct GeorefConvergedPayload {  // A10
  double cep95_m;
  double horizontal_sigma_m;
  std::uint32_t samples;   // inliers in the converged fit
  std::int32_t epsg;       // 0 until the origin picks a UTM zone
  std::uint8_t converged;  // 0/1 — a transform can also STOP converging
};

struct JobProgressPayload {  // A15
  std::uint64_t job_id;
  float progress;   // 0..1
  std::uint8_t state;
};

struct ErrorPayload {
  ScanError error;
  DeviceId device;    // kInvalidDeviceId when not device-specific
  StreamId stream;
};

// One event. Trivially copyable, no pointers, no ownership.
struct Event {
  EventType type = EventType::kNone;
  std::uint16_t reserved = 0;
  std::uint32_t sequence = 0;   // global publish order, starts at 1
  std::int64_t t_mono_ns = 0;   // SteadyClock at publish

  union Payload {
    EventsDroppedPayload dropped;
    EngineStatePayload engine_state;
    SessionStatePayload session;
    DeviceStatePayload device;
    DeviceHealthPayload health;
    PointsAvailablePayload points;
    RotationPayload rotation;
    PoseUpdatePayload pose;
    GnssFixPayload gnss;
    NtripStatePayload ntrip;
    GeorefConvergedPayload georef;
    JobProgressPayload job;
    ErrorPayload error;
    std::uint8_t raw[64];
  } payload{};

  Event() { for (auto& b : payload.raw) b = 0; }
};

static_assert(sizeof(Event::Payload) <= 72,
              "Event payload must stay small: it is copied by value into every "
              "subscriber ring and mirrored across the C ABI");

// Bitmask filter for subscriptions. EventType values are sparse on purpose
// (10, 20, 30 ...) so a mask is built from categories rather than 64 flags.
enum class EventCategory : std::uint32_t {
  kNone = 0,
  kEngine = 1u << 0,   // kEngineState, kSessionState
  kDevice = 1u << 1,   // kDeviceState, kDeviceHealth
  kPoints = 1u << 2,   // kPointsAvailable, kRotation
  // kPose covers the whole trajectory/geo group: a consumer that wants the
  // pose stream wants the fix quality behind it, the corrections link that
  // produced that quality, and the moment the session became georeferenced.
  kPose = 1u << 3,     // kPoseUpdate, kGnssFix, kNtripState, kGeorefConverged
  kJobs = 1u << 4,     // kJobProgress
  kErrors = 1u << 5,   // kError
  kMeta = 1u << 6,     // kEventsDropped (always delivered regardless)
  kAll = 0xFFFFFFFFu,
};

EventCategory category_of(EventType t) noexcept;

inline std::uint32_t mask_of(EventCategory c) noexcept {
  return static_cast<std::uint32_t>(c);
}

}  // namespace scanengine

#endif  // SCANENGINE_CORE_EVENT_H
