#include "scanengine/gnss/nmea.h"

#include <cstdio>

namespace scanengine {

const char* to_string(FixType f) noexcept {
  switch (f) {
    case FixType::kNone: return "none";
    case FixType::kSingle: return "single";
    case FixType::kDgps: return "dgps";
    case FixType::kRtkFloat: return "rtk-float";
    case FixType::kRtkFixed: return "rtk-fixed";
  }
  return "?";
}

double default_sigma_for_fix(FixType f) noexcept {
  switch (f) {
    case FixType::kRtkFixed: return 0.02;
    case FixType::kRtkFloat: return 0.30;
    case FixType::kDgps: return 0.50;
    case FixType::kSingle: return 2.00;
    case FixType::kNone: break;
  }
  return 100.0;  // "no information", not "zero error"
}

namespace nmea {
namespace {

bool is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return c - 'a' + 10;
}

// Locale-independent, allocation-free decimal parsing. `strtod` is NOT used:
// it honours LC_NUMERIC, and an app running under a de_DE locale would read
// "22.28" as 22. That is a real, silent, 22-degree georeferencing bug.
bool parse_double(std::string_view s, double* out) noexcept {
  if (s.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[i] == '+' || s[i] == '-') {
    neg = (s[i] == '-');
    ++i;
  }
  std::int64_t mant = 0;
  int digits = 0, frac_digits = 0;
  bool seen_dot = false, seen_digit = false, saturated = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.') {
      if (seen_dot) return false;
      seen_dot = true;
      continue;
    }
    if (c < '0' || c > '9') return false;
    seen_digit = true;
    // 18 digits fits int64 with room; anything longer is a receiver bug and
    // the extra digits are far below the noise floor, so they are dropped
    // rather than overflowing.
    if (digits < 18) {
      mant = mant * 10 + (c - '0');
      ++digits;
      if (seen_dot) ++frac_digits;
    } else {
      saturated = true;
      if (!seen_dot) return false;  // an integer part this long is nonsense
    }
  }
  (void)saturated;
  if (!seen_digit) return false;
  double v = static_cast<double>(mant);
  for (int k = 0; k < frac_digits; ++k) v *= 0.1;
  *out = neg ? -v : v;
  return true;
}

bool parse_int(std::string_view s, int* out) noexcept {
  if (s.empty()) return false;
  std::size_t i = 0;
  bool neg = false;
  if (s[i] == '+' || s[i] == '-') {
    neg = (s[i] == '-');
    ++i;
  }
  if (i >= s.size()) return false;
  long long v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
    if (v > 2000000000LL) return false;
  }
  *out = static_cast<int>(neg ? -v : v);
  return true;
}

// "hhmmss.ss" -> seconds of day. Tolerates a missing fractional part and the
// leap-second value 60.
bool parse_time(std::string_view s, double* out) noexcept {
  if (s.size() < 6) return false;
  int h = 0, m = 0;
  if (!parse_int(s.substr(0, 2), &h) || !parse_int(s.substr(2, 2), &m)) return false;
  double sec = 0.0;
  if (!parse_double(s.substr(4), &sec)) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0.0 || sec >= 61.0) return false;
  *out = h * 3600.0 + m * 60.0 + sec;
  return true;
}

// "ddmm.mmmm" / "dddmm.mmmm" + hemisphere -> signed degrees.
bool parse_latlon(std::string_view v, std::string_view hemi, bool is_lat,
                  double* out) noexcept {
  if (v.empty() || hemi.empty()) return false;
  const std::size_t deg_digits = is_lat ? 2 : 3;
  if (v.size() < deg_digits + 1) return false;
  int deg = 0;
  if (!parse_int(v.substr(0, deg_digits), &deg)) return false;
  double minutes = 0.0;
  if (!parse_double(v.substr(deg_digits), &minutes)) return false;
  if (minutes < 0.0 || minutes >= 60.0) return false;
  double d = static_cast<double>(deg) + minutes / 60.0;
  const char h = hemi[0];
  if (is_lat) {
    if (h == 'S' || h == 's') d = -d;
    else if (h != 'N' && h != 'n') return false;
    if (d < -90.0 || d > 90.0) return false;
  } else {
    if (h == 'W' || h == 'w') d = -d;
    else if (h != 'E' && h != 'e') return false;
    if (d < -180.0 || d > 180.0) return false;
  }
  *out = d;
  return true;
}

