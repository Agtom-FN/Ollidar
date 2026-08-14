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
#include "scanengine/poses/pose_source.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/slam.h"
#include "scanengine/timesync/clock.h"
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
