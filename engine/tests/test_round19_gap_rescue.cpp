// ROUND 19 items 73/74 — the gyro-constrained gap rescue and the loss-window
// recovery, pinned on synthetic rooms whose ground truth is known to the
// millimetre.
//
// The shape of every fixture: a room painted TWICE, with the first painting
// stored in a world frame that a blind tracking loss rotated and shifted by a
// KNOWN transform T_true. The gyro stub witnesses the operator's own (small)
// rotation, exactly the way ImuDensifiedPoseSource::relative_rotation reports
// it, so the rescue's locked rotation must come out as T_true's rotation and
// the solved translation as T_true's translation — or the case says which
// gate refused and why that refusal is correct.
//
// The owner-capture truths this pins against regression:
//   scan-050  RESCUED   gyro 115.63 deg locked, 0.298 m solved,
//                       self-check 1.77 -> 1.40 cm, loop gap 5.70 -> 3.39 m
//   scan-046  REFUSED   ruler-says-worse (2.23 -> 2.46 cm) — the registration
//                       itself was excellent (sides 39.2 -> 12.3 cm), and the
//                       refusal is the seventh gate doing its job
//   scan-040  REFUSED   ruler-says-worse (2.64 -> 2.79 cm)
//   scan-039  NO ANCHOR no tracked pose exists anywhere, so no rescue —
//                       kNoAnchor is that refusal's name here

#include "microtest_shim.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/gap_rescue.h"

using namespace scanengine;
using namespace scanengine::post;