SentenceId id_of(std::string_view type) {
  if (type.size() != 3) return SentenceId::kUnknown;
  if (type == "GGA") return SentenceId::kGga;
  if (type == "RMC") return SentenceId::kRmc;
  if (type == "GST") return SentenceId::kGst;
  if (type == "GSA") return SentenceId::kGsa;
  if (type == "VTG") return SentenceId::kVtg;
  return SentenceId::kUnknown;
}

}  // namespace

const char* to_string(SentenceId id) noexcept {
  switch (id) {
    case SentenceId::kUnknown: return "unknown";
    case SentenceId::kGga: return "GGA";
    case SentenceId::kRmc: return "RMC";
    case SentenceId::kGst: return "GST";
    case SentenceId::kGsa: return "GSA";
    case SentenceId::kVtg: return "VTG";
  }
  return "?";
}

const char* to_string(NmeaError e) noexcept {
  switch (e) {
    case NmeaError::kOk: return "ok";
    case NmeaError::kEmpty: return "empty";
    case NmeaError::kNoStart: return "no-start-delimiter";
    case NmeaError::kNoChecksum: return "no-checksum";
    case NmeaError::kBadChecksum: return "bad-checksum";
    case NmeaError::kBadChecksumHex: return "bad-checksum-hex";
    case NmeaError::kTooLong: return "too-long";
    case NmeaError::kTooShort: return "too-short";
    case NmeaError::kBadCharacter: return "bad-character";
    case NmeaError::kTooManyFields: return "too-many-fields";
  }
  return "?";
}

std::uint8_t checksum_of(std::string_view body) noexcept {
  std::uint8_t cs = 0;
  for (char c : body) cs ^= static_cast<std::uint8_t>(c);
  return cs;
}

NmeaError parse_sentence(std::string_view line, Sentence* out,
                         const ParseOptions& opt) noexcept {
  if (out) *out = Sentence{};
  if (line.empty()) return NmeaError::kEmpty;
  if (line.size() > opt.max_bytes) return NmeaError::kTooLong;
  if (line[0] != '$' && line[0] != '!') return NmeaError::kNoStart;

  // Body ends at '*' (if present) or at the end of the line.
  const std::size_t star = line.find('*');
  std::string_view body = (star == std::string_view::npos) ? line.substr(1)
                                                           : line.substr(1, star - 1);
  if (body.size() < 5) return NmeaError::kTooShort;  // "GPGGA" is the minimum

  // Reject control characters and 8-bit bytes inside the body: those are how
  // a lost byte in a binary UBX message masquerades as a sentence.
  for (char c : body) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) return NmeaError::kBadCharacter;
  }

  bool checksum_present = false;
  std::uint8_t checksum = 0;
  if (star != std::string_view::npos) {
    const std::string_view cs = line.substr(star + 1);
    if (cs.size() < 2 || !is_hex(cs[0]) || !is_hex(cs[1])) return NmeaError::kBadChecksumHex;
    checksum = static_cast<std::uint8_t>(hex_val(cs[0]) * 16 + hex_val(cs[1]));
    checksum_present = true;
  } else if (!opt.allow_missing_checksum) {
    return NmeaError::kNoChecksum;
  }

  Sentence s;
  s.raw = line;
  s.checksum_present = checksum_present;
  s.checksum = checksum;

  // Address field, then comma-separated data fields.
  std::size_t comma = body.find(',');
  const std::string_view addr = (comma == std::string_view::npos) ? body : body.substr(0, comma);
  if (addr.size() < 5) return NmeaError::kTooShort;
  if (addr[0] == 'P') {
    s.proprietary = true;
    s.talker = addr.substr(0, 1);
    s.type = addr.substr(1);
  } else {
    s.talker = addr.substr(0, 2);
    s.type = addr.substr(2);
  }
  s.id = s.proprietary ? SentenceId::kUnknown : id_of(s.type);

  while (comma != std::string_view::npos) {
    const std::size_t next = body.find(',', comma + 1);
    const std::string_view f = (next == std::string_view::npos)
                                   ? body.substr(comma + 1)
                                   : body.substr(comma + 1, next - comma - 1);
    if (s.field_count >= kMaxFields) {
      if (out) *out = s;
      return NmeaError::kTooManyFields;
    }
    s.fields[s.field_count++] = f;
    comma = next;
  }

  if (out) *out = s;
  if (checksum_present && checksum_of(body) != checksum) return NmeaError::kBadChecksum;
  return NmeaError::kOk;
}

