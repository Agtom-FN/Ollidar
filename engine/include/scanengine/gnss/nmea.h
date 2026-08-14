// nmea.h — NMEA 0183 sentence parsing (Tech Spec §2.3, §3.4).
//
// What this file is for: a u-blox ZED-F9P or an Emlid Reach hands the app a
// byte stream over Bluetooth SPP / USB serial. The app pushes those bytes
// into the engine opaquely (DESIGN §3 key rule 1). This is where they become
// positions, fix states and per-axis uncertainties.
//
// Three deliberate properties, each of which came out of the S5 spike:
//
//  1. **Per-sentence errors are tolerated, never fatal.** A Bluetooth SPP
//     link drops bytes; a receiver mid-configuration emits a truncated
//     sentence; a multi-GNSS receiver emits proprietary `$PUBX`/`$PASHR`
//     sentences this parser has no opinion about. Every one of those is
//     counted in `NmeaStats` and skipped. A GNSS stream that loses a
//     sentence must not lose the session — that is the §3 "failure is
//     graded, not binary" rule applied to a link with no retransmission.
//
//  2. **Fields are string_views into the caller's buffer.** Parsing a
//     sentence allocates nothing. `Sentence` is valid only while the line it
//     was parsed from is alive; `NmeaFramer` owns that line for the duration
//     of its handler call and no longer.
//
//  3. **The fix vocabulary is `FixType` from gnss.h, not a private enum.**
//     B9's status strip and A8's outdoor-trajectory gate key off those five
//     values (gnss.h header comment), so the mapping from GGA quality digits
//     and NMEA 2.3 mode characters onto them lives here, in one function,
//     with the raw digit preserved in `GgaData::quality_raw` for anyone who
//     needs to tell "PPS" from "single".
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_NMEA_H
#define SCANENGINE_GNSS_NMEA_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/gnss/gnss.h"

namespace scanengine {
namespace nmea {

// NMEA 0183 caps a sentence at 82 bytes including "$" and the CRLF. Real
// receivers exceed it (u-blox GSV with many constellations, some GST
// variants), so the framer's limit is a config field that DEFAULTS higher
// than the standard and counts oversize lines rather than corrupting the
// stream by resyncing mid-sentence.
inline constexpr std::size_t kStandardMaxSentenceBytes = 82;
inline constexpr std::size_t kMaxFields = 40;

enum class SentenceId : std::uint8_t {
  kUnknown = 0,
  kGga = 1,  // position, fix quality, satellites, HDOP, altitude, DGPS age
  kRmc = 2,  // recommended minimum: UTC date+time, position, SOG, COG
  kGst = 3,  // pseudorange error statistics — the per-axis sigmas
  kGsa = 4,  // active satellites + PDOP/HDOP/VDOP + 2D/3D fix type
  kVtg = 5,  // course and speed over ground
};

const char* to_string(SentenceId id) noexcept;

// Why a sentence was rejected. Every value is counted separately in
// NmeaStats because they mean different things operationally: a burst of
// kBadChecksum is a noisy link, a burst of kTooLong is the wrong baud rate,
// and kUnknownStart is usually binary (UBX/RTCM) mixed into the NMEA stream.
enum class NmeaError : std::uint8_t {
  kOk = 0,
  kEmpty = 1,          // nothing between the delimiters
  kNoStart = 2,        // no '$' / '!' start delimiter
  kNoChecksum = 3,     // no '*HH' — accepted or rejected per config
  kBadChecksum = 4,    // '*HH' present and wrong
  kBadChecksumHex = 5, // '*' present but the two bytes after it are not hex
  kTooLong = 6,
  kTooShort = 7,       // shorter than "$XXYYY"
  kBadCharacter = 8,   // control byte or non-ASCII inside the body
  kTooManyFields = 9,
};

const char* to_string(NmeaError e) noexcept;

// A framed, checksum-verified sentence. Views point into the buffer passed
// to parse_sentence(); nothing here owns memory.
struct Sentence {
  std::string_view raw;          // "$GNGGA,...*4A", no CR/LF
  std::string_view talker;       // "GN", "GP", "GL", ... ; "P" for proprietary
  std::string_view type;         // "GGA", "RMC", ...
  SentenceId id = SentenceId::kUnknown;
  bool proprietary = false;      // "$P..." — talker/type split does not apply
  bool checksum_present = false;
  std::uint8_t checksum = 0;

  std::string_view fields[kMaxFields];
  std::size_t field_count = 0;

