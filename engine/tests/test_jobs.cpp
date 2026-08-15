// test_jobs.cpp — task A15: the jobs module (Tech Spec §3.8).
//
// Four groups:
//   queue/*     JobQueue semantics against a fast, fully-controllable job
//               kind (kTransferExport occupying a BlockingColorizer-style
//               gate isn't needed here — a blocking test Colorizer plays
//               that role): FIFO within a priority, priority-before-FIFO,
//               cancel queued vs cancel running, stop() draining.
//   post/*      a REAL PostSlamPipeline run, through JobQueue, on a tiny
//               synthetic .lscan built the way test_lscan_io.cpp/test_post.cpp
//               build one — seconds-fast, no accuracy claims (those belong
//               to test_post.cpp; this is plumbing).
//   transfer/*  zip_export -> zip_import + manifest sanity report round trip,
//               plus the cancellation/include_results behaviour documented
//               in jobs/transfer.h.
//   cloud/*     CloudSubmitClient against a scripted fake HttpTransport:
//               happy path, token reject, mid-upload disconnect + resume,
//               poll to completion, result download, the MVP size cap, and
//               a JobQueue-level CloudSubmit chained from a TransferExport.
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "doctest.h"

#include "scanengine/core/event_bus.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/jobs/cloud_submit.h"
#include "scanengine/jobs/http_transport.h"
#include "scanengine/jobs/job_queue.h"
#include "scanengine/jobs/job_types.h"
#include "scanengine/jobs/colorize_wiring.h"
#include "scanengine/jobs/local_runner.h"
#include "scanengine/jobs/transfer.h"
#include "scanengine/record/lscan.h"
#include "scanengine/record/zip.h"
#include "scanengine/timesync/imu_ingest.h"

using namespace scanengine;
using namespace scanengine::jobs;

namespace {

namespace fs = std::filesystem;

// --- temp dirs ---------------------------------------------------------

std::string make_temp_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("jobs_test_") + tag + "_" + std::to_string(now) + "_" +
                      std::to_string(id));
  std::error_code ec;
  fs::remove_all(p, ec);
  return p.string();
}

struct TempDirGuard {
  std::string path;
  explicit TempDirGuard(std::string p) : path(std::move(p)) {}
  ~TempDirGuard() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDirGuard(const TempDirGuard&) = delete;
  TempDirGuard& operator=(const TempDirGuard&) = delete;
};

// --- polling helper (no test framework of our own — same discipline as
// tests/test_lio.cpp / tests/test_post.cpp: poll status(), do not sleep
// blindly) ----------------------------------------------------------------

Job wait_for_terminal(const JobQueue& q, std::uint64_t id, int timeout_ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const Job j = q.status(id);
    if (j.state == JobState::kDone || j.state == JobState::kFailed) return j;
    if (std::chrono::steady_clock::now() >= deadline) return j;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void wait_until_running(const JobQueue& q, std::uint64_t id, int timeout_ms = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (q.status(id).state == JobState::kRunning) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// --- a tiny synthetic .lscan (queue/post plumbing, not accuracy — see
// tests/test_post.cpp for the accuracy claims) -----------------------------

void put_mid360_header(std::uint8_t* buf, std::uint16_t len, std::uint16_t dot_num, std::uint16_t udp_cnt,
                        std::uint8_t data_type, std::uint64_t timestamp) {
  std::memset(buf, 0, sizeof(mid360::DataHeader));
  mid360::DataHeader h{};
  h.version = 0;
  h.length = len;
  h.time_interval = 5;
  h.dot_num = dot_num;
  h.udp_cnt = udp_cnt;
  h.frame_cnt = 0;
  h.data_type = data_type;
  h.time_type = 0;
  h.crc32 = 0;
  h.timestamp = timestamp;
  std::memcpy(buf, &h, sizeof(h));
}

// Stationary sensor: gravity-only IMU, and point packets whose points sit on
// a fixed 3 m sphere shell (no real scene geometry — this is not an
// accuracy test). Enough for PostSlamPipeline to run end to end, produce a
// nonempty final cloud, and finish in well under a second.
void write_tiny_synthetic_lscan(const std::string& dir, double duration_s = 2.2) {
  lscan::FileRecordWriter w;
  REQUIRE(w.open(dir).ok());

  const std::int64_t t0 = 1'700'000'000'000'000'000LL;
  const double points_per_sec = 4000.0;
  const double imu_hz = 200.0;
  const double dt_pkt = static_cast<double>(mid360::kPointsPerPacket) / points_per_sec;
  const double dt_imu = 1.0 / imu_hz;

  std::vector<std::uint8_t> pbuf(mid360::kPointPacketBytes);
  std::vector<std::uint8_t> ibuf(mid360::kImuPacketBytes);
  std::uint16_t udp_cnt = 0;

  double t_pkt = 0.0, t_imu = 0.0;
  while (t_pkt < duration_s || t_imu < duration_s) {
    if (t_imu <= t_pkt) {
      put_mid360_header(ibuf.data(), static_cast<std::uint16_t>(mid360::kImuPacketBytes), 1, udp_cnt++,
                         mid360::kDataTypeImu, static_cast<std::uint64_t>(t_imu * 1e9) + 1);
      mid360::ImuRaw raw{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};  // stationary; +1g on z
      std::memcpy(ibuf.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
      REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Imu, t0 + static_cast<std::int64_t>(t_imu * 1e9) + 1,
                            ByteSpan(ibuf.data(), ibuf.size()))
                  .ok());
      t_imu += dt_imu;
      continue;
    }
    put_mid360_header(pbuf.data(), static_cast<std::uint16_t>(mid360::kPointPacketBytes),
                       mid360::kPointsPerPacket, udp_cnt++, mid360::kDataTypeCartesianHigh,
                       static_cast<std::uint64_t>(t_pkt * 1e9) + 1);
    auto* pts = reinterpret_cast<mid360::CartesianHigh*>(pbuf.data() + sizeof(mid360::DataHeader));
    for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
      const double az = std::fmod(static_cast<double>(i) * 2.399963, 6.283185307179586);
      const double el =
          (-7.0 + 59.0 * std::fmod(static_cast<double>(i) * 0.0173, 1.0)) * 0.017453292519943295;
      const double r = 3.0;
      pts[i].x = static_cast<std::int32_t>(std::cos(el) * std::cos(az) * r * 1000.0);
      pts[i].y = static_cast<std::int32_t>(std::cos(el) * std::sin(az) * r * 1000.0);
      pts[i].z = static_cast<std::int32_t>(std::sin(el) * r * 1000.0);
      pts[i].reflectivity = 120;
      pts[i].tag = 0;
    }
    REQUIRE(w.write_chunk(lscan::ChunkType::kMid360Points, t0 + static_cast<std::int64_t>(t_pkt * 1e9) + 1,
                          ByteSpan(pbuf.data(), pbuf.size()))
                .ok());
    t_pkt += dt_pkt;
  }
  REQUIRE(w.close().ok());
}