// --- typed decoders --------------------------------------------------------

bool decode_gga(const Sentence& s, GgaData* out) noexcept {
  if (s.id != SentenceId::kGga || !out) return false;
  *out = GgaData{};
  // A GGA with no fix legitimately has empty lat/lon/alt fields, so the only
  // hard requirement is enough commas to reach the quality digit.
  if (s.field_count < 6) return false;

  out->has_time = parse_time(s.field(0), &out->utc_sod_s);

  double lat = 0.0, lon = 0.0;
  if (parse_latlon(s.field(1), s.field(2), true, &lat) &&
      parse_latlon(s.field(3), s.field(4), false, &lon)) {
    out->has_position = true;
    out->lat_deg = lat;
    out->lon_deg = lon;
  }

  int q = 0;
  if (parse_int(s.field(5), &q)) out->quality_raw = q;
  out->fix = fix_from_gga_quality(out->quality_raw);
  if (!out->has_position) out->fix = FixType::kNone;

  int sats = 0;
  if (parse_int(s.field(6), &sats) && sats >= 0 && sats < 128) {
    out->has_satellites = true;
    out->satellites = sats;
  }
  out->has_hdop = parse_double(s.field(7), &out->hdop);
  out->has_alt = parse_double(s.field(8), &out->alt_msl_m);
  out->has_geoid_sep = parse_double(s.field(10), &out->geoid_sep_m);
  out->has_dgps_age = parse_double(s.field(12), &out->dgps_age_s);
  int station = 0;
  if (parse_int(s.field(13), &station)) {
    out->has_station = true;
    out->station_id = station;
  }
  return true;
}

bool decode_rmc(const Sentence& s, RmcData* out) noexcept {
  if (s.id != SentenceId::kRmc || !out) return false;
  *out = RmcData{};
  if (s.field_count < 9) return false;

  out->has_time = parse_time(s.field(0), &out->utc_sod_s);
  out->valid = !s.field(1).empty() && (s.field(1)[0] == 'A' || s.field(1)[0] == 'a');

  double lat = 0.0, lon = 0.0;
  if (parse_latlon(s.field(2), s.field(3), true, &lat) &&
      parse_latlon(s.field(4), s.field(5), false, &lon)) {
    out->has_position = true;
    out->lat_deg = lat;
    out->lon_deg = lon;
  }
  out->has_speed = parse_double(s.field(6), &out->speed_knots);
  out->has_course = parse_double(s.field(7), &out->course_deg);

  const std::string_view d = s.field(8);
  if (d.size() == 6) {
    int dd = 0, mm = 0, yy = 0;
    if (parse_int(d.substr(0, 2), &dd) && parse_int(d.substr(2, 2), &mm) &&
        parse_int(d.substr(4, 2), &yy) && dd >= 1 && dd <= 31 && mm >= 1 && mm <= 12) {
      out->has_date = true;
      out->day = dd;
      out->month = mm;
      // NMEA's two-digit year. The windowing convention (00–79 → 2000s) is
      // the one every receiver and every parser uses; it breaks in 2080 and
      // is documented as such rather than silently.
      out->year = (yy < 80) ? 2000 + yy : 1900 + yy;
    }
  }
  if (s.field_count >= 12 && !s.field(11).empty()) out->mode = s.field(11)[0];
  out->fix = fix_from_mode_char(out->mode);
  if (!out->valid || !out->has_position) out->fix = FixType::kNone;
  return true;
}