  std::string_view field(std::size_t i) const {
    return i < field_count ? fields[i] : std::string_view{};
  }
  bool has_field(std::size_t i) const { return i < field_count && !fields[i].empty(); }
};

// XOR of every byte strictly between '$' and '*', the NMEA 0183 algorithm.
std::uint8_t checksum_of(std::string_view body) noexcept;

struct ParseOptions {
  // A sentence with no "*HH" is legal in NMEA 0183 v2.x and some Bluetooth
  // bridges strip it. Default false: for a survey-grade position we insist
  // on the integrity check, because the alternative is silently trusting a
  // latitude with a corrupted digit.
  bool allow_missing_checksum = false;
  std::size_t max_bytes = 256;
};

// Parses ONE sentence with no trailing CR/LF. `out` is filled as far as the
// parse got even on failure, so a caller can log the talker of a
// checksum-failed line.
NmeaError parse_sentence(std::string_view line, Sentence* out,
                         const ParseOptions& opt = {}) noexcept;

// --- typed decoders --------------------------------------------------------
//
// Each returns false only when the sentence is not that type or has too few
// fields to be that type at all. A single unparseable NUMBER inside an
// otherwise well-formed sentence leaves its `has_*` flag false and the rest
// of the decode intact — a receiver that emits an empty HDOP must not cost
// us the position that came with it.

struct GgaData {
  bool has_time = false;
  double utc_sod_s = 0.0;      // UTC seconds of day (hhmmss.ss)
  bool has_position = false;
  double lat_deg = 0.0, lon_deg = 0.0;
  int quality_raw = 0;         // GGA field 6, verbatim
  FixType fix = FixType::kNone;
  bool has_satellites = false;
  int satellites = 0;
  bool has_hdop = false;
  double hdop = 0.0;
  bool has_alt = false;
  double alt_msl_m = 0.0;      // orthometric height above the receiver's geoid
  bool has_geoid_sep = false;
  double geoid_sep_m = 0.0;    // ellipsoidal = alt_msl_m + geoid_sep_m
  bool has_dgps_age = false;
  double dgps_age_s = 0.0;     // AGE OF DIFFERENTIAL DATA — the corrections age
  bool has_station = false;
  int station_id = 0;
};

struct RmcData {
  bool has_time = false;
  double utc_sod_s = 0.0;
  bool valid = false;          // 'A' = active, 'V' = void
  bool has_position = false;
  double lat_deg = 0.0, lon_deg = 0.0;
  bool has_speed = false;
  double speed_knots = 0.0;
  bool has_course = false;
  double course_deg = 0.0;     // true course over ground
  bool has_date = false;
  int day = 0, month = 0, year = 0;  // year is full (2026), windowed from yy
  char mode = '\0';            // NMEA 2.3+ mode indicator, '\0' when absent
  FixType fix = FixType::kNone;  // derived from `mode` (kNone when absent)
};

struct GstData {
  bool has_time = false;
  double utc_sod_s = 0.0;
  bool has_rms = false;
  double rms_m = 0.0;
  bool has_ellipse = false;
  double semi_major_m = 0.0, semi_minor_m = 0.0, orientation_deg = 0.0;
  bool has_sigmas = false;
  double lat_sigma_m = 0.0, lon_sigma_m = 0.0, alt_sigma_m = 0.0;
};

struct GsaData {
  char mode = '\0';        // 'M' manual, 'A' automatic
  bool has_fix_type = false;
  int fix_type = 0;        // 1 = none, 2 = 2D, 3 = 3D
  int satellites_used = 0; // count of non-empty PRN fields
  bool has_pdop = false, has_hdop = false, has_vdop = false;
  double pdop = 0.0, hdop = 0.0, vdop = 0.0;
};

struct VtgData {
  bool has_course_true = false;
  double course_true_deg = 0.0;
  bool has_course_mag = false;
  double course_mag_deg = 0.0;
  bool has_speed = false;
  double speed_knots = 0.0, speed_kmh = 0.0;
  char mode = '\0';
};

bool decode_gga(const Sentence& s, GgaData* out) noexcept;
bool decode_rmc(const Sentence& s, RmcData* out) noexcept;
bool decode_gst(const Sentence& s, GstData* out) noexcept;
bool decode_gsa(const Sentence& s, GsaData* out) noexcept;
bool decode_vtg(const Sentence& s, VtgData* out) noexcept;

// --- fix-state vocabulary --------------------------------------------------
//
// GGA quality digit (NMEA 0183 / receiver convention):
//   0 invalid · 1 GPS(SPS) · 2 DGPS · 3 PPS · 4 RTK Fixed · 5 RTK Float
//   6 estimated (dead reckoning) · 7 manual input · 8 simulator
//
// 6 and 7 map to kNone ON PURPOSE. Dead reckoning and a manually entered
// coordinate are not GNSS observations; georeferencing a cloud with one
// would produce a confidently wrong absolute position, which is the one
// failure mode §3.4's fix-quality gate exists to prevent. 8 (simulator) maps
// to kSingle so a bench rig driven by the S5 simulator still exercises the
// full path, and `GgaData::quality_raw` keeps the evidence.
FixType fix_from_gga_quality(int quality_raw) noexcept;

// NMEA 2.3+ mode indicator (RMC/VTG/GNS field): N none · A autonomous ·
// D differential · R RTK fixed · F RTK float · E dead reckoning ·
// S simulator · M manual.
FixType fix_from_mode_char(char mode) noexcept;

// --- UTC ------------------------------------------------------------------

// (date from RMC, seconds-of-day from any sentence) -> ns since the Unix
// epoch. Returns false when the date is absent or out of range. Proleptic
// Gregorian civil-from-days (Howard Hinnant's algorithm) — no <ctime>, so it
// is identical on all five CI legs and unaffected by TZ.
bool utc_to_unix_ns(int year, int month, int day, double sod_s,
                    std::int64_t* out_ns) noexcept;

// --- streaming framer ------------------------------------------------------

struct NmeaStats {
  std::uint64_t bytes_in = 0;
  std::uint64_t lines = 0;             // CR/LF-delimited candidates seen
  std::uint64_t sentences_ok = 0;
  std::uint64_t checksum_failed = 0;
  std::uint64_t missing_checksum = 0;
  std::uint64_t malformed = 0;         // kNoStart/kTooShort/kBadCharacter/...
  std::uint64_t oversize = 0;
  std::uint64_t unknown_type = 0;      // parsed fine, not one of the five
  std::uint64_t proprietary = 0;       // "$P..." — counted, not an error
  std::uint64_t dropped_bytes = 0;     // bytes discarded outside any sentence
  std::int64_t t_last_sentence_ns = 0;

