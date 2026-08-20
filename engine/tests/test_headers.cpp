// Every public header must be self-contained: including it alone, first,
// must compile. Seam headers (slam/, gnss/, color/, plan/, merge/, jobs/,
// export/, poses/) have no .cpp yet, so without this file a missing include
// in one of them would not be caught until A6–A15 opened the module.
#include "scanengine/cloud/page_store.h"
#include "scanengine/cloud/point_page.h"
#include "scanengine/color/clock_sweep.h"
#include "scanengine/color/colorize.h"
#include "scanengine/color/colorizer.h"
#include "scanengine/color/extrinsics_solver.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/color/image_source.h"
#include "scanengine/core/engine.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/core/log.h"
#include "scanengine/core/span.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/d6/commands.h"
#include "scanengine/drivers/d6/d6_driver.h"
#include "scanengine/drivers/d6/d6_parser.h"
#include "scanengine/drivers/driver.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/drivers/stl27l/stl27l_driver.h"
#include "scanengine/drivers/stl27l/stl27l_parser.h"
#include "scanengine/export/exporter.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/jobs/colorize_wiring.h"
#include "scanengine/jobs/job.h"
#include "scanengine/jobs/job_runner_adapter.h"
#include "scanengine/merge/merge.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/poses/pose_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
#include "scanengine/record/zip.h"
#include "scanengine/slam/eskf.h"
#include "scanengine/slam/ivox.h"
#include "scanengine/slam/lio.h"
#include "scanengine/slam/pushbroom/mount_calibration.h"
#include "scanengine/slam/pushbroom/pushbroom_assembler.h"
#include "scanengine/slam/slam.h"
#include "scanengine/timesync/clock.h"
#include "scanengine/timesync/imu_ingest.h"
#include "scanengine/timesync/offset_estimator.h"
#include "scanengine/transport/byte_source.h"
#include "scanengine/transport/udp_source.h"
#include "scanengine/transport/usb_serial_source.h"

#include <type_traits>

#include "doctest.h"

using namespace scanengine;

TEST_CASE("headers/seam_types_are_usable") {
  // Touch one type from each seam so the header is instantiated, not just
  // preprocessed.
  ExportOptions eo;
  CHECK(eo.format == ExportFormat::kPlyBinary);

  SliceOptions so;
  CHECK(so.z_min_m == 1.0f);
  CHECK(so.z_max_m == 1.5f);  // §3.6 default slice band

  JobRequest jr;
  CHECK(jr.mode == JobMode::kLocal);

  GnssFix fix;
  CHECK(fix.fix == FixType::kNone);

  Pose p;
  CHECK(p.quality == PoseQuality::kInvalid);
  CHECK(p.orientation[3] == 1.0);  // identity quaternion

  MergePair mp;
  CHECK(mp.b_from_a[0] == 1.0);
  CHECK(mp.b_from_a[15] == 1.0);

  CameraIntrinsics ci;
  CHECK(ci.rolling_shutter_row_time_ns == 0.f);
}

// A6's and A8's headers now live in this list too (docs/A8-pushbroom.md §7.3,
// docs/A6-lio.md §9). Both modules' own test files include their headers first
// and alone, so the property was already held — this is the file that
// documents it, and it is also the one place the two ImuSample declarations
// (timesync/imu_ingest.h and slam/slam.h) would collide if slam.h ever grew
// its own back.
TEST_CASE("headers/A6_and_A8_types_are_usable") {
  PoseSample s;
  CHECK(s.gate == PoseGate::kNoData);
  CHECK_FALSE(s.has_pose);
  CHECK_FALSE(s.flagged());

  ExternalPoseConfig epc;
  CHECK(epc.stream == StreamId::kPoseAr);
  CHECK(epc.max_extrapolation_ns == 0);  // never project a VIO pose forward

  PushbroomConfig pbc;
  CHECK(pbc.exclude_flagged);  // §3.3's default

  MountCalibConfig mcc;
  CHECK(mcc.gate_good_px == 12.0);
  CHECK_FALSE(mcc.robust_first_stage);  // S6 §2.3: never robust in stage 1

  MountCalibResult mcr;
  CHECK(mcr.gate == CalibGate::kUnknown);
  CHECK(mcr.split_half_px == -1.0);

  LioConfig lc;
  CHECK(lc.scan_period_s == 0.1);       // §3.3's 10 Hz
  CHECK(lc.max_correspondence_m == 1.0);
  CHECK_FALSE(lc.internal_thread);      // inline is the engine's default posture

  ImuSample imu;  // timesync/imu_ingest.h's — the one with a real producer
  CHECK(imu.t_engine_ns == 0);
  CHECK_FALSE(imu.time_converged);

  // se3.h is header-only and has no config type to touch, so touch the math.
  double m[16];
  se3::mat4_identity(m);
  CHECK(se3::mat4_is_rigid(m));
}