bool decode_gst(const Sentence& s, GstData* out) noexcept {
  if (s.id != SentenceId::kGst || !out) return false;
  *out = GstData{};
  if (s.field_count < 8) return false;
  out->has_time = parse_time(s.field(0), &out->utc_sod_s);
  out->has_rms = parse_double(s.field(1), &out->rms_m);
  const bool a = parse_double(s.field(2), &out->semi_major_m);
  const bool b = parse_double(s.field(3), &out->semi_minor_m);
  const bool c = parse_double(s.field(4), &out->orientation_deg);
  out->has_ellipse = a && b;
  (void)c;
  const bool la = parse_double(s.field(5), &out->lat_sigma_m);
  const bool lo = parse_double(s.field(6), &out->lon_sigma_m);
  const bool al = parse_double(s.field(7), &out->alt_sigma_m);
  out->has_sigmas = la && lo && al;
  return true;
}

bool decode_gsa(const Sentence& s, GsaData* out) noexcept {
  if (s.id != SentenceId::kGsa || !out) return false;
  *out = GsaData{};
  if (s.field_count < 17) return false;
  if (!s.field(0).empty()) out->mode = s.field(0)[0];
  int ft = 0;
  if (parse_int(s.field(1), &ft)) {
    out->has_fix_type = true;
    out->fix_type = ft;
  }
  for (std::size_t i = 2; i < 14; ++i) {
    if (!s.field(i).empty()) ++out->satellites_used;
  }
  out->has_pdop = parse_double(s.field(14), &out->pdop);
  out->has_hdop = parse_double(s.field(15), &out->hdop);
  out->has_vdop = parse_double(s.field(16), &out->vdop);
  return true;
}

bool decode_vtg(const Sentence& s, VtgData* out) noexcept {
  if (s.id != SentenceId::kVtg || !out) return false;
  *out = VtgData{};
  if (s.field_count < 8) return false;
  out->has_course_true = parse_double(s.field(0), &out->course_true_deg);
  out->has_course_mag = parse_double(s.field(2), &out->course_mag_deg);
  const bool kn = parse_double(s.field(4), &out->speed_knots);
  const bool kh = parse_double(s.field(6), &out->speed_kmh);
  out->has_speed = kn || kh;
  if (kn && !kh) out->speed_kmh = out->speed_knots * 1.852;
  if (kh && !kn) out->speed_knots = out->speed_kmh / 1.852;
  if (s.field_count >= 9 && !s.field(8).empty()) out->mode = s.field(8)[0];
  return true;
}

FixType fix_from_gga_quality(int q) noexcept {
  switch (q) {
    case 1: return FixType::kSingle;
    case 2: return FixType::kDgps;
    case 3: return FixType::kSingle;   // PPS — an autonomous fix with a key
    case 4: return FixType::kRtkFixed;
    case 5: return FixType::kRtkFloat;
    case 6: return FixType::kNone;     // dead reckoning: not an observation
    case 7: return FixType::kNone;     // manual input
    case 8: return FixType::kSingle;   // simulator (bench rigs; quality_raw keeps it)
    default: break;
  }
  return FixType::kNone;
}

FixType fix_from_mode_char(char mode) noexcept {
  switch (mode) {
    case 'A': case 'a': return FixType::kSingle;
    case 'D': case 'd': return FixType::kDgps;
    case 'R': case 'r': return FixType::kRtkFixed;
    case 'F': case 'f': return FixType::kRtkFloat;
    case 'P': case 'p': return FixType::kSingle;   // precise (NMEA 4.1)
    case 'S': case 's': return FixType::kSingle;   // simulator
    case 'E': case 'e': return FixType::kNone;     // dead reckoning
    case 'M': case 'm': return FixType::kNone;     // manual
    case 'N': case 'n': default: break;
  }
  return FixType::kNone;
}

