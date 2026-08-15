// test_job_runner.cpp — INT-34: A1's `JobRunner` seam (jobs/job.h) in front of
// A15's `JobQueue`, i.e. `jobs::QueueJobRunner` (docs/A15-jobs.md §7.3).
//
// A SEPARATE FILE, and for a concrete reason rather than tidiness: there are
// two `JobState` enums — A1's seven-value seam one in `scanengine::` and
// A15's five-value concrete one in `scanengine::jobs::` — and they coexist on
// purpose (docs/A15-jobs.md §1). `tests/test_jobs.cpp` opens BOTH namespaces
// with using-directives, so every unqualified `JobState` in it means A15's;
// including `jobs/job.h` there would make hundreds of pre-existing lines
// ambiguous. Here the seam enum is the unqualified one and A15's is spelled
// `jobs::JobState`, which is also the right emphasis for a file about the
// translation between them.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "doctest.h"

#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/jobs/job_queue.h"
#include "scanengine/jobs/job_runner_adapter.h"
#include "scanengine/record/lscan.h"

using namespace scanengine;
using scanengine::jobs::JobQueue;
using scanengine::jobs::JobRunnerOptions;
using scanengine::jobs::QueueJobRunner;

namespace {

namespace fs = std::filesystem;

std::string runner_temp_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto n = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("runner_test_") + tag + "_" + std::to_string(now) + "_" +
                      std::to_string(n));
  std::error_code ec;
  fs::create_directories(p, ec);
  return p.string();
}

// The same tiny synthetic Mid-360 capture tests/test_jobs.cpp and
// tools/engine_cli.cpp build: a stationary sensor, gravity-only IMU, points
// on a fixed 3 m sphere shell. Enough for A7's pipeline to run end to end in
// well under a second. Not a scene — tests/test_post.cpp owns accuracy.
void put_header(std::uint8_t* buf, std::uint16_t len, std::uint16_t dot_num,
                std::uint16_t udp_cnt, std::uint8_t data_type, std::uint64_t timestamp) {
  mid360::DataHeader h{};
  std::memset(&h, 0, sizeof(h));
  h.version = 0;
  h.length = len;
  h.time_interval = 5;
  h.dot_num = dot_num;
  h.udp_cnt = udp_cnt;
  h.data_type = data_type;
  h.timestamp = timestamp;
  std::memcpy(buf, &h, sizeof(h));
}

void write_tiny_lscan(const std::string& dir, double duration_s = 2.2) {
  lscan::FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::int64_t t0 = 1'700'000'000'000'000'000LL;
  const double dt_pkt = static_cast<double>(mid360::kPointsPerPacket) / 4000.0;
  const double dt_imu = 1.0 / 200.0;
  std::vector<std::uint8_t> pbuf(mid360::kPointPacketBytes);
  std::vector<std::uint8_t> ibuf(mid360::kImuPacketBytes);
  std::uint16_t udp_cnt = 0;
  double t_pkt = 0.0, t_imu = 0.0;
  while (t_pkt < duration_s || t_imu < duration_s) {
    if (t_imu <= t_pkt) {
      put_header(ibuf.data(), static_cast<std::uint16_t>(mid360::kImuPacketBytes), 1, udp_cnt++,
                 mid360::kDataTypeImu, static_cast<std::uint64_t>(t_imu * 1e9) + 1);
      const mid360::ImuRaw raw{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};
      std::memcpy(ibuf.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
      REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Imu,
                            t0 + static_cast<std::int64_t>(t_imu * 1e9) + 1,
                            ByteSpan(ibuf.data(), ibuf.size()))
                  .ok());
      t_imu += dt_imu;
      continue;
    }
    put_header(pbuf.data(), static_cast<std::uint16_t>(mid360::kPointPacketBytes),
               mid360::kPointsPerPacket, udp_cnt++, mid360::kDataTypeCartesianHigh,
               static_cast<std::uint64_t>(t_pkt * 1e9) + 1);
    auto* pts = reinterpret_cast<mid360::CartesianHigh*>(pbuf.data() + sizeof(mid360::DataHeader));
    for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
      const double az = std::fmod(static_cast<double>(i) * 2.399963, 6.283185307179586);
      const double el =
          (-7.0 + 59.0 * std::fmod(static_cast<double>(i) * 0.0173, 1.0)) * 0.017453292519943295;
      pts[i].x = static_cast<std::int32_t>(std::cos(el) * std::cos(az) * 3000.0);
      pts[i].y = static_cast<std::int32_t>(std::cos(el) * std::sin(az) * 3000.0);
      pts[i].z = static_cast<std::int32_t>(std::sin(el) * 3000.0);
      pts[i].reflectivity = 120;
      pts[i].tag = 0;
    }
    REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Points,
                          t0 + static_cast<std::int64_t>(t_pkt * 1e9) + 1,
                          ByteSpan(pbuf.data(), pbuf.size()))
                .ok());
    t_pkt += dt_pkt;
  }
  REQUIRE(w.close().ok());
}