namespace {

// The witness, as the tests need it: a fixed relative rotation over any
// asked-for interval (the fixtures put all the operator's rotation across the
// gap, which is the conservative direction — a real gyro would spread it).
struct StubGyro final : reanchor::GyroBridge {
  double q[4] = {0.0, 0.0, 0.0, 1.0};
  bool ok = true;
  bool hole = false;
  bool relative_rotation(std::int64_t, std::int64_t, double q_rel[4], double* peak_rate_rad_s,
                         bool* saw_hole) const override {
    *saw_hole = hole;
    if (!ok) return false;
    for (int i = 0; i < 4; ++i) q_rel[i] = q[i];
    if (peak_rate_rad_s != nullptr) *peak_rate_rad_s = 0.0;
    return true;
  }
};

void yaw_quat(double deg, double q[4]) {
  const double rv[3] = {0.0, deg * se3::kDegToRad, 0.0};
  se3::quat_from_rotvec(rv, q);
}

struct Fixture {
  std::vector<TrajPose> poses;
  std::vector<PointVertex> cloud;
  std::vector<std::int64_t> times;
};

void apply16(const double m[16], PointVertex* v) {
  const double in[3] = {v->x, v->y, v->z};
  double o[3];
  se3::mat4_apply(m, in, o);
  v->x = static_cast<float>(o[0]);
  v->y = static_cast<float>(o[1]);
  v->z = static_cast<float>(o[2]);
}

// One painting of the room, in TRUE (frame-B) coordinates: walls at x = +-3
// and z = +-4 (y in [0, 2.5]) and a floor, on a 0.1 m grid. RECTANGULAR on
// purpose: a square room is symmetric under 90-degree yaw, and the lying-gyro
// case below needs a room where a wrong rotation cannot land on a true
// symmetry — the same reason a real flat is registrable at all. `walls`
// selects surfaces so the observability cases can starve the solver on
// purpose: bit 0 = x walls, bit 1 = z walls, bit 2 = floor.
std::vector<PointVertex> room_points(int walls) {
  std::vector<PointVertex> out;
  const double step = 0.1;
  if (walls & 1) {
    for (double s = -4.0; s <= 4.0 + 1e-9; s += step) {
      for (double y = 0.0; y <= 2.5 + 1e-9; y += step) {
        for (double x : {-3.0, 3.0}) {
          PointVertex v{};
          v.x = static_cast<float>(x);
          v.y = static_cast<float>(y);
          v.z = static_cast<float>(s);
          v.a = 255;
          out.push_back(v);
        }
      }
    }
  }
  if (walls & 2) {
    for (double s = -3.0; s <= 3.0 + 1e-9; s += step) {
      for (double y = 0.0; y <= 2.5 + 1e-9; y += step) {
        for (double z : {-4.0, 4.0}) {
          PointVertex v{};
          v.x = static_cast<float>(s);
          v.y = static_cast<float>(y);
          v.z = static_cast<float>(z);
          v.a = 255;
          out.push_back(v);
        }
      }
    }
  }
  if (walls & 4) {
    for (double x = -3.0; x <= 3.0 + 1e-9; x += step) {
      for (double z = -4.0; z <= 4.0 + 1e-9; z += step) {
        PointVertex v{};
        v.x = static_cast<float>(x);
        v.y = 0.0f;
        v.z = static_cast<float>(z);
        v.a = 255;
        out.push_back(v);
      }
    }
  }
  return out;
}

// Two paintings of the same room across a 7 s blind gap. The FIRST is stored
// through T_true^-1 (the fold the tracker's frame restart left behind); the
// second is stored as-is. The operator turned `operator_yaw_deg` during the
// gap — the gyro's story — and the anchor poses are consistent with all of it.
Fixture make_fixture(const double t_true[16], double operator_yaw_deg, int walls = 7) {
  Fixture f;
  double t_inv[16];
  se3::mat4_inverse_rigid(t_true, t_inv);

  const std::int64_t kSecond = 1'000'000'000LL;
  const std::int64_t t_gap_start = 10 * kSecond;
  const std::int64_t t_gap_end = 17 * kSecond;

  // Section A: [0, 10 s), through the fold.
  {
    std::vector<PointVertex> pts = room_points(walls);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      apply16(t_inv, &pts[i]);
      f.cloud.push_back(pts[i]);
      f.times.push_back(static_cast<std::int64_t>(
          (static_cast<double>(i) / static_cast<double>(pts.size())) * 10.0 * 1e9));
    }
  }
  // Section B: [17 s, 27 s), the frame the tracker restarted into.
  {
    std::vector<PointVertex> pts = room_points(walls);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      f.cloud.push_back(pts[i]);
      f.times.push_back(t_gap_end + static_cast<std::int64_t>(
                                        (static_cast<double>(i) /
                                         static_cast<double>(pts.size())) *
                                        10.0 * 1e9));
    }
  }

  // Anchor poses. True before-pose: identity orientation at (0, 1.4, 0) —
  // recorded in frame A, i.e. through T_true^-1. True after-pose: the
  // operator's own yaw at (0.5, 1.4, 0.3) — recorded verbatim (frame B).
  {
    TrajPose pb{};
    pb.t_ns = t_gap_start;
    double q_true[4] = {0, 0, 0, 1};
    double p_true[3] = {0.0, 1.4, 0.0};
    double m_true[16], m_a[16];
    se3::mat4_from_quat_pos(q_true, p_true, m_true);
    se3::mat4_mul(t_inv, m_true, m_a);
    double R[9], t[3], q[4];
    se3::mat4_get_rt(m_a, R, t);
    se3::matrix_to_quat(R, q);
    for (int i = 0; i < 4; ++i) pb.q[i] = q[i];
    for (int i = 0; i < 3; ++i) pb.p[i] = t[i];
    pb.quality = 3;
    pb.tracking_lost = 0;
    f.poses.push_back(pb);

    TrajPose pa{};
    pa.t_ns = t_gap_end;
    double qa[4];
    yaw_quat(operator_yaw_deg, qa);
    for (int i = 0; i < 4; ++i) pa.q[i] = qa[i];
    pa.p[0] = 0.5;
    pa.p[1] = 1.4;
    pa.p[2] = 0.3;
    pa.quality = 3;
    pa.tracking_lost = 0;
    f.poses.push_back(pa);
  }
  return f;
}

GapRescueConfig test_config(const StubGyro* gyro) {
  GapRescueConfig cfg;
  cfg.gyro = gyro;
  // The fixtures' sections are 10 s of uniform painting; the default 12 s
  // window takes all of it.
  return cfg;
}

}  // namespace