post::PostConfig fast_post_config() {
  post::PostConfig cfg;
  cfg.detect_loops = false;  // plumbing test, not an accuracy/loop test
  cfg.keyframe_max_interval_s = 0.5;
  cfg.dedup.voxel_size_m = 0.05;
  // The synthetic capture's 96 points/packet repeat identically at every
  // keyframe (a fixed deterministic direction pattern, see
  // write_tiny_synthetic_lscan) and dedup down to ~96 points scattered
  // thinly over a 3 m sphere shell — too sparse and too synthetic for
  // cloud_filter.h's statistical outlier filter's k-NN distance
  // distribution to behave meaningfully (it is tuned and tested against
  // real point density in tests/test_post.cpp). Off here: this is a queue/
  // job plumbing test, not an accuracy one.
  cfg.outlier.enabled = false;
  cfg.progress_chunk_interval = 32;
  return cfg;
}

// A minimal, sealed, empty-of-streams .lscan — enough for zip_export/
// zip_import/manifest-sanity-report round-trip tests, built in microseconds.
void write_minimal_lscan(const std::string& dir) {
  lscan::FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  w.set_profile("quick");
  REQUIRE(w.close().ok());
}

// --- a Colorizer test double that blocks until released -------------------
// Deliberately implements only the ABSTRACT scanengine::Colorizer seam (not
// color::PointColorizer), so it also exercises jobs/local_runner.h's
// generic fallback path. Used as a deterministic "occupy the one worker
// thread" gate for queue ordering/cancellation tests — no timing guesses.
class BlockingColorizer final : public Colorizer {
 public:
  Status set_extrinsics(const double[16]) override { return kOkStatus; }
  Status add_keyframe(const Keyframe&) override { return kOkStatus; }
  Status colorize(PageStore*) override {
    std::unique_lock<std::mutex> lk(m_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lk, [this] { return release_; });
    return kOkStatus;
  }
  float progress() const override { return entered_ ? 0.5f : 0.f; }

  void wait_entered() {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return entered_; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lk(m_);
      release_ = true;
    }
    cv_.notify_all();
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool release_ = false;
};

JobSpec blocking_colorize_spec(BlockingColorizer* c, int priority = 0) {
  JobSpec spec;
  spec.kind = JobKind::kColorize;
  spec.priority = priority;
  spec.colorize.colorizer = c;
  spec.colorize.store = std::make_shared<PageStore>();
  return spec;
}

JobSpec cheap_transfer_spec(const std::string& project_dir, const std::string& zip_path,
                            int priority = 0) {
  JobSpec spec;
  spec.kind = JobKind::kTransferExport;
  spec.priority = priority;
  spec.transfer.project_dir = project_dir;
  spec.transfer.zip_path = zip_path;
  return spec;
}

// --- a scripted fake HttpTransport implementing docs/A15-jobs.md's REST
// contract in-memory --------------------------------------------------------

std::string find_field(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  std::size_t pos = body.find(needle);
  if (pos == std::string::npos) return "";
  pos = body.find(':', pos);
  if (pos == std::string::npos) return "";
  ++pos;
  while (pos < body.size() && (body[pos] == ' ')) ++pos;
  if (pos < body.size() && body[pos] == '"') {
    const std::size_t end = body.find('"', pos + 1);
    return body.substr(pos + 1, end - pos - 1);
  }
  std::size_t end = pos;
  while (end < body.size() && body[end] != ',' && body[end] != '}') ++end;
  return body.substr(pos, end - pos);
}