bool utc_to_unix_ns(int year, int month, int day, double sod_s,
                    std::int64_t* out_ns) noexcept {
  if (!out_ns) return false;
  if (year < 1970 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  if (!(sod_s >= 0.0 && sod_s < 86401.0)) return false;
  // days_from_civil (Howard Hinnant): proleptic Gregorian, no <ctime>, no TZ,
  // identical on all five CI legs.
  int y = year;
  y -= month <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days =
      static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
  const std::int64_t sod_ns = static_cast<std::int64_t>(sod_s * 1e9 + 0.5);
  *out_ns = days * 86400LL * 1000000000LL + sod_ns;
  return true;
}

// --- framer ----------------------------------------------------------------

NmeaFramer::NmeaFramer(const FramerConfig& cfg) : cfg_(cfg) {
  line_.reserve(cfg_.max_line_bytes);
}

void NmeaFramer::reset() {
  line_.clear();
  overflowed_ = false;
}

void NmeaFramer::push(ByteSpan bytes, std::int64_t t_arrival_ns) {
  stats_.bytes_in += bytes.size();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const char c = static_cast<char>(bytes[i]);
    if (c == '\r' || c == '\n') {
      if (!line_.empty() || overflowed_) emit_line_(t_arrival_ns);
      continue;
    }
    if (line_.empty() && c != '$' && c != '!') {
      // Outside a sentence. Binary UBX/RTCM interleaved into the NMEA stream
      // lands here; it is counted, not an error.
      ++stats_.dropped_bytes;
      continue;
    }
    if (line_.size() >= cfg_.max_line_bytes) {
      overflowed_ = true;
      ++stats_.dropped_bytes;
      continue;
    }
    line_.push_back(c);
  }
}

void NmeaFramer::emit_line_(std::int64_t t_ns) {
  const bool was_over = overflowed_;
  overflowed_ = false;
  std::string line;
  line.swap(line_);
  line_.reserve(cfg_.max_line_bytes);
  if (line.empty()) return;

  ++stats_.lines;
  if (was_over) {
    ++stats_.oversize;
    if (errors_) errors_(line, NmeaError::kTooLong, t_ns);
    return;
  }

  Sentence s;
  const NmeaError e = parse_sentence(line, &s, cfg_.parse);
  switch (e) {
    case NmeaError::kOk:
      break;
    case NmeaError::kBadChecksum:
      ++stats_.checksum_failed;
      if (errors_) errors_(line, e, t_ns);
      return;
    case NmeaError::kNoChecksum:
      ++stats_.missing_checksum;
      if (errors_) errors_(line, e, t_ns);
      return;
    case NmeaError::kTooLong:
      ++stats_.oversize;
      if (errors_) errors_(line, e, t_ns);
      return;
    default:
      ++stats_.malformed;
      if (errors_) errors_(line, e, t_ns);
      return;
  }

  ++stats_.sentences_ok;
  stats_.t_last_sentence_ns = t_ns;
  if (s.proprietary) ++stats_.proprietary;
  else if (s.id == SentenceId::kUnknown) ++stats_.unknown_type;
  if (handler_) handler_(line, s, t_ns);
}

// --- GGA construction ------------------------------------------------------

std::string build_gga(const GgaBuilderInput& in) {
  const double lat = in.lat_deg < 0 ? -in.lat_deg : in.lat_deg;
  const double lon = in.lon_deg < 0 ? -in.lon_deg : in.lon_deg;
  const int lat_d = static_cast<int>(lat);
  const int lon_d = static_cast<int>(lon);
  const double lat_m = (lat - lat_d) * 60.0;
  const double lon_m = (lon - lon_d) * 60.0;

  const double sod = (in.utc_sod_s >= 0.0 && in.utc_sod_s < 86400.0) ? in.utc_sod_s : 0.0;
  const int hh = static_cast<int>(sod / 3600.0);
  const int mm = static_cast<int>((sod - hh * 3600.0) / 60.0);
  const double ss = sod - hh * 3600.0 - mm * 60.0;

  char body[160];
  const int n = std::snprintf(
      body, sizeof(body),
      "%sGGA,%02d%02d%05.2f,%02d%07.4f,%c,%03d%07.4f,%c,%d,%02d,%.1f,%.2f,M,%.1f,M,,",
      in.talker ? in.talker : "GN", hh, mm, ss, lat_d, lat_m, in.lat_deg < 0 ? 'S' : 'N',
      lon_d, lon_m, in.lon_deg < 0 ? 'W' : 'E', in.quality, in.satellites, in.hdop,
      in.alt_msl_m, in.geoid_sep_m);
  if (n <= 0) return std::string();

  std::string s;
  s.reserve(static_cast<std::size_t>(n) + 8);
  s.push_back('$');
  s.append(body, static_cast<std::size_t>(n));
  char cs[8];
  std::snprintf(cs, sizeof(cs), "*%02X\r\n",
                checksum_of(std::string_view(body, static_cast<std::size_t>(n))));
  s.append(cs);
  return s;
}

}  // namespace nmea
}  // namespace scanengine
