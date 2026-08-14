// Every public header must be self-contained: including it alone, first,
// must compile. Seam headers (slam/, gnss/, color/, plan/, merge/, jobs/,
// export/, poses/) have no .cpp yet, so without this file a missing include
// in one of them would not be caught until A6–A15 opened the module.
#include "scanengine/cloud/page_store.h"
#include "scanengine/cloud/point_page.h"
#include "scanengine/color/colorize.h"
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
#include "scanengine/export/exporter.h"
#include "scanengine/gnss/gnss.h"
#include "scanengine/jobs/job.h"
#include "scanengine/merge/merge.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/poses/external_pose_source.h"
#include "scanengine/poses/pose_interpolator.h"
#include "scanengine/poses/pose_source.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"
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