std::string body_str(const std::vector<std::uint8_t>& b) {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
std::vector<std::uint8_t> str_bytes(const std::string& s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

// Parses "bytes START-END/TOTAL" or "bytes */TOTAL".
struct ContentRange {
  bool probe = false;
  std::uint64_t start = 0, end = 0, total = 0;
  bool ok = false;
};
ContentRange parse_content_range(const std::string& v) {
  ContentRange cr;
  const std::string prefix = "bytes ";
  if (v.rfind(prefix, 0) != 0) return cr;
  std::string rest = v.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  if (slash == std::string::npos) return cr;
  const std::string range = rest.substr(0, slash);
  cr.total = std::strtoull(rest.substr(slash + 1).c_str(), nullptr, 10);
  if (range == "*") {
    cr.probe = true;
    cr.ok = true;
    return cr;
  }
  const std::size_t dash = range.find('-');
  if (dash == std::string::npos) return cr;
  cr.start = std::strtoull(range.substr(0, dash).c_str(), nullptr, 10);
  cr.end = std::strtoull(range.substr(dash + 1).c_str(), nullptr, 10);
  cr.ok = true;
  return cr;
}

class FakeCloudServer final : public HttpTransport {
 public:
  std::string expected_token = "good-token";
  std::uint64_t server_size_cap = 0;  // 0 = unlimited
  int polls_until_done = 1;
  bool server_fails_job = false;  // if true, the job settles into "failed"
  std::string result_payload = "RESULT-BYTES";

  // Arms exactly one simulated disconnect: the (0-based) chunk-th non-probe
  // PUT to /upload is stored server-side (so a resume probe reports the
  // advanced offset) but the response is dropped (transport_ok = false).
  int disconnect_on_chunk_index = -1;

  int request_count = 0;
  int chunk_seen = 0;  // ordinal of non-probe /upload PUTs seen so far

  struct JobRec {
    std::vector<std::uint8_t> received;
    std::uint64_t total_bytes = 0;
    bool upload_complete = false;
    int poll_count = 0;
    bool done = false;
  };
  std::unordered_map<std::string, JobRec> jobs;

  HttpResponse request(const HttpRequest& req) override {
    ++request_count;
    HttpResponse resp;

    const std::string* auth = find_header(req.headers, "Authorization");
    const std::string want = "Bearer " + expected_token;
    if (auth == nullptr || *auth != want) {
      resp.transport_ok = true;
      resp.status_code = 401;
      return resp;
    }

    const bool is_upload = req.url.find("/upload") != std::string::npos;
    const bool is_result = req.url.find("/result") != std::string::npos;

    if (req.method == HttpMethod::kPost && req.url.find("/jobs") != std::string::npos && !is_upload) {
      return handle_create(req);
    }
    if (req.method == HttpMethod::kPut && is_upload) {
      return handle_upload(req);
    }
    if (req.method == HttpMethod::kGet && is_result) {
      return handle_result(req);
    }
    if (req.method == HttpMethod::kGet) {
      return handle_poll(req);
    }
    resp.transport_ok = true;
    resp.status_code = 400;
    return resp;
  }

 private:
  std::string job_id_from_url(const std::string& url) const {
    const std::string marker = "/jobs/";
    const std::size_t pos = url.find(marker);
    if (pos == std::string::npos) return "";
    std::size_t start = pos + marker.size();
    std::size_t end = url.find('/', start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
  }

  HttpResponse handle_create(const HttpRequest& req) {
    HttpResponse resp;
    const std::string body = body_str(req.body);
    const std::uint64_t size_bytes = std::strtoull(find_field(body, "size_bytes").c_str(), nullptr, 10);
    if (server_size_cap != 0 && size_bytes > server_size_cap) {
      resp.transport_ok = true;
      resp.status_code = 413;
      return resp;
    }
    const std::string id = "job-" + std::to_string(jobs.size() + 1);
    JobRec rec;
    rec.total_bytes = size_bytes;
    jobs[id] = rec;
    resp.transport_ok = true;
    resp.status_code = 201;
    resp.body = str_bytes("{\"id\":\"" + id + "\",\"upload_url\":\"/jobs/" + id + "/upload\"}");
    return resp;
  }

  HttpResponse handle_upload(const HttpRequest& req) {
    HttpResponse resp;
    const std::string id = job_id_from_url(req.url);
    auto it = jobs.find(id);
    if (it == jobs.end()) {
      resp.transport_ok = true;
      resp.status_code = 404;
      return resp;
    }
    JobRec& rec = it->second;
    const std::string* range_hdr = find_header(req.headers, "Content-Range");
    if (range_hdr == nullptr) {
      resp.transport_ok = true;
      resp.status_code = 400;
      return resp;
    }
    const ContentRange cr = parse_content_range(*range_hdr);
    if (!cr.ok) {
      resp.transport_ok = true;
      resp.status_code = 400;
      return resp;
    }
    if (cr.probe) {
      resp.transport_ok = true;
      resp.status_code = 308;
      resp.headers.push_back({"Upload-Offset", std::to_string(rec.received.size())});
      return resp;
    }

    const int this_chunk = chunk_seen++;

    // Accept in-order bytes idempotently: a retry of an already-received
    // range is a no-op (still may report failure below to simulate a lost
    // ack), a genuinely new range is appended.
    if (cr.start == rec.received.size()) {
      rec.received.insert(rec.received.end(), req.body.begin(), req.body.end());
      if (rec.received.size() >= rec.total_bytes) rec.upload_complete = true;
    }

    if (this_chunk == disconnect_on_chunk_index) {
      disconnect_on_chunk_index = -1;  // one-shot
      resp.transport_ok = false;       // bytes landed; the ack is lost
      return resp;
    }

    resp.transport_ok = true;
    resp.status_code = rec.upload_complete ? 201 : 200;
    resp.headers.push_back({"Upload-Offset", std::to_string(rec.received.size())});
    return resp;
  }

  HttpResponse handle_poll(const HttpRequest& req) {
    HttpResponse resp;
    const std::string id = job_id_from_url(req.url);
    auto it = jobs.find(id);
    if (it == jobs.end()) {
      resp.transport_ok = true;
      resp.status_code = 404;
      return resp;
    }
    JobRec& rec = it->second;
    ++rec.poll_count;
    std::string state = "processing";
    float progress = static_cast<float>(rec.poll_count) / static_cast<float>(polls_until_done);
    if (rec.poll_count >= polls_until_done) {
      rec.done = true;
      progress = 1.f;
      state = server_fails_job ? "failed" : "done";
    }
    resp.transport_ok = true;
    resp.status_code = 200;
    resp.body = str_bytes("{\"id\":\"" + id + "\",\"state\":\"" + state +
                          "\",\"progress\":" + std::to_string(progress) + ",\"message\":\"\"}");
    return resp;
  }

  HttpResponse handle_result(const HttpRequest& req) {
    HttpResponse resp;
    const std::string id = job_id_from_url(req.url);
    auto it = jobs.find(id);
    if (it == jobs.end() || !it->second.done) {
      resp.transport_ok = true;
      resp.status_code = 404;
      return resp;
    }
    resp.transport_ok = true;
    resp.status_code = 200;
    resp.body = str_bytes(result_payload);
    return resp;
  }
};

}  // namespace

// ===========================================================================
// queue/* — JobQueue semantics
// ===========================================================================

TEST_CASE("queue/fifo_within_equal_priority") {
  JobQueue q;
  BlockingColorizer gate;
  const auto gate_id = q.submit(blocking_colorize_spec(&gate)).value();
  wait_until_running(q, gate_id);

  const std::string dir = make_temp_dir("fifo_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);

  std::mutex order_m;
  std::vector<std::uint64_t> order;
  auto sub = q.on_completion([&](const Job& j) {
    std::lock_guard<std::mutex> lk(order_m);
    order.push_back(j.id);
  });

  const std::string z1 = dir + "_1.zip", z2 = dir + "_2.zip";
  const auto id1 = q.submit(cheap_transfer_spec(dir, z1)).value();
  const auto id2 = q.submit(cheap_transfer_spec(dir, z2)).value();

  gate.release();
  const Job gate_final = wait_for_terminal(q, gate_id);
  CHECK(gate_final.state == JobState::kDone);
  const Job r1 = wait_for_terminal(q, id1);
  const Job r2 = wait_for_terminal(q, id2);
  CHECK(r1.state == JobState::kDone);
  CHECK(r2.state == JobState::kDone);

  std::lock_guard<std::mutex> lk(order_m);
  REQUIRE(order.size() == 3);
  CHECK(order[0] == gate_id);
  CHECK(order[1] == id1);  // FIFO: submitted first among equal priority
  CHECK(order[2] == id2);
  CHECK(q.remove_completion_listener(sub).ok());
}

TEST_CASE("queue/priority_order_is_observable_in_completion_order") {
  JobQueue q;
  BlockingColorizer gate;
  const auto gate_id = q.submit(blocking_colorize_spec(&gate)).value();
  wait_until_running(q, gate_id);

  const std::string dir = make_temp_dir("prio2_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);

  std::mutex order_m;
  std::vector<std::uint64_t> order;
  auto sub = q.on_completion([&](const Job& j) {
    std::lock_guard<std::mutex> lk(order_m);
    order.push_back(j.id);
  });

  const auto low_id = q.submit(cheap_transfer_spec(dir, dir + "_low2.zip", 0)).value();
  const auto high_id = q.submit(cheap_transfer_spec(dir, dir + "_high2.zip", 10)).value();

  gate.release();
  wait_for_terminal(q, gate_id);
  wait_for_terminal(q, low_id);
  wait_for_terminal(q, high_id);

  std::lock_guard<std::mutex> lk(order_m);
  REQUIRE(order.size() == 3);
  CHECK(order[0] == gate_id);
  CHECK(order[1] == high_id);  // priority 10 runs before priority 0
  CHECK(order[2] == low_id);
  CHECK(q.remove_completion_listener(sub).ok());
}

TEST_CASE("queue/cancel_queued_job_never_runs") {
  JobQueue q;
  BlockingColorizer gate;
  const auto gate_id = q.submit(blocking_colorize_spec(&gate)).value();
  wait_until_running(q, gate_id);

  const std::string dir = make_temp_dir("cancelq_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);
  const std::string zip_path = dir + "_never.zip";
  const auto id = q.submit(cheap_transfer_spec(dir, zip_path)).value();

  REQUIRE(q.status(id).state == JobState::kQueued);
  CHECK(q.cancel(id).ok());
  const Job j = q.status(id);
  CHECK(j.state == JobState::kFailed);
  CHECK(j.error == ScanError::kCancelled);

  gate.release();
  wait_for_terminal(q, gate_id);
  // Never ran: the zip was never produced.
  CHECK_FALSE(fs::exists(zip_path));
}

TEST_CASE("queue/cancel_running_post_process_job_stops_it") {
  // Real cancellation, wired end to end: JobQueue::cancel() -> the
  // post::CancelToken registered in run_kind_post_process() ->
  // PostSlamPipeline's own per-datagram poll (docs/A7-post.md §6.5). Duration
  // is generous (still well under a second total) to give cancel() a
  // realistic chance to land while Running rather than after the job has
  // already finished — the same race tests/test_post.cpp's own
  // cancellation test tolerates for the same reason.
  const std::string dir = make_temp_dir("cancelrun_src");
  TempDirGuard guard(dir);
  write_tiny_synthetic_lscan(dir, /*duration_s=*/8.0);

  JobQueue q;
  JobSpec spec;
  spec.kind = JobKind::kPostProcess;
  spec.post.lscan_dir = dir;
  spec.post.config = fast_post_config();
  const auto id = q.submit(spec).value();

  wait_until_running(q, id);
  CHECK(q.cancel(id).ok());
  const Job j = wait_for_terminal(q, id, 10000);
  if (j.error == ScanError::kCancelled) {
    CHECK(j.state == JobState::kFailed);
  } else {
    // The pipeline finished before cancel() could land — legitimate for a
    // tiny synthetic capture on a fast machine. Still must have settled
    // cleanly into a terminal state.
    CHECK((j.state == JobState::kDone || j.state == JobState::kFailed));
  }
}

TEST_CASE("queue/unknown_job_id_reports_not_found") {
  JobQueue q;
  const Status s = q.cancel(999999);
  CHECK_FALSE(s.ok());
  CHECK(s.error() == ScanError::kNotFound);
  CHECK(q.status(999999).id == 0);
}

TEST_CASE("queue/cancel_already_finished_job_is_invalid_state") {
  JobQueue q;
  const std::string dir = make_temp_dir("done_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);
  const auto id = q.submit(cheap_transfer_spec(dir, dir + "_x.zip")).value();
  const Job j = wait_for_terminal(q, id);
  REQUIRE(j.state == JobState::kDone);
  CHECK(q.cancel(id).error() == ScanError::kInvalidState);
}

TEST_CASE("queue/stop_finalizes_still_queued_jobs_as_cancelled") {
  JobQueue q;
  BlockingColorizer gate;
  const auto gate_id = q.submit(blocking_colorize_spec(&gate)).value();
  wait_until_running(q, gate_id);

  const std::string dir = make_temp_dir("stop_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);
  const auto queued_id = q.submit(cheap_transfer_spec(dir, dir + "_stopped.zip")).value();
  REQUIRE(q.status(queued_id).state == JobState::kQueued);

  // Release the gate concurrently with stop(): the running job finishes
  // normally, the still-queued one is finalized without ever running.
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    gate.release();
  });
  q.stop();
  releaser.join();

  CHECK(q.status(gate_id).state == JobState::kDone);
  const Job qj = q.status(queued_id);
  CHECK(qj.state == JobState::kFailed);
  CHECK(qj.error == ScanError::kCancelled);
}

TEST_CASE("queue/progress_republishes_as_kJobProgress") {
  EventBus bus;
  const Result<SubscriptionId> sub = bus.subscribe(SubscriptionOptions{});
  REQUIRE(sub.ok());

  JobQueue q(&bus);
  const std::string dir = make_temp_dir("evt_src");
  TempDirGuard guard(dir);
  write_minimal_lscan(dir);
  const auto id = q.submit(cheap_transfer_spec(dir, dir + "_evt.zip")).value();
  wait_for_terminal(q, id);

  bool saw_job_progress_for_id = false;
  Event ev;
  while (bus.poll(sub.value(), &ev)) {
    if (ev.type == EventType::kJobProgress && ev.payload.job.job_id == id) {
      saw_job_progress_for_id = true;
    }
  }
  CHECK(saw_job_progress_for_id);
}

// ===========================================================================
// post/* — a real PostSlamPipeline run, through JobQueue
// ===========================================================================

TEST_CASE("post/tiny_synthetic_lscan_runs_to_done_and_publishes_points") {
  const std::string dir = make_temp_dir("post_src");
  TempDirGuard guard(dir);
  write_tiny_synthetic_lscan(dir);

  JobQueue q;
  JobSpec spec;
  spec.kind = JobKind::kPostProcess;
  spec.post.lscan_dir = dir;
  spec.post.config = fast_post_config();
  const auto id = q.submit(spec).value();

  const Job j = wait_for_terminal(q, id, 10000);
  REQUIRE(j.state == JobState::kDone);
  CHECK(j.progress == doctest::Approx(1.0f));
  CHECK(j.error == ScanError::kOk);

  const std::shared_ptr<PageStore> store = q.produced_store(id);
  REQUIRE(store != nullptr);
  CHECK(store->total_points() > 0);
}

TEST_CASE("post/invalid_lscan_dir_fails_fast") {
  JobQueue q;
  JobSpec spec;
  spec.kind = JobKind::kPostProcess;
  spec.post.lscan_dir = "/nonexistent/definitely/not/a/dir";
  const Result<std::uint64_t> id = q.submit(spec);
  REQUIRE(id.ok());
  const Job j = wait_for_terminal(q, id.value());
  CHECK(j.state == JobState::kFailed);
  CHECK(j.error != ScanError::kOk);
}

TEST_CASE("export/chains_from_a_finished_post_process_job") {
  const std::string dir = make_temp_dir("chain_src");
  TempDirGuard guard(dir);
  write_tiny_synthetic_lscan(dir);

  JobQueue q;
  JobSpec post_spec;
  post_spec.kind = JobKind::kPostProcess;
  post_spec.post.lscan_dir = dir;
  post_spec.post.config = fast_post_config();
  const auto post_id = q.submit(post_spec).value();
  REQUIRE(wait_for_terminal(q, post_id, 10000).state == JobState::kDone);

  const std::string ply_path = dir + "_export.ply";
  JobSpec export_spec;
  export_spec.kind = JobKind::kExportPoints;
  export_spec.export_points.chain_from = post_id;
  export_spec.export_points.format = ExportFormat::kPlyBinary;
  export_spec.export_points.output_path = ply_path;
  const auto export_id = q.submit(export_spec).value();

  const Job ej = wait_for_terminal(q, export_id);
  CHECK(ej.state == JobState::kDone);
  REQUIRE(fs::exists(ply_path));
  CHECK(fs::file_size(ply_path) > 0);
}

TEST_CASE("export/chain_from_unfinished_job_fails_with_invalid_state") {
  JobQueue q;
  JobSpec export_spec;
  export_spec.kind = JobKind::kExportPoints;
  export_spec.export_points.chain_from = 42;  // never submitted
  export_spec.export_points.output_path = make_temp_dir("no_chain") + ".ply";
  const auto id = q.submit(export_spec).value();
  const Job j = wait_for_terminal(q, id);
  CHECK(j.state == JobState::kFailed);
  CHECK(j.error == ScanError::kInvalidState);
}

// ===========================================================================
// transfer/* — zip_export / zip_import round trip
// ===========================================================================

TEST_CASE("transfer/export_then_import_reports_a_sane_manifest") {
  const std::string project = make_temp_dir("xfer_src");
  const std::string dest = make_temp_dir("xfer_dst");
  TempDirGuard g1(project), g2(dest);
  write_minimal_lscan(project);

  const std::string zip_path = project + ".lscan.zip";
  TransferExportParams params;
  params.project_dir = project;
  params.zip_path = zip_path;
  float last_progress = -1.f;
  const Status st = run_transfer_export(
      params, [&](float f) { last_progress = f; }, [] { return false; });
  REQUIRE(st.ok());
  CHECK(last_progress == doctest::Approx(1.0f));
  REQUIRE(fs::exists(zip_path));

  const ImportValidationReport report = import_and_validate(zip_path, dest);
  CHECK(report.zip_import_ok);
  CHECK(report.manifest_present);
  CHECK(report.manifest_ok);
  CHECK(report.truncated_tail_chunks == 0);
  CHECK(report.crc_mismatch_chunks == 0);
  CHECK(report.sane());
}

TEST_CASE("transfer/via_job_queue_produces_a_zip_for_chaining") {
  const std::string project = make_temp_dir("xferq_src");
  TempDirGuard g1(project);
  write_minimal_lscan(project);
  const std::string zip_path = project + "_q.lscan.zip";

  JobQueue q;
  const auto id = q.submit(cheap_transfer_spec(project, zip_path)).value();
  const Job j = wait_for_terminal(q, id);
  REQUIRE(j.state == JobState::kDone);
  CHECK(q.produced_zip_path(id) == zip_path);
  REQUIRE(fs::exists(zip_path));
}

TEST_CASE("transfer/bad_zip_reports_not_sane_without_crashing") {
  const std::string dest = make_temp_dir("badzip_dst");
  TempDirGuard g(dest);
  const std::string bogus = make_temp_dir("bogus") + ".zip";
  {
    std::ofstream f(bogus, std::ios::binary);
    f << "not a zip file";
  }
  const ImportValidationReport report = import_and_validate(bogus, dest);
  CHECK_FALSE(report.zip_import_ok);
  CHECK_FALSE(report.sane());
  std::error_code ec;
  fs::remove(bogus, ec);
}

TEST_CASE("transfer/include_results_false_stages_raw_only") {
  const std::string project = make_temp_dir("stage_src");
  TempDirGuard g(project);
  lscan::FileRecordWriter w;
  REQUIRE(w.open(project).ok());
  const std::uint8_t byte = 0xAB;
  REQUIRE(w.write_chunk(lscan::ChunkType::kSessionNote, 1, ByteSpan(&byte, 1)).ok());
  REQUIRE(w.close().ok());
  // Simulate a "processed/" result file that include_results=false must not
  // carry into the bundle.
  {
    std::error_code ec;
    fs::create_directories(fs::path(project) / lscan::kProcessedDir, ec);
    std::ofstream f(fs::path(project) / lscan::kProcessedDir / "final.cloud", std::ios::binary);
    f << "pretend result bytes";
  }

  const std::string zip_path = project + "_raw.zip";
  TransferExportParams params;
  params.project_dir = project;
  params.zip_path = zip_path;
  params.include_results = false;
  REQUIRE(run_transfer_export(params, nullptr, nullptr).ok());

  const std::string dest = make_temp_dir("stage_dst");
  TempDirGuard g2(dest);
  const ImportValidationReport report = import_and_validate(zip_path, dest);
  CHECK(report.sane());
  CHECK_FALSE(fs::exists(fs::path(dest) / lscan::kProcessedDir / "final.cloud"));
  CHECK(fs::exists(fs::path(dest) / lscan::kStreamsDir));
}

// ===========================================================================
// cloud/* — CloudSubmitClient against the scripted fake transport
// ===========================================================================

CloudSubmitConfig fast_cloud_config() {
  CloudSubmitConfig cfg;
  cfg.auth_token = "good-token";
  cfg.chunk_bytes = 16;
  cfg.max_retries = 3;
  cfg.backoff_initial_ms = 0;
  cfg.backoff_max_ms = 0;
  cfg.poll_interval_ms = 0;
  return cfg;
}

std::string write_temp_file(const std::string& tag, std::size_t bytes) {
  const std::string path = make_temp_dir(tag.c_str()) + ".bin";
  std::ofstream f(path, std::ios::binary);
  for (std::size_t i = 0; i < bytes; ++i) f.put(static_cast<char>(i % 251));
  return path;
}

TEST_CASE("cloud/happy_path_submit_poll_download") {
  FakeCloudServer server;
  server.polls_until_done = 3;
  const std::string file = write_temp_file("happy", 100);

  CloudSubmitClient client(server, fast_cloud_config());
  std::vector<float> upload_progress;
  const Result<std::string> job_id =
      client.submit(file, [&](float f) { upload_progress.push_back(f); }, [] { return false; });
  REQUIRE(job_id.ok());
  CHECK_FALSE(upload_progress.empty());
  CHECK(upload_progress.back() == doctest::Approx(1.0f));

  int poll_calls = 0;
  const Result<CloudJobStatus> final_status = client.wait_until_terminal(
      job_id.value(), [&](const CloudJobStatus&) { ++poll_calls; }, [] { return false; });
  REQUIRE(final_status.ok());
  CHECK(final_status.value().state == CloudJobState::kDone);
  CHECK(poll_calls == 3);

  const std::string dest = make_temp_dir("happy_result") + ".zip";
  REQUIRE(client.download_result(job_id.value(), dest).ok());
  std::ifstream in(dest, std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(got == server.result_payload);

  std::error_code ec;
  fs::remove(file, ec);
  fs::remove(dest, ec);
}

TEST_CASE("cloud/token_reject_is_permission_denied") {
  FakeCloudServer server;
  server.expected_token = "good-token";
  const std::string file = write_temp_file("reject", 10);

  CloudSubmitConfig cfg = fast_cloud_config();
  cfg.auth_token = "wrong-token";
  CloudSubmitClient client(server, cfg);
  const Result<std::string> r = client.submit(file, nullptr, nullptr);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kPermissionDenied);

  std::error_code ec;
  fs::remove(file, ec);
}

TEST_CASE("cloud/size_cap_rejects_before_any_request") {
  FakeCloudServer server;
  const std::string file = write_temp_file("toobig", 1000);

  CloudSubmitConfig cfg = fast_cloud_config();
  cfg.max_upload_bytes = 100;  // smaller than the file
  CloudSubmitClient client(server, cfg);
  const Result<std::string> r = client.submit(file, nullptr, nullptr);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kCapacityExceeded);
  CHECK(server.request_count == 0);  // client-side cap, never touched the transport

  std::error_code ec;
  fs::remove(file, ec);
}

TEST_CASE("cloud/mid_upload_disconnect_then_resume_completes") {
  FakeCloudServer server;
  server.disconnect_on_chunk_index = 1;  // the second chunk (0-based) drops its ack
  const std::string file = write_temp_file("resume", 64);  // 64 / 16 = 4 chunks

  CloudSubmitClient client(server, fast_cloud_config());
  std::vector<float> progress;
  const Result<std::string> job_id =
      client.submit(file, [&](float f) { progress.push_back(f); }, [] { return false; });
  REQUIRE(job_id.ok());
  CHECK(progress.back() == doctest::Approx(1.0f));

  // The file landed byte-for-byte despite the simulated disconnect.
  const auto& rec = server.jobs.at(job_id.value());
  REQUIRE(rec.received.size() == 64);
  std::ifstream in(file, std::ios::binary);
  std::vector<char> expected((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(std::equal(expected.begin(), expected.end(), rec.received.begin()));
  CHECK(rec.upload_complete);

  std::error_code ec;
  fs::remove(file, ec);
}

TEST_CASE("cloud/total_outage_exhausts_retries_and_reports_network_error") {
  FakeCloudServer server;  // unused directly; kept to mirror the other cases
  class AlwaysDownTransport final : public HttpTransport {
   public:
    HttpResponse request(const HttpRequest&) override {
      HttpResponse r;
      r.transport_ok = false;
      return r;
    }
  } down;
  const std::string file = write_temp_file("down", 16);
  CloudSubmitConfig cfg = fast_cloud_config();
  cfg.max_retries = 1;
  CloudSubmitClient client(down, cfg);
  const Result<std::string> r = client.submit(file, nullptr, nullptr);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kNetworkError);  // fails at POST /jobs, before any resume logic

  std::error_code ec;
  fs::remove(file, ec);
  (void)server;
}

TEST_CASE("cloud/cancellation_during_upload_is_honoured") {
  FakeCloudServer server;
  const std::string file = write_temp_file("cancel", 64);
  CloudSubmitClient client(server, fast_cloud_config());

  std::atomic<int> calls{0};
  auto cancelled = [&] { return ++calls > 1; };  // cancel after the first chunk
  const Result<std::string> r = client.submit(file, nullptr, cancelled);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kCancelled);

  std::error_code ec;
  fs::remove(file, ec);
}

TEST_CASE("cloud/server_side_job_failure_is_reported") {
  FakeCloudServer server;
  server.polls_until_done = 1;
  server.server_fails_job = true;
  const std::string file = write_temp_file("fail", 8);
  CloudSubmitClient client(server, fast_cloud_config());
  const Result<std::string> job_id = client.submit(file, nullptr, nullptr);
  REQUIRE(job_id.ok());
  const Result<CloudJobStatus> st = client.wait_until_terminal(job_id.value(), nullptr, nullptr);
  REQUIRE(st.ok());
  CHECK(st.value().state == CloudJobState::kFailed);

  std::error_code ec;
  fs::remove(file, ec);
}

TEST_CASE("cloud/job_queue_chains_cloud_submit_from_transfer_export") {
  FakeCloudServer server;
  server.polls_until_done = 1;

  const std::string project = make_temp_dir("cloudq_src");
  TempDirGuard g(project);
  write_minimal_lscan(project);
  const std::string zip_path = project + "_cq.lscan.zip";

  JobQueue q;
  const auto transfer_id = q.submit(cheap_transfer_spec(project, zip_path)).value();
  REQUIRE(wait_for_terminal(q, transfer_id).state == JobState::kDone);

  const std::string result_dir = make_temp_dir("cloudq_result");
  TempDirGuard g2(result_dir);
  JobSpec cloud_spec;
  cloud_spec.kind = JobKind::kCloudSubmit;
  cloud_spec.cloud.transport = &server;
  cloud_spec.cloud.cloud_config = fast_cloud_config();
  cloud_spec.cloud.chain_from = transfer_id;
  cloud_spec.cloud.result_dir = result_dir;
  const auto cloud_id = q.submit(cloud_spec).value();

  const Job cj = wait_for_terminal(q, cloud_id, 10000);
  CHECK(cj.state == JobState::kDone);
  REQUIRE(fs::exists(result_dir + "/result.zip"));
  std::ifstream in(result_dir + "/result.zip", std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(got == server.result_payload);
}

TEST_CASE("cloud/job_queue_reports_no_transport_as_invalid_argument") {
  JobQueue q;
  JobSpec spec;
  spec.kind = JobKind::kCloudSubmit;
  spec.cloud.transport = nullptr;
  spec.cloud.local_zip_path = "/whatever.zip";
  const Result<std::uint64_t> r = q.submit(spec);
  CHECK_FALSE(r.ok());
  CHECK(r.error() == ScanError::kInvalidArgument);
}

// ===========================================================================
// INT-34 — closing docs/A15-jobs.md §7.3, §7.4 and §7.6
// ===========================================================================

namespace {

// A Colorizer that overrides NOTHING beyond the pure-virtual four. It is the
// proof that A15 §7.6's two new hooks are additive: this class compiles
// unchanged against the extended interface and behaves exactly as it did.
class PlainColorizer final : public Colorizer {
 public:
  Status set_extrinsics(const double[16]) override { return kOkStatus; }
  Status add_keyframe(const Keyframe&) override {
    ++keyframes;
    return kOkStatus;
  }
  Status colorize(PageStore*) override {
    ++colorize_calls;
    return kOkStatus;
  }
  float progress() const override { return 1.f; }

  int keyframes = 0;
  int colorize_calls = 0;
};

// One that DOES override them — what A11's PointColorizer now is, expressed
// as a test double so the wiring can be asserted without a scene.
class CancellableColorizer final : public Colorizer {
 public:
  Status set_extrinsics(const double[16]) override { return kOkStatus; }
  Status add_keyframe(const Keyframe&) override { return kOkStatus; }
  Status colorize(PageStore*) override {
    // Block until cancelled — which is only possible because the token
    // reached us through the abstract seam.
    for (int i = 0; i < 2000; ++i) {
      if (post::cancelled(token_)) return ScanError::kCancelled;
      if (progress_) progress_(static_cast<float>(i) / 2000.f);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return kOkStatus;
  }
  float progress() const override { return 0.f; }

  void set_cancel_token(post::CancelToken* t) override { token_ = t; }
  void set_progress_fn(ColorizeProgressFn cb) override { progress_ = std::move(cb); }

  post::CancelToken* token_ = nullptr;
  ColorizeProgressFn progress_;
};

}  // namespace

TEST_CASE("jobs/the_abstract_colorizer_seam_now_carries_cancel_and_progress") {
  auto store = std::make_shared<PageStore>();
  std::vector<PointVertex> pts(8);
  REQUIRE(store->append(StreamId::kSlamMap, Span<const PointVertex>(pts.data(), pts.size()), 1)
              .ok());

  // A plain implementation is untouched by the change: the hooks default to
  // no-ops, so run_colorize() still drives it through the bare seam.
  PlainColorizer plain;
  ColorizeParams p;
  p.colorizer = &plain;
  p.store = store;
  p.keyframes.resize(3);
  for (auto& kf : p.keyframes) kf.pose.orientation[3] = 1.0;

  std::vector<float> ticks;
  post::CancelToken never;
  CHECK(run_colorize(p, [&](float f) { ticks.push_back(f); }, &never).ok());
  CHECK(plain.keyframes == 3);
  CHECK(plain.colorize_calls == 1);
  REQUIRE(ticks.size() >= 2);
  CHECK(ticks.front() == 0.f);
  CHECK(ticks.back() == 1.f);

  // An implementation that overrides them gets REAL cancellation out of the
  // same call — which is the whole point of §7.6: before this, only a
  // dynamic_cast to color::PointColorizer could do it.
  CancellableColorizer cancellable;
  ColorizeParams p2;
  p2.colorizer = &cancellable;
  p2.store = store;

  post::CancelToken token;
  std::atomic<int> seen{0};
  std::thread worker([&] {
    const Status st = run_colorize(p2, [&](float) { seen.fetch_add(1); }, &token);
    CHECK(st.error() == ScanError::kCancelled);
  });
  while (seen.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  token.cancel();
  worker.join();
  CHECK(cancellable.token_ == &token);
  CHECK(seen.load() > 0);

  // A null colorizer still fails fast rather than silently no-op'ing.
  ColorizeParams p3;
  p3.store = store;
  CHECK(run_colorize(p3, nullptr, nullptr).error() == ScanError::kUnimplemented);
}

// docs/A15-jobs.md §7.4: zip_export()/zip_import() had no progress or cancel
// hook, so a kTransferExport job could report only start/end and could only
// be cancelled either side of a multi-minute blocking call.
TEST_CASE("transfer/zip_export_and_import_report_progress_and_honour_cancel") {
  const std::string src = make_temp_dir("zipprog");
  write_minimal_lscan(src);
  // A few hundred KiB so the copy loop iterates more than once per file and
  // the mid-file cancel poll is actually reached.
  for (int i = 0; i < 3; ++i) {
    std::ofstream f(src + "/streams/blob" + std::to_string(i) + ".bin", std::ios::binary);
    const std::vector<char> chunk(200 * 1024, static_cast<char>('a' + i));
    f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
  }

  const std::string zip = src + ".zip";
  std::vector<std::uint64_t> done;
  std::uint64_t total = 0;
  REQUIRE(lscan::zip_export(src, zip,
                            [&](std::uint64_t d, std::uint64_t t, const char* entry) {
                              CHECK(entry != nullptr);
                              done.push_back(d);
                              total = t;
                            })
              .ok());
  REQUIRE(done.size() > 4);  // more than the two end ticks
  CHECK(done.front() == 0);
  CHECK(done.back() == total);
  CHECK(total >= 600u * 1024u);
  for (std::size_t i = 1; i < done.size(); ++i) CHECK(done[i] >= done[i - 1]);  // monotone

  // Import reports against its own denominator, taken from the central
  // directory rather than from a read pass.
  const std::string dest = make_temp_dir("zipprog_out");
  std::vector<std::uint64_t> idone;
  std::uint64_t itotal = 0;
  REQUIRE(lscan::zip_import(zip, dest,
                            [&](std::uint64_t d, std::uint64_t t, const char*) {
                              idone.push_back(d);
                              itotal = t;
                            })
              .ok());
  CHECK(itotal == total);
  CHECK(idone.back() == itotal);

  // Cancelled before it starts: kCancelled, and NO half-written archive left
  // on disk — a partial zip looks openable and is not, because its central
  // directory never gets written.
  const std::string zip2 = src + "2.zip";
  lscan::ZipCancelToken pre;
  pre.request_cancel();
  CHECK(lscan::zip_export(src, zip2, nullptr, &pre).error() == ScanError::kCancelled);
  CHECK_FALSE(fs::exists(zip2));

  // Cancelled from INSIDE the copy loop — the case that did not exist before.
  const std::string zip3 = src + "3.zip";
  lscan::ZipCancelToken mid;
  CHECK(lscan::zip_export(src, zip3,
                          [&](std::uint64_t d, std::uint64_t, const char*) {
                            if (d > 64 * 1024) mid.request_cancel();
                          },
                          &mid)
            .error() == ScanError::kCancelled);
  CHECK_FALSE(fs::exists(zip3));

  // Import cancellation LEAVES what it extracted: dest_dir is the caller's
  // directory and may have had contents, so removing it is not zip_import's
  // decision (record/zip.h says so).
  const std::string dest2 = make_temp_dir("zipprog_cancel");
  lscan::ZipCancelToken icancel;
  icancel.request_cancel();
  CHECK(lscan::zip_import(zip, dest2, nullptr, &icancel).error() == ScanError::kCancelled);

  std::error_code ec;
  fs::remove_all(src, ec);
  fs::remove_all(dest, ec);
  fs::remove_all(dest2, ec);
  fs::remove(zip, ec);
}

// docs/A11-color.md §8.3's four one-liners, which nothing wired before.
TEST_CASE("jobs/colorize_wiring_connects_A4_and_A7_to_A11") {
  TimeSync ts;

  ColorizeWiring w;
  color::ColorizeConfig base;
  base.max_incidence_deg = 61.f;  // a caller's own choice must survive
  base.w_motion = 3.f;

  // Null timesync leaves the REFUSAL in place: A11 §2's fail-closed default
  // is the whole reason ColorizeConfig::sync_quality starts at kUnknown, and
  // a wiring helper that quietly assumed kGood would undo it.
  color::ColorizeConfig c0 = colorize_config_from(w, base);
  CHECK(c0.sync_quality == SyncQuality::kUnknown);
  CHECK(c0.max_incidence_deg == 61.f);
  CHECK(c0.w_motion == 3.f);

  w.timesync = &ts;
  w.sync_stream = StreamId::kLidarMid360;
  w.camera_clock_offset_ns = 23'000'000;
  w.allow_poor_sync = true;
  color::ColorizeConfig c1 = colorize_config_from(w, base);
  CHECK(c1.sync_quality == ts.quality(StreamId::kLidarMid360));
  CHECK(c1.camera_clock_offset_ns == 23'000'000);
  CHECK(c1.allow_poor_sync);
  CHECK(c1.max_incidence_deg == 61.f);

  CHECK(wire_colorizer(w, nullptr).error() == ScanError::kInvalidArgument);

  // The motion gate, end to end: a keyframe whose RECORDED rate is slow, an
  // IMU that says it was fast, and the S6 rule that the caller-supplied
  // function outranks the recording (A11 §7.3's third behaviour). The
  // rejection happens in prepare_keyframes, before any image is decoded, so
  // this needs no JPEG.
  const std::string dir = make_temp_dir("wiring");
  const std::int64_t t_kf = 5'000'000'000LL;
  {
    color::KeyframeIndexWriter kw;
    REQUIRE(kw.open(dir).ok());
    Keyframe kf;
    kf.t_mono_ns = t_kf;
    kf.image_path = "streams/frames/kf_0.jpg";
    kf.pose.t_mono_ns = t_kf;
    kf.pose.position[2] = -3.0;
    kf.pose.orientation[3] = 1.0;
    kf.pose.quality = PoseQuality::kGood;
    kf.intrinsics.fx = kf.intrinsics.fy = 300.f;
    kf.intrinsics.cx = 160.f;
    kf.intrinsics.cy = 120.f;
    kf.intrinsics.width = 320;
    kf.intrinsics.height = 240;
    kf.flags = kKeyframeFlagMotionValid;
    kf.angular_rate_rad_s = 0.05f;  // ~2.9 deg/s: comfortably inside any gate
    REQUIRE(kw.add(kf).ok());
    REQUIRE(kw.close().ok());
  }

  auto store = std::make_shared<PageStore>();
  std::vector<PointVertex> pts(16);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    pts[i].x = 0.05f * static_cast<float>(i);
    pts[i].a = 255;
  }
  REQUIRE(store->append(StreamId::kSlamMap, Span<const PointVertex>(pts.data(), pts.size()), 1)
              .ok());

  // 3 rad/s ~= 172 deg/s: past every reject threshold in the S6 table.
  ImuIngest imu(ts, StreamId::kImu);
  const float accel[3] = {0.f, 0.f, 9.80665f};
  for (int i = 0; i < 60; ++i) {
    const float gyro[3] = {3.0f, 0.f, 0.f};
    imu.add(t_kf - 200'000'000LL + i * 4'000'000LL,
            TimePoint{t_kf - 200'000'000LL + i * 4'000'000LL}, gyro, accel);
  }

  color::ColorizeConfig cc;
  cc.sync_quality = SyncQuality::kGood;
  color::PointColorizer pc(cc);
  REQUIRE(pc.load_keyframes(dir).ok());

  ColorizeWiring w2;
  w2.imu = &imu;
  REQUIRE(wire_colorizer(w2, &pc).ok());
  (void)pc.colorize(store.get());
  CHECK(pc.stats().keyframes_total == 1);
  CHECK(pc.stats().keyframes_rejected_motion == 1);  // the IMU outranked the record

  // Without the wiring the recorded 0.05 rad/s stands and the keyframe is
  // kept — the control that proves it was the IMU doing it.
  color::PointColorizer pc2(cc);
  REQUIRE(pc2.load_keyframes(dir).ok());
  (void)pc2.colorize(store.get());
  CHECK(pc2.stats().keyframes_rejected_motion == 0);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