// A11 §8.5 asked for the four color/ headers to join this list, and INT-34
// added record/zip.h and the two new jobs/ headers alongside them. The point
// is not that the modules compile — their own test files prove that — but
// that each header compiles ALONE, as the first include of a translation
// unit, which is the property a consumer opening one for the first time
// depends on and the one nothing else checks.
TEST_CASE("headers/A11_color_and_INT34_types_are_usable") {
  color::ColorizeConfig cc;
  // The default FAILS CLOSED: A4 §7 and A11 §2 — a caller who never wired
  // the sync gate must be refused, not silently trusted.
  CHECK(cc.sync_quality == SyncQuality::kUnknown);
  CHECK(cc.pose_frame == color::KeyframePoseFrame::kCamera);
  CHECK(cc.occlusion_test);
  CHECK(cc.rolling_shutter);
  CHECK(cc.depth_scale == 0.125f);  // §5.3's coarse-buffer default
  CHECK(cc.w_motion == 2.0f);       // S6 §6.1: sync x turn rate dominates

  const color::ColorizationPolicy unknown = color::policy_for(SyncQuality::kUnknown);
  CHECK_FALSE(unknown.colorize);
  const color::ColorizationPolicy gated = color::policy_for(SyncQuality::kGated);
  CHECK(gated.colorize);
  CHECK(gated.motion_gate_deg_s == 15.f);  // S6 T8

  color::ClockSweepConfig sw;
  CHECK(sw.max_offset_ns == 100'000'000);
  CHECK(sw.resample_dt_ns == 2'000'000);

  color::FrameIndexStats fis;
  CHECK(fis.records == 0);
  CHECK(color::kKeyframeRecordFixedBytes == 160);

  color::DecodedImage img;
  CHECK(img.width == 0);

  Keyframe kf;
  CHECK(kf.flags == 0);
  CHECK_FALSE(kf.has_motion());

  // The two hooks A15 §7.6 asked for, now on the ABSTRACT seam and defaulted
  // to no-ops — which is what makes them additive.
  CHECK(std::is_abstract<Colorizer>::value);

  // INT-34's own headers.
  lscan::ZipCancelToken zct;
  CHECK_FALSE(zct.cancelled());
  zct.request_cancel();
  CHECK(zct.cancelled());

  jobs::JobRunnerOptions jro;
  CHECK(jro.priority == 0);
  CHECK(jro.export_format == ExportFormat::kPlyBinary);
  CHECK(jro.colorizer == nullptr);
  CHECK(jro.camera_from_lidar[0] == 1.0);
  CHECK(jro.camera_from_lidar[15] == 1.0);

  jobs::ColorizeWiring cw;
  CHECK(cw.timesync == nullptr);           // null leaves the refusal in place
  CHECK(cw.sync_stream == StreamId::kLidarMid360);
  CHECK(cw.imu_window_ns == 250'000'000);  // A11 §8.3's one-liner

  // INT-FINAL: the ExtrinsicsSolver seam is no longer only a seam.
  CHECK(std::is_abstract<ExtrinsicsSolver>::value);
  CHECK_FALSE(std::is_abstract<color::MountExtrinsicsSolver>::value);
  color::ExtrinsicsSolverConfig esc;
  CHECK(esc.camera_from_keyframes);
  CHECK(esc.match_tolerance_ns == 2'000'000);
  CHECK(esc.default_sigma_m == 0.02);        // the Mid-360's range noise
  CHECK(esc.cad_camera_from_lidar[0] == 1.0);
  color::BoardDetection bd;
  CHECK(bd.d == 0.0);                        // refused until the app fills it in
  CHECK(bd.sigma_m < 0.0);                   // < 0 = take the config's default
}