  double checksum_pass_rate() const {
    const std::uint64_t seen = sentences_ok + checksum_failed;
    return seen ? static_cast<double>(sentences_ok) / static_cast<double>(seen) : 0.0;
  }
};

struct FramerConfig {
  ParseOptions parse{};
  // Bytes buffered for one sentence before the framer gives up and resyncs.
  // Larger than parse.max_bytes so an oversize line is REPORTED rather than
  // silently split into two bogus ones.
  std::size_t max_line_bytes = 512;
};

// Byte-stream → sentences. Handles arbitrary chunking (Bluetooth SPP hands
// the app 20–990-byte MTU fragments; the S1 replay harness proved the D6
// parser at 64-byte boundaries and this one is tested the same way), bare
// LF as well as CRLF, and binary garbage between sentences.
//
// Not thread-safe: one framer per link, pushed from that link's thread.
class NmeaFramer {
 public:
  // (line, parsed sentence, arrival stamp). `line` and every view inside
  // `s` die when the handler returns.
  using Handler = std::function<void(std::string_view line, const Sentence& s,
                                     std::int64_t t_arrival_ns)>;
  using ErrorHandler = std::function<void(std::string_view line, NmeaError e,
                                          std::int64_t t_arrival_ns)>;

  explicit NmeaFramer(const FramerConfig& cfg = {});

  void set_handler(Handler h) { handler_ = std::move(h); }
  void set_error_handler(ErrorHandler h) { errors_ = std::move(h); }

  void push(ByteSpan bytes, std::int64_t t_arrival_ns);
  void reset();

  const NmeaStats& stats() const { return stats_; }
  const FramerConfig& config() const { return cfg_; }

 private:
  void emit_line_(std::int64_t t_ns);

  FramerConfig cfg_;
  std::string line_;
  bool overflowed_ = false;
  Handler handler_;
  ErrorHandler errors_;
  NmeaStats stats_{};
};

// --- sentence construction (NTRIP GGA upload) -----------------------------
//
// A Network-RTK/VRS caster needs the rover's approximate position to pick a
// base or synthesize a virtual one, and the standard channel is a GGA
// sentence uploaded on the same socket the corrections come down. The NTRIP
// client prefers the rover's OWN last GGA (byte-identical, so the caster
// sees exactly what the receiver said); this builder is the fallback for
// before the first fix arrives and for a caller that only has a coordinate.
struct GgaBuilderInput {
  double utc_sod_s = 0.0;
  double lat_deg = 0.0, lon_deg = 0.0;
  double alt_msl_m = 0.0;
  double geoid_sep_m = 0.0;
  int quality = 1;
  int satellites = 8;
  double hdop = 1.0;
  const char* talker = "GN";
};

std::string build_gga(const GgaBuilderInput& in);

}  // namespace nmea
}  // namespace scanengine

#endif  // SCANENGINE_GNSS_NMEA_H