TEST_CASE("round19/rescue/a known fold is recovered to centimetres, rotation locked") {
  double t_true[16];
  {
    double q[4];
    yaw_quat(35.0, q);
    const double p[3] = {0.4, -0.1, 0.25};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 10.0);
  StubGyro gyro;
  yaw_quat(10.0, gyro.q);

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));

  CHECK(r.decision == GapRescueDecision::kRescued);
  CHECK(r.gyro_rotation_deg == doctest::Approx(10.0).epsilon(0.01));
  // The locked rotation IS the fold's rotation — not solved, constructed.
  CHECK(r.rotation_applied_deg == doctest::Approx(35.0).epsilon(0.02));
  CHECK(r.solved_axes == 3);
  CHECK(r.coarse_overlap > 0.5);

  // The proof: a frame-A point pushed through the correction lands on its
  // true position. Ground truth, not a solver metric.
  double t_inv[16];
  se3::mat4_inverse_rigid(t_true, t_inv);
  const double truth[3] = {3.0, 1.0, 2.0};
  double folded[3];
  se3::mat4_apply(t_inv, truth, folded);
  double back[3];
  se3::mat4_apply(r.correction, folded, back);
  CHECK(std::fabs(back[0] - truth[0]) < 0.05);
  CHECK(std::fabs(back[1] - truth[1]) < 0.05);
  CHECK(std::fabs(back[2] - truth[2]) < 0.05);

  // Deterministic: the same bytes in, the same transform out, bit for bit.
  const GapRescueReport r2 = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  for (int i = 0; i < 16; ++i) CHECK(r.correction[i] == r2.correction[i]);
}

TEST_CASE("round19/rescue/no gyro is a refusal by name, never a free-rotation ICP") {
  double t_true[16];
  {
    double q[4];
    yaw_quat(35.0, q);
    const double p[3] = {0.4, -0.1, 0.25};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 10.0);

  GapRescueConfig cfg = test_config(nullptr);
  const GapRescueReport none = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, cfg);
  CHECK(none.decision == GapRescueDecision::kNoGyro);

  StubGyro holed;
  holed.hole = true;
  const GapRescueReport h = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&holed));
  CHECK(h.decision == GapRescueDecision::kNoGyro);
  // Identity correction on every refusal: a careless caller multiplies by I.
  for (int i = 0; i < 16; ++i) {
    const double expect = (i % 5 == 0) ? 1.0 : 0.0;
    CHECK(none.correction[i] == expect);
    CHECK(h.correction[i] == expect);
  }
}

TEST_CASE("round19/rescue/a disowned anchor is scan-039's refusal, by name") {
  double t_true[16];
  {
    double q[4];
    yaw_quat(35.0, q);
    const double p[3] = {0.4, -0.1, 0.25};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 10.0);
  f.poses[0].tracking_lost = 1;  // the "before" side has no owned frame
  StubGyro gyro;
  yaw_quat(10.0, gyro.q);

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  CHECK(r.decision == GapRescueDecision::kNoAnchor);
}

TEST_CASE("round19/rescue/two sides that share no surface refuse as no-overlap") {
  double t_true[16];
  {
    double q[4];
    yaw_quat(35.0, q);
    const double p[3] = {0.4, -0.1, 0.25};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 10.0);
  // Move the second painting's GEOMETRY 50 m away while the after-pose stays
  // where the tracker put it — the two sides genuinely share no surface
  // within the search's reach. (Moving the pose WITH the room would be a
  // consistent 50 m frame shift, and rescuing that would be correct.)
  const std::int64_t t_gap_end = 17'000'000'000LL;
  for (std::size_t i = 0; i < f.cloud.size(); ++i) {
    if (f.times[i] >= t_gap_end) f.cloud[i].x += 50.0f;
  }
  StubGyro gyro;
  yaw_quat(10.0, gyro.q);

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  CHECK(r.decision == GapRescueDecision::kNoOverlap);
  CHECK(r.coarse_overlap < 0.25);
}

TEST_CASE("round19/rescue/only the components the walls can see are solved") {
  // Floor + x-walls only: Y and X observable, Z (the corridor) is not. The
  // fold deliberately has NO Z component, so a 2-axis solve can still be
  // exactly right — and the report must SAY it was a 2-axis solve, with the
  // weak axis named.
  double t_true[16];
  {
    double q[4];
    yaw_quat(20.0, q);
    const double p[3] = {0.3, -0.1, 0.0};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 5.0, /*walls=*/1 | 4);
  StubGyro gyro;
  yaw_quat(5.0, gyro.q);

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  // The yaw fold rotates the x-walls' normals, so "Z" here is whatever the
  // registration's weak direction really is — assert on the report's own
  // claim rather than a hand-derived axis.
  CHECK(r.decision == GapRescueDecision::kRescued);
  CHECK(r.solved_axes == 2);
  CHECK(std::fabs(r.weak_axis[1]) < 0.2);  // the weak direction is horizontal

  double t_inv[16];
  se3::mat4_inverse_rigid(t_true, t_inv);
  const double truth[3] = {3.0, 1.0, 1.0};
  double folded[3];
  se3::mat4_apply(t_inv, truth, folded);
  double back[3];
  se3::mat4_apply(r.correction, folded, back);
  CHECK(std::fabs(back[0] - truth[0]) < 0.06);
  CHECK(std::fabs(back[1] - truth[1]) < 0.06);
}