post::PostConfig fast_config() {
  post::PostConfig cfg;
  cfg.detect_loops = false;
  cfg.keyframe_max_interval_s = 0.5;
  cfg.dedup.voxel_size_m = 0.05;
  // The synthetic cloud is far too sparse for cloud_filter.h's k-NN distance
  // distribution to mean anything, and it would delete every point. Same
  // reason tests/test_jobs.cpp turns it off.
  cfg.outlier.enabled = false;
  cfg.progress_chunk_interval = 32;
  return cfg;
}

JobStatus wait_terminal(const QueueJobRunner& runner, std::uint64_t id, int max_ms = 20000) {
  JobStatus st;
  float last = -1.f;
  for (int i = 0; i < max_ms / 5; ++i) {
    st = runner.status(id);
    CHECK(st.progress >= last);  // monotone ACROSS the hand-off, not per stage
    last = st.progress;
    if (st.state == JobState::kDone || st.state == JobState::kFailed ||
        st.state == JobState::kCancelled) {
      return st;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return st;
}

}  // namespace

// docs/A15-jobs.md §7.3: A1's JobRunner seam in front of A15's JobQueue.
TEST_CASE("jobs/the_job_runner_adapter_turns_a_JobRequest_into_a_chain") {
  const std::string dir = runner_temp_dir("runner");
  write_tiny_lscan(dir);
  const std::string out = runner_temp_dir("runner_out");

  JobQueue queue;
  JobRunnerOptions opts;
  opts.post_config = fast_config();
  opts.store = std::make_shared<PageStore>();
  opts.export_format = ExportFormat::kPlyBinary;
  opts.export_basename = "cloud";
  QueueJobRunner runner(&queue, opts);

  // A pipeline with no job kind behind it is kUnimplemented, not a silent
  // no-op and not an invented job: A12's plan and A13's merge are not job
  // kinds, and pretending otherwise would hide that.
  JobRequest plan;
  plan.mode = JobMode::kLocal;
  plan.lscan_dir = dir;
  plan.pipeline = "plan";
  CHECK(runner.submit(plan).error() == ScanError::kUnimplemented);

  JobRequest empty;
  empty.mode = JobMode::kLocal;
  CHECK(runner.submit(empty).error() == ScanError::kInvalidArgument);

  // "colorize" needs a Colorizer, and the runner says so AT SUBMIT — a mode
  // chooser greys the button out on this answer.
  JobRequest col;
  col.mode = JobMode::kLocal;
  col.lscan_dir = dir;
  col.pipeline = "colorize";
  CHECK(runner.submit(col).error() == ScanError::kUnimplemented);

  CHECK(runner.status(99999).error == ScanError::kNotFound);
  CHECK(runner.cancel(99999).error() == ScanError::kNotFound);

  // The real thing: "post then export", as ONE request with ONE id and ONE
  // monotone progress, which is what a UI wanted from JobRequest all along.
  JobRequest req;
  req.mode = JobMode::kLocal;
  req.lscan_dir = dir;
  req.output_dir = out;
  req.pipeline = "export";
  auto id = runner.submit(req);
  REQUIRE(id.ok());

  JobStatus st = wait_terminal(runner, id.value());
  INFO("runner: state " << static_cast<int>(st.state) << " progress " << st.progress << " '"
                        << st.message << "'");
  CHECK(st.state == JobState::kDone);
  CHECK(st.progress == doctest::Approx(1.f));
  CHECK(st.error == ScanError::kOk);
  CHECK(opts.store->total_points() > 0);
  REQUIRE(fs::exists(out + "/cloud.ply"));
  CHECK(fs::file_size(out + "/cloud.ply") > 256u);

  // Extract-for-transfer is one job, and the runner names the bundle.
  JobRequest tx;
  tx.mode = JobMode::kExtractForTransfer;
  tx.lscan_dir = dir;
  tx.output_dir = out;
  runner.options().transfer_basename = "session";
  auto txid = runner.submit(tx);
  REQUIRE(txid.ok());
  st = wait_terminal(runner, txid.value());
  CHECK(st.state == JobState::kDone);
  CHECK(fs::exists(out + "/session.lscan.zip"));

  // mode = kCloud without a transport is refused at submit, and the refusal
  // UNWINDS the TransferExport it had already queued — a half-built chain
  // would run its head and report kDone for a request that never completed.
  JobRequest cloud;
  cloud.mode = JobMode::kCloud;
  cloud.lscan_dir = dir;
  cloud.output_dir = out;
  CHECK(runner.submit(cloud).error() == ScanError::kUnimplemented);
  // The TransferExport the refusal had already queued was cancelled on the
  // way out: nothing is left Queued or Running, so no zip appears for a
  // request that was rejected.
  for (int i = 0; i < 400; ++i) {
    bool live = false;
    for (const jobs::Job& j : queue.list()) {
      live = live || j.state == jobs::JobState::kQueued || j.state == jobs::JobState::kRunning;
    }
    if (!live) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  for (const jobs::Job& j : queue.list()) {
    CHECK((j.state == jobs::JobState::kDone || j.state == jobs::JobState::kFailed));
  }

  queue.stop();
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(out, ec);
}

TEST_CASE("jobs/a_cancelled_chain_reports_A1s_kCancelled_not_A15s_kFailed") {
  // A15 folds cancellation into kFailed + ScanError::kCancelled (five states,
  // not six). A1's seam enum keeps them apart. The adapter is where they come
  // back apart, and a UI that shows "failed" for a user-pressed cancel is a
  // bug report waiting to happen.
  const std::string dir = runner_temp_dir("runner_cancel");
  write_tiny_lscan(dir, /*duration_s=*/8.0);

  JobQueue queue;
  JobRunnerOptions opts;
  opts.post_config = fast_config();
  QueueJobRunner runner(&queue, opts);

  JobRequest req;
  req.mode = JobMode::kLocal;
  req.lscan_dir = dir;
  auto id = runner.submit(req);
  REQUIRE(id.ok());

  // Wait until it is really running, then cancel.
  for (int i = 0; i < 2000; ++i) {
    if (runner.status(id.value()).state == JobState::kRunning) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  (void)runner.cancel(id.value());

  const JobStatus st = wait_terminal(runner, id.value());
  // kDone is a legitimate race on a fast machine — the same tolerance
  // tests/test_post.cpp's own cancellation test uses.
  CHECK((st.state == JobState::kCancelled || st.state == JobState::kDone));
  if (st.state == JobState::kCancelled) CHECK(st.error == ScanError::kCancelled);

  queue.stop();
  std::error_code ec;
  fs::remove_all(dir, ec);
}

