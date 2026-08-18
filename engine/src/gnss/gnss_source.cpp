#include "scanengine/gnss/gnss_source.h"

#include <algorithm>
#include <cmath>

#include "scanengine/core/log.h"
#include "scanengine/poses/se3.h"

namespace scanengine {
namespace {

constexpr const char* kMod = "gnss";

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// One receiver epoch: the burst of sentences sharing a UTC stamp.
struct GnssSource::Epoch {
  bool open = false;
  double sod = -1.0;
  std::int64_t t_arrival_ns = 0;
  bool have_gga = false, have_rmc = false, have_gst = false, have_gsa = false,
       have_vtg = false;
  nmea::GgaData gga{};
  nmea::RmcData rmc{};
  nmea::GstData gst{};
  nmea::GsaData gsa{};
  nmea::VtgData vtg{};
  std::string gga_line;
};

GnssSource::GnssSource(const GnssSourceConfig& cfg)
    : cfg_(cfg), framer_(cfg.framer), pending_(new Epoch()) {
  if (cfg_.capacity == 0) cfg_.capacity = 1;
  ring_.resize(cfg_.capacity);
  framer_.set_handler([this](std::string_view line, const nmea::Sentence& s, std::int64_t t) {
    on_sentence_(line, s, t);
  });
}

GnssSource::~GnssSource() = default;

PoseQuality GnssSource::quality_for(FixType f) noexcept {
  switch (f) {
    case FixType::kRtkFixed: return PoseQuality::kGood;
    case FixType::kRtkFloat: return PoseQuality::kFair;
    case FixType::kDgps: return PoseQuality::kFair;
    case FixType::kSingle: return PoseQuality::kPoor;
    case FixType::kNone: break;
  }
  return PoseQuality::kInvalid;
}

float GnssSource::confidence_for(FixType f) noexcept {
  // A GNSS receiver has no scalar confidence; the fix state IS the
  // confidence, and these are the numbers ExternalPoseConfig::min_confidence
  // (default 0.35) was calibrated against, so the same gate value means the
  // same thing whichever source a consumer holds.
  switch (f) {
    case FixType::kRtkFixed: return 1.0f;
    case FixType::kRtkFloat: return 0.7f;
    case FixType::kDgps: return 0.55f;
    case FixType::kSingle: return 0.35f;
    case FixType::kNone: break;
  }
  return 0.0f;
}

Status GnssSource::start() {
  std::lock_guard<std::mutex> lock(m_);
  running_ = true;
  return kOkStatus;
}

Status GnssSource::stop() {
  flush();
  std::lock_guard<std::mutex> lock(m_);
  running_ = false;
  return kOkStatus;
}

bool GnssSource::running() const {
  std::lock_guard<std::mutex> lock(m_);
  return running_;
}

Status GnssSource::push_pose(const Pose&) {
  return set_last_error(ScanError::kNotSupported,
                        "GnssSource: poses are derived from NMEA, not pushed; "
                        "use ExternalPoseSource for a replayed track");
}

void GnssSource::set_callback(PoseCallback cb) {
  std::lock_guard<std::mutex> lock(m_);
  pose_cb_ = std::move(cb);
}

void GnssSource::set_fix_callback(FixCallback cb) {
  std::lock_guard<std::mutex> lock(m_);
  fix_cb_ = std::move(cb);
}

Status GnssSource::set_origin(const crs::Geodetic& origin) {
  std::lock_guard<std::mutex> lock(m_);
  if (origin_set_ && !origin_explicit_) {
    return set_last_error(ScanError::kAlreadyExists,
                          "GnssSource::set_origin: origin already anchored on a fix at "
                          "%.7f,%.7f — moving it would change what every already-"
                          "published coordinate means",
                          enu_.origin.lat_deg, enu_.origin.lon_deg);
  }
  enu_ = crs::make_enu_frame(origin);
  origin_set_ = true;
  origin_explicit_ = true;
  return kOkStatus;
}

bool GnssSource::origin(crs::Geodetic* out) const {
  std::lock_guard<std::mutex> lock(m_);
  if (!origin_set_) return false;
  if (out) *out = enu_.origin;
  return true;
}

bool GnssSource::has_origin() const {
  std::lock_guard<std::mutex> lock(m_);
  return origin_set_;
}

std::string GnssSource::last_gga_sentence() const {
  std::lock_guard<std::mutex> lock(m_);
  return last_gga_;
}

GnssFix GnssSource::last_fix() const {
  std::lock_guard<std::mutex> lock(m_);
  return last_fix_;
}

GnssStats GnssSource::stats() const {
  std::lock_guard<std::mutex> lock(m_);
  GnssStats s = stats_;
  s.nmea = framer_.stats();
  if (cfg_.timesync != nullptr) {
    const TimeModel m = cfg_.timesync->model(cfg_.stream);
    s.time_converged = m.converged;
    s.time_uncertainty_ns = m.uncertainty_ns;
  }
  return s;
}

std::size_t GnssSource::fix_count() const {
  std::lock_guard<std::mutex> lock(m_);
  return count_;
}

std::vector<GnssFix> GnssSource::fixes() const {
  std::lock_guard<std::mutex> lock(m_);
  std::vector<GnssFix> out;
  out.reserve(count_);
  for (std::size_t i = 0; i < count_; ++i) out.push_back(at_locked_(i).fix);
  return out;
}

// ROUND 14 — see the header for why the ring goes with the origin.
void GnssSource::reset_frame() {
  std::lock_guard<std::mutex> lock(m_);
  head_ = count_ = 0;
  if (!origin_explicit_) {
    origin_set_ = false;
    enu_ = crs::EnuFrame{};
  }
}

void GnssSource::clear() {
  std::lock_guard<std::mutex> lock(m_);
  head_ = count_ = 0;
  last_fix_ = GnssFix{};
  last_gga_.clear();
  *pending_ = Epoch{};
  stats_ = GnssStats{};
  framer_.reset();
}

// --- ingestion -------------------------------------------------------------

Status GnssSource::push_nmea(ByteSpan sentence, std::int64_t t_mono_ns) {
  std::vector<Entry> notify;
  FixCallback fix_cb;
  PoseCallback pose_cb;
  {
    std::lock_guard<std::mutex> lock(m_);
    // Close a stale epoch before the new bytes can extend it: a receiver that
    // stopped mid-burst must not merge its last epoch into the next one.
    if (pending_->open && cfg_.epoch_timeout_ns > 0 &&
        t_mono_ns - pending_->t_arrival_ns > cfg_.epoch_timeout_ns) {
      close_epoch_locked_();
    }
    notify_.clear();
    framer_.push(sentence, t_mono_ns);
    notify.swap(notify_);
    fix_cb = fix_cb_;
    pose_cb = pose_cb_;
  }
  for (const Entry& e : notify) {
    if (fix_cb) fix_cb(e.fix);
    if (e.pose_valid && pose_cb) pose_cb(e.pose);
  }
  return kOkStatus;
}

void GnssSource::flush() {
  std::vector<Entry> notify;
  FixCallback fix_cb;
  PoseCallback pose_cb;
  {
    std::lock_guard<std::mutex> lock(m_);
    notify_.clear();
    if (pending_->open) close_epoch_locked_();
    notify.swap(notify_);
    fix_cb = fix_cb_;
    pose_cb = pose_cb_;
  }
  for (const Entry& e : notify) {
    if (fix_cb) fix_cb(e.fix);
    if (e.pose_valid && pose_cb) pose_cb(e.pose);
  }
}

void GnssSource::on_sentence_(std::string_view line, const nmea::Sentence& s,
                              std::int64_t t_ns) {
  // Sentences that carry a UTC define the epoch boundary; GSA and VTG do not
  // and simply attach to whatever epoch is open.
  double sod = -1.0;
  bool has_sod = false;

  nmea::GgaData gga{};
  nmea::RmcData rmc{};
  nmea::GstData gst{};
  nmea::GsaData gsa{};
  nmea::VtgData vtg{};

  switch (s.id) {
    case nmea::SentenceId::kGga:
      if (!nmea::decode_gga(s, &gga)) return;
      has_sod = gga.has_time;
      sod = gga.utc_sod_s;
      break;
    case nmea::SentenceId::kRmc:
      if (!nmea::decode_rmc(s, &rmc)) return;
      has_sod = rmc.has_time;
      sod = rmc.utc_sod_s;
      break;
    case nmea::SentenceId::kGst:
      if (!nmea::decode_gst(s, &gst)) return;
      has_sod = gst.has_time;
      sod = gst.utc_sod_s;
      break;
    case nmea::SentenceId::kGsa:
      if (!nmea::decode_gsa(s, &gsa)) return;
      break;
    case nmea::SentenceId::kVtg:
      if (!nmea::decode_vtg(s, &vtg)) return;
      break;
    default:
      return;
  }

  // 0.5 ms: two sentences of the same epoch always carry an identical time
  // field, so any real difference is a new epoch.
  if (has_sod && pending_->open && pending_->sod >= 0.0 &&
      std::fabs(sod - pending_->sod) > 5e-4) {
    close_epoch_locked_();
  }
  if (!pending_->open) {
    *pending_ = Epoch{};
    pending_->open = true;
    pending_->t_arrival_ns = t_ns;
  }
  if (has_sod && pending_->sod < 0.0) pending_->sod = sod;

  switch (s.id) {
    case nmea::SentenceId::kGga:
      pending_->gga = gga;
      pending_->have_gga = true;
      pending_->gga_line.assign(line.data(), line.size());
      break;
    case nmea::SentenceId::kRmc:
      pending_->rmc = rmc;
      pending_->have_rmc = true;
      break;
    case nmea::SentenceId::kGst:
      pending_->gst = gst;
      pending_->have_gst = true;
      break;
    case nmea::SentenceId::kGsa:
      pending_->gsa = gsa;
      pending_->have_gsa = true;
      break;
    case nmea::SentenceId::kVtg:
      pending_->vtg = vtg;
      pending_->have_vtg = true;
      break;
    default:
      break;
  }
}

void GnssSource::close_epoch_locked_() {
  Epoch& ep = *pending_;
  if (!ep.open) return;
  ep.open = false;
  if (!ep.have_gga && !ep.have_rmc) return;  // nothing positional in this burst
  ++stats_.epochs;

  GnssFix fix;
  fix.t_arrival_ns = ep.t_arrival_ns;

  // --- position and fix state ------------------------------------------
  if (ep.have_gga && ep.gga.has_position) {
    fix.fix = ep.gga.fix;
    fix.lat_deg = ep.gga.lat_deg;
    fix.lon_deg = ep.gga.lon_deg;
    fix.alt_m = ep.gga.has_alt ? ep.gga.alt_msl_m : 0.0;
    fix.quality_raw = static_cast<std::uint8_t>(clampd(ep.gga.quality_raw, 0, 255));
    if (ep.gga.has_geoid_sep) {
      fix.geoid_sep_m = ep.gga.geoid_sep_m;
      fix.has_geoid_sep = true;
    }
    fix.height_ellipsoid_m = fix.alt_m + fix.geoid_sep_m;
    if (ep.gga.has_hdop) fix.hdop = static_cast<float>(ep.gga.hdop);
    if (ep.gga.has_satellites) {
      fix.satellites = static_cast<std::uint8_t>(std::min(ep.gga.satellites, 255));
    }
    if (ep.gga.has_dgps_age) fix.correction_age_s = static_cast<float>(ep.gga.dgps_age_s);
    if (ep.gga.has_station) {
      fix.station_id = static_cast<std::uint16_t>(clampd(ep.gga.station_id, 0, 65535));
    }
    last_gga_ = ep.gga_line;
    if (!last_gga_.empty() && last_gga_.back() != '\n') last_gga_ += "\r\n";
  } else if (ep.have_rmc && ep.rmc.has_position && ep.rmc.valid) {
    // A receiver that emits RMC but not GGA (rare, but the Emlid "minimal"
    // NMEA profile does it) still gives a usable trajectory; the mode
    // indicator is then the only fix-state evidence there is.
    fix.fix = ep.rmc.fix;
    fix.lat_deg = ep.rmc.lat_deg;
    fix.lon_deg = ep.rmc.lon_deg;
    fix.height_ellipsoid_m = 0.0;
  } else {
    fix.fix = FixType::kNone;
  }

  // GSA/VTG/RMC extras.
  if (ep.have_gsa) {
    if (ep.gsa.has_pdop) fix.pdop = static_cast<float>(ep.gsa.pdop);
    if (ep.gsa.has_vdop) fix.vdop = static_cast<float>(ep.gsa.vdop);
    if (ep.gsa.has_hdop && fix.hdop == 0.f) fix.hdop = static_cast<float>(ep.gsa.hdop);
    if (ep.gsa.has_fix_type) {
      fix.fix_dimension = static_cast<std::uint8_t>(clampd(ep.gsa.fix_type, 0, 255));
    }
  }
  if (ep.have_rmc && ep.rmc.has_speed) {
    fix.speed_mps = static_cast<float>(ep.rmc.speed_knots * 0.514444444);
  } else if (ep.have_vtg && ep.vtg.has_speed) {
    fix.speed_mps = static_cast<float>(ep.vtg.speed_knots * 0.514444444);
  }
  if (ep.have_rmc && ep.rmc.has_course) {
    fix.course_deg = static_cast<float>(ep.rmc.course_deg);
    fix.has_course = true;
  } else if (ep.have_vtg && ep.vtg.has_course_true) {
    fix.course_deg = static_cast<float>(ep.vtg.course_true_deg);
    fix.has_course = true;
  }

  // --- uncertainty ------------------------------------------------------
  if (ep.have_gst && ep.gst.has_sigmas) {
    fix.sigma_north_m = static_cast<float>(ep.gst.lat_sigma_m);
    fix.sigma_east_m = static_cast<float>(ep.gst.lon_sigma_m);
    fix.sigma_up_m = static_cast<float>(ep.gst.alt_sigma_m);
    fix.sigma_from_gst = true;
    ++stats_.gst_epochs;
  } else if (ep.have_gst && ep.gst.has_ellipse) {
    // No per-axis sigmas but an error ellipse: use the semi-major for both
    // axes. Conservative and correct in the worst direction.
    fix.sigma_north_m = fix.sigma_east_m = static_cast<float>(ep.gst.semi_major_m);
    fix.sigma_up_m = static_cast<float>(ep.gst.semi_major_m * cfg_.vertical_sigma_ratio);
    fix.sigma_from_gst = true;
    ++stats_.gst_epochs;
  } else {
    double s = default_sigma_for_fix(fix.fix);
    if (cfg_.scale_fallback_by_hdop && fix.hdop > 0.f) {
      // HDOP is a geometry multiplier on the *ranging* error, so scaling the
      // table by it is the standard first-order model. Clamped: an HDOP of
      // 99.9 (the "no fix" filler value) must not produce a 200 m sigma that
      // then dominates every weighted average it enters.
      s *= clampd(fix.hdop, 0.5, 5.0);
    }
    fix.sigma_north_m = fix.sigma_east_m = static_cast<float>(s);
    fix.sigma_up_m = static_cast<float>(s * cfg_.vertical_sigma_ratio);
    fix.sigma_from_gst = false;
  }
  fix.sigma_horizontal_m = static_cast<float>(
      std::sqrt(0.5 * (static_cast<double>(fix.sigma_east_m) * fix.sigma_east_m +
                       static_cast<double>(fix.sigma_north_m) * fix.sigma_north_m)));

  // --- time -------------------------------------------------------------
  if (ep.have_rmc && ep.rmc.has_date) {
    year_ = ep.rmc.year;
    month_ = ep.rmc.month;
    day_ = ep.rmc.day;
    have_date_ = true;
  }
  std::int64_t utc_ns = 0;
  const double sod = ep.sod >= 0.0 ? ep.sod : -1.0;
  if (have_date_ && sod >= 0.0 && nmea::utc_to_unix_ns(year_, month_, day_, sod, &utc_ns)) {
    // Day rollover with no fresh RMC date: seconds-of-day jumping backwards
    // by more than half a day is midnight, not a receiver glitch.
    if (last_sod_ >= 0.0 && sod + 43200.0 < last_sod_) {
      utc_ns += 86400LL * 1000000000LL;
      // Roll the stored date forward too, so the NEXT epoch is right without
      // needing another RMC.
      const std::int64_t days = utc_ns / (86400LL * 1000000000LL);
      std::int64_t z = days + 719468;
      const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const std::int64_t doe = z - era * 146097;
      const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const std::int64_t y = yoe + era * 400;
      const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const std::int64_t mp = (5 * doy + 2) / 153;
      day_ = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
      month_ = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
      year_ = static_cast<int>(y + (month_ <= 2 ? 1 : 0));
    }
    fix.utc_unix_ns = utc_ns;
    last_sod_ = sod;
  } else {
    ++stats_.utc_unavailable;
  }

  if (fix.utc_unix_ns != 0 && cfg_.correlate_utc && cfg_.timesync != nullptr) {
    cfg_.timesync->add_pair(cfg_.stream, fix.utc_unix_ns, TimePoint{ep.t_arrival_ns});
    fix.t_mono_ns = cfg_.timesync->to_engine_time(cfg_.stream, fix.utc_unix_ns);
    ++stats_.utc_pairs;
  } else {
    fix.t_mono_ns = ep.t_arrival_ns;
  }

  stats_.by_fix[static_cast<std::size_t>(fix.fix)]++;

  // --- pose -------------------------------------------------------------
  Pose pose;
  bool pose_valid = false;
  if (fix.fix == FixType::kNone) {
    ++stats_.epochs_no_position;
  } else if (!fix_at_least(fix.fix, cfg_.min_fix_for_pose)) {
    ++stats_.epochs_below_gate;
  } else {
    if (!origin_set_ && fix_at_least(fix.fix, cfg_.min_fix_for_origin)) {
      crs::Geodetic o;
      o.lat_deg = fix.lat_deg;
      o.lon_deg = fix.lon_deg;
      o.height_m = fix.height_ellipsoid_m;
      enu_ = crs::make_enu_frame(o);
      origin_set_ = true;
      SCAN_LOG_INFO(kMod, "ENU origin anchored at %.8f, %.8f, %.3f m (fix=%s)",
                    o.lat_deg, o.lon_deg, o.height_m, to_string(fix.fix));
    }
    if (origin_set_) {
      crs::Geodetic g;
      g.lat_deg = fix.lat_deg;
      g.lon_deg = fix.lon_deg;
      g.height_m = fix.height_ellipsoid_m;
      const crs::Enu e = crs::geodetic_to_enu(enu_, g);
      pose.t_mono_ns = fix.t_mono_ns;
      pose.position[0] = e.e + cfg_.antenna_offset_enu[0];
      pose.position[1] = e.n + cfg_.antenna_offset_enu[1];
      pose.position[2] = e.u + cfg_.antenna_offset_enu[2];
      pose.source = cfg_.stream;
      pose.quality = quality_for(fix.fix);
      pose.position_sigma_m = fix.sigma_horizontal_m;

      // Course over ground -> yaw about +Up. Local frame is x=East, y=North,
      // so a compass course C (clockwise from North) is a yaw of (90° − C)
      // counter-clockwise from East.
      if (fix.has_course && fix.speed_mps >= cfg_.min_speed_for_heading_mps) {
        const double yaw = (90.0 - static_cast<double>(fix.course_deg)) * se3::kDegToRad;
        pose.orientation[0] = 0.0;
        pose.orientation[1] = 0.0;
        pose.orientation[2] = std::sin(yaw * 0.5);
        pose.orientation[3] = std::cos(yaw * 0.5);
        pose.orientation_sigma_deg = static_cast<float>(cfg_.heading_sigma_moving_deg);
      } else {
        se3::quat_identity(pose.orientation);
        pose.orientation_sigma_deg = static_cast<float>(cfg_.heading_sigma_static_deg);
      }
      pose_valid = true;
    }
  }

  publish_locked_(fix, pose_valid, pose);
}

void GnssSource::publish_locked_(const GnssFix& fix, bool pose_valid, const Pose& pose) {
  if (count_ > 0) {
    const Entry& newest = at_locked_(count_ - 1);
    if (fix.t_mono_ns <= newest.fix.t_mono_ns) {
      // A4 can legitimately move the mapping backwards across a resync. A
      // non-monotone trajectory would corrupt every interpolation after it,
      // so the epoch is dropped and counted rather than reordered.
      ++stats_.epochs_rejected_time;
      return;
    }
  }
  Entry e;
  e.fix = fix;
  e.pose = pose;
  e.pose_valid = pose_valid;

  if (count_ < ring_.size()) {
    ring_[(head_ + count_) % ring_.size()] = e;
    ++count_;
  } else {
    ring_[head_] = e;
    head_ = (head_ + 1) % ring_.size();
    ++stats_.overwritten;
  }
  last_fix_ = fix;
  ++stats_.fixes_published;
  if (pose_valid) ++stats_.poses_published;
  notify_.push_back(e);
}

// --- interpolation ---------------------------------------------------------

const GnssSource::Entry& GnssSource::at_locked_(std::size_t i) const {
  return ring_[(head_ + i) % ring_.size()];
}

std::ptrdiff_t GnssSource::upper_index_locked_(std::int64_t t) const {
  if (count_ == 0) return -1;
  std::size_t lo = 0, hi = count_;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (at_locked_(mid).fix.t_mono_ns <= t) lo = mid + 1;
    else hi = mid;
  }
  return static_cast<std::ptrdiff_t>(lo) - 1;
}

PoseSample GnssSource::sample_at(std::int64_t t) const {
  std::lock_guard<std::mutex> lock(m_);
  ++stats_.queries;
  PoseSample out;
  if (count_ == 0) {
    out.gate = PoseGate::kNoData;
    ++stats_.queries_gated;
    return out;
  }
  const Entry& first = at_locked_(0);
  const Entry& last = at_locked_(count_ - 1);
  if (t < first.fix.t_mono_ns) {
    out.gate = PoseGate::kBeforeFirst;
    ++stats_.queries_gated;
    return out;
  }
  if (t > last.fix.t_mono_ns) {
    if (cfg_.max_extrapolation_ns <= 0 ||
        t - last.fix.t_mono_ns > cfg_.max_extrapolation_ns) {
      out.gate = PoseGate::kFuture;
      ++stats_.queries_gated;
      return out;
    }
    out.pose = last.pose;
    out.pose.t_mono_ns = t;
    out.has_pose = last.pose_valid;
    out.confidence = confidence_for(last.fix.fix);
    out.bracket_gap_ns = 0;
    out.gate = last.pose_valid ? PoseGate::kOk : PoseGate::kLowConfidence;
    if (!out.ok()) ++stats_.queries_gated;
    return out;
  }

  const std::ptrdiff_t i = upper_index_locked_(t);
  const std::size_t ia = (i < 0) ? 0 : static_cast<std::size_t>(i);
  const Entry& a = at_locked_(ia);
  const bool exact = (a.fix.t_mono_ns == t) || (ia + 1 >= count_);
  const Entry& b = exact ? a : at_locked_(ia + 1);
  out.bracket_gap_ns = exact ? 0 : (b.fix.t_mono_ns - a.fix.t_mono_ns);

  const FixType worst = static_cast<std::uint8_t>(a.fix.fix) <=
                                static_cast<std::uint8_t>(b.fix.fix)
                            ? a.fix.fix
                            : b.fix.fix;
  out.confidence = confidence_for(worst);

  if (!a.pose_valid || !b.pose_valid) {
    // One end of the bracket has no position: the interpolation would cross
    // a fix outage. Report the nearer real pose so a diagnostic UI can draw
    // the trajectory, but gate it.
    const Entry& src = a.pose_valid ? a : b;
    if (src.pose_valid) {
      out.pose = src.pose;
      out.pose.t_mono_ns = t;
      out.has_pose = true;
    }
    out.gate = PoseGate::kTrackingLost;
    ++stats_.queries_gated;
    return out;
  }

  double u = 0.0;
  if (!exact && b.fix.t_mono_ns > a.fix.t_mono_ns) {
    u = static_cast<double>(t - a.fix.t_mono_ns) /
        static_cast<double>(b.fix.t_mono_ns - a.fix.t_mono_ns);
  }
  Pose p;
  p.t_mono_ns = t;
  for (int k = 0; k < 3; ++k) {
    p.position[k] = a.pose.position[k] + (b.pose.position[k] - a.pose.position[k]) * u;
  }
  se3::quat_slerp(a.pose.orientation, b.pose.orientation, u, p.orientation);
  p.source = cfg_.stream;
  p.quality = quality_for(worst);
  p.position_sigma_m = std::max(a.pose.position_sigma_m, b.pose.position_sigma_m);
  p.orientation_sigma_deg = std::max(a.pose.orientation_sigma_deg, b.pose.orientation_sigma_deg);
  out.pose = p;
  out.has_pose = true;

  if (out.bracket_gap_ns > cfg_.max_gap_ns) {
    out.gate = PoseGate::kStale;
  } else if (out.confidence < cfg_.min_confidence ||
             static_cast<std::uint8_t>(p.quality) <
                 static_cast<std::uint8_t>(cfg_.min_quality) ||
             p.quality == PoseQuality::kInvalid) {
    out.gate = PoseGate::kLowConfidence;
  } else {
    out.gate = PoseGate::kOk;
  }
  if (!out.ok()) ++stats_.queries_gated;
  return out;
}

Status GnssSource::pose_at(std::int64_t t, Pose* out) const {
  const PoseSample s = sample_at(t);
  if (s.gate == PoseGate::kFuture || s.gate == PoseGate::kNoData) return ScanError::kAgain;
  if (!s.has_pose) return ScanError::kNotFound;
  if (out) *out = s.pose;
  return kOkStatus;
}

bool GnssSource::time_span(std::int64_t* first_ns, std::int64_t* last_ns) const {
  std::lock_guard<std::mutex> lock(m_);
  if (count_ == 0) return false;
  if (first_ns) *first_ns = at_locked_(0).fix.t_mono_ns;
  if (last_ns) *last_ns = at_locked_(count_ - 1).fix.t_mono_ns;
  return true;
}

}  // namespace scanengine