TEST_CASE("round19/rescue/one wall pair cannot rescue anything — unobservable") {
  double t_true[16];
  {
    double q[4];
    yaw_quat(0.0, q);
    const double p[3] = {0.3, 0.0, 0.0};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  // x-walls only, un-rotated: every normal is +-X, one observable direction.
  Fixture f = make_fixture(t_true, 0.0, /*walls=*/1);
  StubGyro gyro;  // identity: the operator held still

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  CHECK(r.decision == GapRescueDecision::kUnobservable);
  CHECK(r.solved_axes < 2);
}

TEST_CASE("round19/rescue/a lying gyro cannot buy an acceptance") {
  // The gyro claims 60 deg of operator rotation that never happened, so the
  // locked rotation is wrong by 50 deg. SOME gate must refuse — which one
  // depends on where the bad registration falls apart, but kRescued is the
  // one answer that must be impossible, and the correction must stay I.
  //
  // Walls only, deliberately: a floor is yaw-invariant, so a floor-heavy
  // fixture lets a wrong yaw that happens to fix the vertical offset read as
  // an improvement — which is the honest limit of what geometry can check,
  // and why the LOCK's correctness rests on the gyro's measured drift
  // (round 17: median 0.11-0.47 deg/s of clean tracking), not on the walls.
  // Vertical is left out of the fold for the same reason: with no floor the
  // walls cannot see it.
  double t_true[16];
  {
    double q[4];
    yaw_quat(35.0, q);
    const double p[3] = {0.4, 0.0, 0.25};
    se3::mat4_from_quat_pos(q, p, t_true);
  }
  Fixture f = make_fixture(t_true, 10.0, /*walls=*/1 | 2);
  StubGyro gyro;
  yaw_quat(60.0, gyro.q);

  const GapRescueReport r = rescue_gap(
      f.poses, Span<const PointVertex>(f.cloud.data(), f.cloud.size()),
      Span<const std::int64_t>(f.times.data(), f.times.size()), 0, 1, test_config(&gyro));
  CHECK(r.decision != GapRescueDecision::kRescued);
  for (int i = 0; i < 16; ++i) {
    const double expect = (i % 5 == 0) ? 1.0 : 0.0;
    CHECK(r.correction[i] == expect);
  }
}

// ===========================================================================
// ITEM 74, END TO END: a sealed container with a blind window, through the
// REAL reprocess pipeline — packets, poses, IMU, manifest and all.
// ===========================================================================
//
// The walk: a corridor (square cross-section, 2 m half-width, walls parallel
// to Z) walked at 0.2 m/s to z = 2, then BACK toward the start — blind for
// 7 s while the tracker holds its last pose — with the tracker restarting
// into a frame rotated 30 deg about Z and shifted (0.25, -0.15, 0). The gyro
// (all zeros: the operator never turned) says the 30 deg jump is not the
// operator, so the stitch pass refuses it — scan-046's shape, with ground
// truth attached. The rescue must lock the 30 deg, solve the translation the
// walls can see (X and Y; Z is the pushbroom null space and the fixture's
// fold deliberately has no Z), and the recovery must then re-admit the
// returns painted during the blindness against the interpolated path.

#include <filesystem>

#include "packet_builder.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/post/reprocess.h"

namespace {

namespace fs = std::filesystem;

std::string fresh_dir19(const char* leaf) {
  const fs::path p = fs::temp_directory_path() / "scanengine_round19" / leaf;
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

// True operator height path: out to z=2 by 10 s, back 1.0 m during the blind
// 7 s, then onward at 0.2 m/s.
double true_z19(double t_s) {
  if (t_s <= 10.0) return 0.2 * t_s;
  if (t_s <= 17.0) return 2.0 - (1.0 / 7.0) * (t_s - 10.0);
  return 1.0 - 0.2 * (t_s - 17.0);
}

// Corridor cross-section: the fan's square, 2 m half-width.
double corridor_range19(double theta_deg) {
  const double th = theta_deg * se3::kDegToRad;
  const double m = std::max(std::fabs(std::sin(th)), std::fabs(std::cos(th)));
  return 2.0 / std::max(m, 1e-6);
}

std::string write_corridor_container19() {
  const std::string dir = fresh_dir19("corridor") + "/corridor.lscan";
  lscan::FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  {
    const double ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    w.set_mount_calibration(ident);
  }

  const std::int64_t kSecond = 1'000'000'000LL;
  const double kEnd = 37.0;

  // The frame restart the blindness leaves behind: rot 30 deg about Z,
  // shifted (0.25, -0.15, 0). No Z component — the corridor cannot see one.
  double t_shift[16];
  {
    const double rv[3] = {0.0, 0.0, 30.0 * se3::kDegToRad};
    double q[4];
    se3::quat_from_rotvec(rv, q);
    const double p[3] = {0.25, -0.15, 0.0};
    se3::mat4_from_quat_pos(q, p, t_shift);
  }
  double q_shift[4];
  {
    double R[9], t[3];
    se3::mat4_get_rt(t_shift, R, t);
    se3::matrix_to_quat(R, q_shift);
  }

  struct Chunk {
    lscan::ChunkType type;
    std::int64_t t_ns;
    std::vector<std::uint8_t> payload;
  };
  std::vector<Chunk> chunks;

  // --- poses at 30 Hz -------------------------------------------------------
  for (int k = 0; k * 33 <= static_cast<int>(kEnd * 1000.0); ++k) {
    const double t_s = k * 0.033;
    lscan::PoseChunkRecord r{};
    r.source = static_cast<std::uint8_t>(StreamId::kPoseAr);
    const bool blind = t_s > 10.0 && t_s < 17.0;
    if (t_s <= 10.0) {
      r.position[2] = true_z19(t_s);
      r.orientation[3] = 1.0;
      r.quality = 3;
    } else if (blind) {
      // Frozen at the last owned pose, disowned — round 17's measured shape.
      r.position[2] = 2.0;
      r.orientation[3] = 1.0;
      r.quality = 0;
      r.tracking_lost = 1;
    } else {
      const double p_true[3] = {0.0, 0.0, true_z19(t_s)};
      double p_rec[3];
      se3::mat4_apply(t_shift, p_true, p_rec);
      for (int i = 0; i < 3; ++i) r.position[i] = p_rec[i];
      for (int i = 0; i < 4; ++i) r.orientation[i] = q_shift[i];
      r.quality = 3;
    }
    std::vector<std::uint8_t> p(lscan::kPoseChunkPayloadBytes);
    lscan::encode_pose_chunk(r, p.data());
    chunks.push_back({lscan::ChunkType::kPoseAr, static_cast<std::int64_t>(t_s * 1e9), p});
  }

  // --- the phone gyro at 200 Hz: all zeros — the operator never turned ------
  for (int k = 0; k * 5 <= static_cast<int>(kEnd * 1000.0); ++k) {
    lscan::PhoneImuChunkRecord s{};
    s.accel_m_s2[1] = 9.81f;
    std::vector<std::uint8_t> p(lscan::kPhoneImuChunkPayloadBytes);
    lscan::encode_phone_imu_chunk(s, p.data());
    chunks.push_back(
        {lscan::ChunkType::kPhoneImu, static_cast<std::int64_t>(k) * 5'000'000LL, p});
  }

  // --- the D6: 4 revolutions/s, 180 samples each, painting the corridor -----
  // One kD6Raw chunk per 18-sample packet, stamped at the packet's end. A
  // start packet first so the parser has its framing.
  {
    d6test::PacketSpec sp;
    sp.start_packet = true;
    sp.scan_freq = 10;
    sp.samples = {d6test::Sample{2000, 128, false}};
    // t = 1 us, NOT 0: a zero TimePoint means "no time" to the transport,
    // which then stamps the chunk with the HOST clock — and the parser's
    // monotonicity clamp would latch every later sample to that future value.
    chunks.push_back({lscan::ChunkType::kD6Raw, 1'000, d6test::build(sp)});
  }
  const double rev_s = 0.25;
  for (int rev = 0; rev < static_cast<int>(kEnd / rev_s); ++rev) {
    for (int pkt = 0; pkt < 10; ++pkt) {
      d6test::PacketSpec sp;
      sp.first_angle_deg = pkt * 36.0;
      sp.last_angle_deg = pkt * 36.0 + 34.0;
      for (int i = 0; i < 18; ++i) {
        const double ang = pkt * 36.0 + i * 2.0;
        const double d = corridor_range19(ang);
        sp.samples.push_back(
            {static_cast<std::uint16_t>(std::lround(d * 1000.0)), 128, false});
      }
      const double t_end = rev * rev_s + (pkt + 1) * (rev_s / 10.0);
      chunks.push_back({lscan::ChunkType::kD6Raw, static_cast<std::int64_t>(t_end * 1e9),
                        d6test::build(sp)});
    }
  }

  // Interleave in time, stable across types, the way a live recorder does.
  std::stable_sort(chunks.begin(), chunks.end(),
                   [](const Chunk& a, const Chunk& b) { return a.t_ns < b.t_ns; });
  for (const Chunk& c : chunks) {
    REQUIRE(w.write_chunk(c.type, c.t_ns, ByteSpan(c.payload.data(), c.payload.size())).ok());
  }
  REQUIRE(w.close().ok());
  return dir;
}

}  // namespace

TEST_CASE("round19/e2e/a refused blind window is rescued and its returns recovered") {
  const std::string dir = write_corridor_container19();

  ReprocessOptions opts;
  // Millimetre-exact synthetic surfaces put the ruler's before/after within
  // float noise of each other; a 2 mm tolerance keeps the vote about the map
  // rather than about the seventh decimal. Owner captures run at 0.
  opts.rescue.self_consistency_tolerance_m = 0.002;
  ReprocessReport rep;
  REQUIRE(reprocess_d6_container(dir, opts, &rep).ok());

  // The stitch pass refused the gap for scan-046's reason...
  REQUIRE(rep.stitch.gaps_examined.size() >= 1);
  bool saw_refusal_shape = false;
  for (const auto& g : rep.stitch.gaps_examined) {
    if (g.gap_s > 5.0) {
      CHECK(g.jump_rotation_deg == doctest::Approx(30.0).epsilon(0.05));
      CHECK(g.gyro_rotation_deg < 1.0);
      saw_refusal_shape = true;
    }
  }
  CHECK(saw_refusal_shape);

  // ...and the rescue took it: rotation locked at the fold's 30 deg, the
  // corridor's two observable axes solved, Z left alone by name.
  REQUIRE(rep.gaps_rescued == 1);
  const GapRescueReport* rr = nullptr;
  for (const auto& r : rep.rescues) {
    if (r.decision == GapRescueDecision::kRescued) rr = &r;
  }
  REQUIRE(rr != nullptr);
  CHECK(rr->rotation_applied_deg == doctest::Approx(30.0).epsilon(0.05));
  CHECK(rr->solved_axes == 2);
  CHECK(std::fabs(rr->weak_axis[2]) > 0.9);  // the corridor is the null space

  // The recovery re-admitted the blind window's returns, with the ruler's
  // consent, and the sidecar counts agree with the report's.
  REQUIRE(rep.recoveries.size() == 1);
  CHECK(rep.recoveries[0].candidates > 4000);
  CHECK_FALSE(rep.recoveries[0].ruler_vetoed);
  CHECK(rep.recovered_points > 3000);
  CHECK(rep.recovered_points == rep.recoveries[0].admitted);
  CHECK(rep.yield.recovered == rep.recovered_points);
  // The recovered points are IN the map file (map points > resolve output).
  CHECK(rep.points > rep.yield.resolved);
  CHECK(rep.map_written);

  // The yield audit adds up: every decoded sample is accounted for once.
  CHECK(rep.yield.samples == rep.yield.no_returns + rep.yield.out_of_window +
                                 rep.yield.no_pose + rep.yield.flagged_excluded +
                                 rep.yield.other_dropped + rep.yield.resolved);

  // With the two switches off, the round-18 behaviour is exactly back:
  // refused gap, nothing rescued, nothing recovered.
  ReprocessOptions off;
  off.rescue_gaps = false;
  off.recover_gap_points = false;
  ReprocessReport rep_off;
  REQUIRE(reprocess_d6_container(dir, off, &rep_off).ok());
  CHECK(rep_off.gaps_rescued == 0);
  CHECK(rep_off.rescues.empty());
  CHECK(rep_off.recovered_points == 0);
  CHECK(rep_off.yield.flagged_excluded > 4000);
}
