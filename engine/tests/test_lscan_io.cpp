// FileRecordWriter / FileRecordReader / ReplaySource / zip export-import
// (task A5). test_record.cpp covers the FORMAT (framing, CRC, NullWriter
// accounting) and must stay green untouched; this file covers the real
// on-disk container: writer -> reader round trips, crash-safety (truncated
// tails, corrupt/missing manifests), and the replay == capture proof (Tech
// Spec §3 Key Rule 2) via ReplaySource feeding a live Engine.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/core/engine.h"
#include "scanengine/record/lscan.h"
#include "scanengine/record/replay.h"
#include "scanengine/record/zip.h"

using namespace scanengine;
using namespace scanengine::lscan;

namespace {

namespace fs = std::filesystem;

// --- test fixtures -----------------------------------------------------------

std::string make_temp_dir(const char* tag) {
  static std::atomic<long long> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path p = fs::temp_directory_path() /
                     (std::string("lscan_io_test_") + tag + "_" + std::to_string(now) + "_" +
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

// One revolution as the S1 packet builder emits it -- copied from
// test_engine.cpp's helper of the same shape so this file has no dependency
// on that file's internals.
std::vector<std::uint8_t> synthetic_revolution(int packets = 10, int per_packet = 40) {
  return d6test::build_revolution(packets, per_packet, /*distance_mm=*/1000,
                                  /*intensity=*/128, /*scan_freq=*/10);
}

EngineConfig small_engine_config() {
  EngineConfig cfg;
  cfg.app_name = "record-io-tests";
  cfg.log_level = LogLevel::kOff;
  cfg.points.page_capacity = 4096;
  cfg.points.max_pages = 16;
  return cfg;
}

DeviceConfig d6_config() {
  DeviceConfig dc;
  dc.kind = DeviceKind::kD6;
  dc.d6.serial.port_name = "test";
  return dc;
}

// Reads the whole of a small file into memory -- test-only helper, real
// FileRecordReader never does this for a whole stream.
std::vector<std::uint8_t> slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void overwrite(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// --- FileRecordWriter ---------------------------------------------------------

TEST_CASE("record_io/file_writer_creates_the_directory_skeleton_and_a_valid_manifest") {
  const std::string dir = make_temp_dir("skeleton");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  CHECK(w.open(dir).ok());
  CHECK(w.is_open());

  CHECK(fs::is_directory(dir));
  CHECK(fs::is_directory(dir + "/streams"));
  CHECK(fs::is_directory(dir + "/streams/frames"));
  CHECK(fs::is_directory(dir + "/processed"));
  CHECK(fs::is_directory(dir + "/merged"));
  CHECK(fs::is_directory(dir + "/exports"));
  CHECK(fs::is_regular_file(dir + "/manifest.json"));

  // A stream with no chunks written to it must not appear on disk.
  CHECK_FALSE(fs::exists(dir + "/streams/imu.bin"));

  const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(w.write_chunk(ChunkType::kD6Raw, 10, ByteSpan(payload, 8)).ok());
  CHECK(fs::is_regular_file(dir + "/streams/lidar.bin"));

  CHECK(w.close().ok());
  CHECK_FALSE(w.is_open());

  const auto manifest = slurp(dir + "/manifest.json");
  const std::string text(manifest.begin(), manifest.end());
  CHECK(text.find("\"schemaVersion\"") != std::string::npos);
  CHECK(text.find("\"sealed\": true") != std::string::npos);
}

TEST_CASE("record_io/file_writer_accounts_like_the_null_writer_and_rejects_the_same_inputs") {
  const std::string dir = make_temp_dir("accounting");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  const std::uint8_t payload[8] = {0};
  CHECK(w.write_chunk(ChunkType::kD6Raw, 1, ByteSpan(payload, 8)).error() ==
        ScanError::kInvalidState);

  CHECK(w.open(dir).ok());
  CHECK(w.open(dir).error() == ScanError::kInvalidState);

  CHECK(w.write_chunk(ChunkType::kNone, 1, ByteSpan(payload, 8)).error() ==
        ScanError::kInvalidArgument);
  CHECK(w.write_chunk(ChunkType::kD6Raw, 10, ByteSpan(payload, 8)).ok());
  CHECK(w.write_chunk(ChunkType::kD6Raw, 20, ByteSpan(payload, 8)).ok());
  CHECK(w.flush().ok());

  const RecordStats st = w.stats();
  CHECK(st.chunks_written == 2);
  CHECK(st.bytes_written == 2 * (8 + kChunkOverheadBytes));
  CHECK(st.t_first_ns == 10);
  CHECK(st.t_last_ns == 20);
  CHECK(st.flushes == 1);

  CHECK(w.close().ok());
  CHECK_FALSE(w.is_open());
  CHECK(w.close().ok());  // idempotent
}

// --- writer -> reader round trip ----------------------------------------------

TEST_CASE("record_io/reader_round_trips_chunks_across_streams_in_time_order") {
  const std::string dir = make_temp_dir("roundtrip");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());

  struct Written {
    ChunkType type;
    std::int64_t t;
    std::vector<std::uint8_t> payload;
  };
  std::vector<Written> written;
  // Three streams, interleaved with overlapping timestamp ranges -- but
  // (as any real capture is, since a single stream is arrival-ordered by
  // construction) each individual stream's own timestamps are increasing in
  // write order. That is the invariant FileRecordReader's merge relies on
  // (see its class comment): D6 is 10/60/130, GNSS is 20/90, PoseAr is
  // 40/110, written in the interleaved index order below so the on-disk
  // files are not simply "one stream after another".
  const std::int64_t times[] = {10, 20, 60, 40, 90, 110, 130};
  const ChunkType types[] = {ChunkType::kD6Raw,   ChunkType::kGnssNmea, ChunkType::kD6Raw,
                             ChunkType::kPoseAr,  ChunkType::kGnssNmea, ChunkType::kPoseAr,
                             ChunkType::kD6Raw};
  for (std::size_t i = 0; i < 7; ++i) {
    std::vector<std::uint8_t> payload(4 + i);
    for (std::size_t k = 0; k < payload.size(); ++k) payload[k] = static_cast<std::uint8_t>(i * 10 + k);
    REQUIRE(w.write_chunk(types[i], times[i], ByteSpan(payload.data(), payload.size())).ok());
    written.push_back({types[i], times[i], payload});
  }
  REQUIRE(w.close().ok());

  FileRecordReader r;
  REQUIRE(r.open(dir).ok());
  CHECK(r.manifest_present());
  CHECK(r.manifest_ok());

  std::vector<Written> read_back;
  for (;;) {
    ChunkHeader h;
    std::vector<std::uint8_t> payload;
    const Status s = r.next_chunk(&h, &payload);
    if (s.error() == ScanError::kAgain) break;
    REQUIRE(s.ok());
    CHECK(r.last_stream() == stream_of(h.type));
    read_back.push_back({h.type, h.t_mono_ns, payload});
  }

  REQUIRE(read_back.size() == written.size());
  // Global chronological order across every stream -- and since every
  // timestamp above is distinct, the exact sequence too.
  const std::int64_t expected_order[] = {10, 20, 40, 60, 90, 110, 130};
  for (std::size_t i = 0; i < read_back.size(); ++i) {
    CHECK(read_back[i].t == expected_order[i]);
  }
  for (std::size_t i = 1; i < read_back.size(); ++i) {
    CHECK(read_back[i].t >= read_back[i - 1].t);
  }
  // Every written chunk is present exactly once with its payload intact
  // (order within equal or differing streams is checked via timestamps
  // above; content-matching here is order-independent by design).
  for (const auto& want : written) {
    const auto it = std::find_if(read_back.begin(), read_back.end(), [&](const Written& got) {
      return got.type == want.type && got.t == want.t && got.payload == want.payload;
    });
    CHECK(it != read_back.end());
  }

  CHECK(r.warnings().truncated_tail_chunks == 0);
  CHECK(r.warnings().crc_mismatch_chunks == 0);

  const auto& summaries = r.stream_summaries();
  std::uint64_t total_chunks = 0;
  for (const auto& s : summaries) total_chunks += s.chunk_count;
  CHECK(total_chunks == written.size());

  CHECK(r.close().ok());
}

TEST_CASE("record_io/reader_open_fails_cleanly_on_a_nonexistent_directory") {
  FileRecordReader r;
  const Status s = r.open("/no/such/lscan/dir/at/all");
  CHECK_FALSE(s.ok());
  CHECK(s.error() == ScanError::kFileError);
}

// --- crash safety --------------------------------------------------------------

TEST_CASE("record_io/reader_recovers_everything_before_a_truncated_tail") {
  const std::string dir = make_temp_dir("truncate");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  constexpr int kChunkCount = 6;
  for (int i = 0; i < kChunkCount; ++i) {
    REQUIRE(w.write_chunk(ChunkType::kD6Raw, 100 + i, ByteSpan(payload, 8)).ok());
  }
  REQUIRE(w.close().ok());

  const std::string stream_path = dir + "/streams/lidar.bin";
  const auto full = slurp(stream_path);
  constexpr std::size_t kFrameBytes = kChunkHeaderBytes + 8 + kChunkTrailerBytes;  // 28
  REQUIRE(full.size() == kStreamHeaderBytes + kChunkCount * kFrameBytes);

  // "Kill mid-write" at several points: mid-header, mid-payload, mid-crc,
  // and exactly at a frame boundary (a clean stop, not a crash).
  struct Case {
    const char* label;
    std::size_t cut_after_bytes;  // absolute offset into the file
    int expected_good_chunks;
    bool expect_truncation_warning;
  };
  const std::vector<Case> cases = {
      {"mid header of chunk 3", kStreamHeaderBytes + 3 * kFrameBytes + 5, 3, true},
      {"mid payload of chunk 2", kStreamHeaderBytes + 2 * kFrameBytes + kChunkHeaderBytes + 4, 2,
       true},
      {"mid crc trailer of chunk 5",
       kStreamHeaderBytes + 5 * kFrameBytes + kChunkHeaderBytes + 8 + 2, 5, true},
      {"exact frame boundary (clean stop, not a crash)", kStreamHeaderBytes + 4 * kFrameBytes, 4,
       false},
      {"immediately after the stream header (nothing written yet)", kStreamHeaderBytes, 0, false},
  };

  for (const auto& c : cases) {
    INFO("truncation case: " << c.label);
    overwrite(stream_path, std::vector<std::uint8_t>(full.begin(), full.begin() + static_cast<long>(c.cut_after_bytes)));

    FileRecordReader r;
    REQUIRE(r.open(dir).ok());

    int good = 0;
    for (;;) {
      ChunkHeader h;
      std::vector<std::uint8_t> payload;
      const Status s = r.next_chunk(&h, &payload);
      if (s.error() == ScanError::kAgain) break;
      REQUIRE(s.ok());
      CHECK(payload.size() == 8);
      ++good;
    }
    CHECK(good == c.expected_good_chunks);
    if (c.expect_truncation_warning) {
      CHECK(r.warnings().truncated_tail_chunks == 1);
    } else {
      CHECK(r.warnings().truncated_tail_chunks == 0);
    }
    CHECK(r.warnings().crc_mismatch_chunks == 0);
    CHECK(r.close().ok());
  }

  // Restore the full file for the next assertion.
  overwrite(stream_path, full);
}

TEST_CASE("record_io/reader_stops_at_a_corrupted_chunk_and_never_reads_past_it") {
  const std::string dir = make_temp_dir("corrupt_crc");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::uint8_t payload[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  constexpr int kChunkCount = 5;
  for (int i = 0; i < kChunkCount; ++i) {
    REQUIRE(w.write_chunk(ChunkType::kD6Raw, 100 + i, ByteSpan(payload, 8)).ok());
  }
  REQUIRE(w.close().ok());

  const std::string stream_path = dir + "/streams/lidar.bin";
  auto bytes = slurp(stream_path);
  constexpr std::size_t kFrameBytes = kChunkHeaderBytes + 8 + kChunkTrailerBytes;
  // Flip one payload byte inside chunk index 3 (0-based) -- the header and
  // length are untouched, only the CRC no longer matches.
  const std::size_t victim = kStreamHeaderBytes + 3 * kFrameBytes + kChunkHeaderBytes + 2;
  bytes[victim] ^= 0xFF;
  overwrite(stream_path, bytes);

  FileRecordReader r;
  REQUIRE(r.open(dir).ok());
  int good = 0;
  for (;;) {
    ChunkHeader h;
    std::vector<std::uint8_t> payload;
    const Status s = r.next_chunk(&h, &payload);
    if (s.error() == ScanError::kAgain) break;
    REQUIRE(s.ok());
    ++good;
  }
  CHECK(good == 3);  // chunks 0,1,2 -- chunk 3's CRC fails, chunk 4 is never reached
  CHECK(r.warnings().crc_mismatch_chunks == 1);
  CHECK(r.warnings().truncated_tail_chunks == 0);
  CHECK(r.close().ok());
}

TEST_CASE("record_io/reader_tolerates_missing_empty_and_corrupt_manifests") {
  const std::string dir = make_temp_dir("manifest");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::uint8_t payload[4] = {1, 2, 3, 4};
  REQUIRE(w.write_chunk(ChunkType::kD6Raw, 1, ByteSpan(payload, 4)).ok());
  REQUIRE(w.close().ok());

  const std::string manifest_path = dir + "/manifest.json";

  SUBCASE("missing manifest") {
    fs::remove(manifest_path);
    FileRecordReader r;
    REQUIRE(r.open(dir).ok());  // data streams are authoritative, not the manifest
    CHECK_FALSE(r.manifest_present());
    CHECK_FALSE(r.manifest_ok());
    ChunkHeader h;
    std::vector<std::uint8_t> pl;
    CHECK(r.next_chunk(&h, &pl).ok());
    CHECK(pl.size() == 4);
    CHECK(r.close().ok());
  }

  SUBCASE("empty manifest") {
    overwrite(manifest_path, {});
    FileRecordReader r;
    REQUIRE(r.open(dir).ok());
    CHECK(r.manifest_present());
    CHECK_FALSE(r.manifest_ok());
    ChunkHeader h;
    std::vector<std::uint8_t> pl;
    CHECK(r.next_chunk(&h, &pl).ok());
    CHECK(r.close().ok());
  }

  SUBCASE("garbage / unbalanced manifest") {
    const std::string garbage = "{ this is not json, and never closes";
    overwrite(manifest_path, std::vector<std::uint8_t>(garbage.begin(), garbage.end()));
    FileRecordReader r;
    REQUIRE(r.open(dir).ok());
    CHECK(r.manifest_present());
    CHECK_FALSE(r.manifest_ok());
    ChunkHeader h;
    std::vector<std::uint8_t> pl;
    CHECK(r.next_chunk(&h, &pl).ok());
    CHECK(r.close().ok());
  }
}

// --- replay == capture ---------------------------------------------------------

TEST_CASE("record_io/replay_reproduces_the_live_decoded_point_stream_bit_for_bit") {
  const std::string dir = make_temp_dir("replay_capture");
  TempDirGuard guard(dir);

  // --- "live capture" pass -----------------------------------------------
  // Decode directly through Engine::push_serial_bytes() while independently
  // recording the exact same bytes into a real .lscan container the same
  // way Engine's own recorder call does it today
  // (write_chunk(kD6Raw, t, bytes) before parsing, see src/core/engine.cpp)
  // -- see docs/A5-lscan.md for why this test does not require
  // Engine::Impl's built-in NullRecordWriter to be swapped for this proof.
  auto live_engine = Engine::create(small_engine_config());
  REQUIRE(live_engine.ok());
  Engine& live = *live_engine.value();
  auto live_id = live.add_device(d6_config());
  REQUIRE(live_id.ok());
  SessionConfig live_sc;
  live_sc.record = false;
  REQUIRE(live.start_session(live_sc).ok());

  FileRecordWriter writer;
  REQUIRE(writer.open(dir).ok());

  auto bytes = synthetic_revolution();
  const auto second = synthetic_revolution();
  bytes.insert(bytes.end(), second.begin(), second.end());

  std::size_t off = 0;
  const std::size_t chunk_sizes[] = {7, 13, 100, 3, 512};
  int k = 0;
  std::int64_t t = 1'000'000;  // arbitrary ns base, deterministic 1ms steps
  while (off < bytes.size()) {
    const std::size_t n = std::min(chunk_sizes[k++ % 5], bytes.size() - off);
    const ByteSpan span(bytes.data() + off, n);
    REQUIRE(live.push_serial_bytes(live_id.value(), span, TimePoint{t}).ok());
    REQUIRE(writer.write_chunk(ChunkType::kD6Raw, t, span).ok());
    off += n;
    t += 1'000'000;
  }
  REQUIRE(writer.close().ok());
  REQUIRE(live.stop_session().ok());

  // --- replay pass ---------------------------------------------------------
  auto replay_engine = Engine::create(small_engine_config());
  REQUIRE(replay_engine.ok());
  Engine& replay = *replay_engine.value();
  auto replay_id = replay.add_device(d6_config());
  REQUIRE(replay_id.ok());
  SessionConfig replay_sc;
  replay_sc.record = false;
  REQUIRE(replay.start_session(replay_sc).ok());

  ReplaySource src(replay);
  ReplayConfig rcfg;
  rcfg.lscan_dir = dir;
  rcfg.target_device = replay_id.value();
  rcfg.speed = 0.0;  // unpaced: this is a correctness proof, not a timing test
  REQUIRE(src.run(rcfg).ok());
  REQUIRE(replay.stop_session().ok());

  CHECK(src.stats().truncated_tail_chunks == 0);
  CHECK(src.stats().crc_mismatch_chunks == 0);
  CHECK(src.stats().bytes_replayed == bytes.size());

  // --- compare: replay == capture (Tech Spec §3 Key Rule 2) ---------------
  const auto lh = live.device_health(live_id.value()).value();
  const auto rh = replay.device_health(replay_id.value()).value();
  CHECK(rh.packets_ok == lh.packets_ok);
  CHECK(rh.packets_ok == 22);  // 2 x (1 start packet + 10 point packets), matches test_engine.cpp
  CHECK(rh.packets_bad == lh.packets_bad);
  CHECK(rh.points_out == lh.points_out);
  CHECK(rh.checksum_pass_rate == doctest::Approx(lh.checksum_pass_rate));
  CHECK(rh.bytes_in == lh.bytes_in);

  REQUIRE(live.points().total_points() == replay.points().total_points());
  const auto lids = live.points().page_ids();
  const auto rids = replay.points().page_ids();
  REQUIRE(lids.size() == 1);
  REQUIRE(rids.size() == 1);

  const PageView lpv = live.points().page_view(lids[0]);
  const PageView rpv = replay.points().page_view(rids[0]);
  REQUIRE(lpv.count == rpv.count);
  // The "checksum" the task description asks for: every decoded point,
  // bit-identical, not just matching counts.
  std::uint64_t identical = 0;
  for (std::uint32_t i = 0; i < lpv.count; ++i) {
    if (lpv.data[i].x == rpv.data[i].x && lpv.data[i].y == rpv.data[i].y &&
        lpv.data[i].z == rpv.data[i].z && lpv.data[i].r == rpv.data[i].r &&
        lpv.data[i].g == rpv.data[i].g && lpv.data[i].b == rpv.data[i].b &&
        lpv.data[i].a == rpv.data[i].a) {
      ++identical;
    }
  }
  CHECK(identical == lpv.count);
}

TEST_CASE("record_io/replay_speed_multiplier_paces_wall_clock") {
  const std::string dir = make_temp_dir("replay_speed");
  TempDirGuard guard(dir);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::uint8_t payload[4] = {1, 2, 3, 4};
  // Five chunks, 20ms apart -> 80ms of recorded span.
  for (int i = 0; i < 5; ++i) {
    REQUIRE(w.write_chunk(ChunkType::kD6Raw, i * 20'000'000LL, ByteSpan(payload, 4)).ok());
  }
  REQUIRE(w.close().ok());

  auto engine = Engine::create(small_engine_config());
  REQUIRE(engine.ok());
  Engine& e = *engine.value();
  auto id = e.add_device(d6_config());
  REQUIRE(id.ok());
  SessionConfig sc;
  sc.record = false;
  REQUIRE(e.start_session(sc).ok());

  ReplaySource src(e);
  ReplayConfig cfg;
  cfg.lscan_dir = dir;
  cfg.target_device = id.value();
  cfg.speed = 1.0;  // real time

  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(src.run(cfg).ok());
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  CHECK(src.stats().chunks_replayed == 5);
  // Real-time pacing should take at least most of the recorded 80ms span;
  // generous lower bound to avoid CI flakiness while still proving the
  // sleeps actually happened (an unpaced run would finish in <1ms).
  CHECK(elapsed >= std::chrono::milliseconds(40));
}

// --- zip export / import --------------------------------------------------------

TEST_CASE("record_io/zip_export_import_round_trips_a_capture") {
  const std::string dir = make_temp_dir("zip_src");
  TempDirGuard src_guard(dir);
  const std::string zip_path = make_temp_dir("zip_file") + ".zip";
  TempDirGuard zip_guard(zip_path);
  const std::string dest = make_temp_dir("zip_dest");
  TempDirGuard dest_guard(dest);

  FileRecordWriter w;
  REQUIRE(w.open(dir).ok());
  const std::uint8_t p1[5] = {1, 2, 3, 4, 5};
  const std::uint8_t p2[3] = {9, 8, 7};
  REQUIRE(w.write_chunk(ChunkType::kD6Raw, 1, ByteSpan(p1, 5)).ok());
  REQUIRE(w.write_chunk(ChunkType::kGnssNmea, 2, ByteSpan(p2, 3)).ok());
  REQUIRE(w.close().ok());

  REQUIRE(zip_export(dir, zip_path).ok());
  CHECK(fs::is_regular_file(zip_path));
  REQUIRE(zip_import(zip_path, dest).ok());

  CHECK(fs::is_regular_file(dest + "/manifest.json"));
  CHECK(fs::is_directory(dest + "/streams/frames"));
  CHECK(fs::is_directory(dest + "/processed"));
  CHECK(fs::is_directory(dest + "/merged"));
  CHECK(fs::is_directory(dest + "/exports"));

  FileRecordReader r;
  REQUIRE(r.open(dest).ok());
  CHECK(r.manifest_ok());
  int count = 0;
  for (;;) {
    ChunkHeader h;
    std::vector<std::uint8_t> payload;
    const Status s = r.next_chunk(&h, &payload);
    if (s.error() == ScanError::kAgain) break;
    REQUIRE(s.ok());
    ++count;
  }
  CHECK(count == 2);
  CHECK(r.warnings().total_skipped() == 0);
  CHECK(r.close().ok());
}

TEST_CASE("record_io/zip_import_rejects_path_traversal_and_unsupported_compression") {
  // Hand-crafted minimal single-entry archives -- not produced by
  // zip_export(), which never emits either of these.
  auto put16 = [](std::string* s, std::uint16_t v) {
    s->push_back(static_cast<char>(v & 0xFF));
    s->push_back(static_cast<char>((v >> 8) & 0xFF));
  };
  auto put32 = [](std::string* s, std::uint32_t v) {
    s->push_back(static_cast<char>(v & 0xFF));
    s->push_back(static_cast<char>((v >> 8) & 0xFF));
    s->push_back(static_cast<char>((v >> 16) & 0xFF));
    s->push_back(static_cast<char>((v >> 24) & 0xFF));
  };
  auto build_single_entry_zip = [&](const std::string& name, std::uint16_t method) {
    std::string local;
    put32(&local, 0x04034b50u);
    put16(&local, 20);
    put16(&local, 0);
    put16(&local, method);
    put16(&local, 0);
    put16(&local, 0x21);
    put32(&local, 0);  // crc (unused for these rejection paths)
    put32(&local, 0);  // compressed size
    put32(&local, 0);  // uncompressed size
    put16(&local, static_cast<std::uint16_t>(name.size()));
    put16(&local, 0);
    local += name;
    // no file data (0 bytes)

    const std::uint32_t central_start = static_cast<std::uint32_t>(local.size());
    std::string central;
    put32(&central, 0x02014b50u);
    put16(&central, 20);
    put16(&central, 20);
    put16(&central, 0);
    put16(&central, method);
    put16(&central, 0);
    put16(&central, 0x21);
    put32(&central, 0);
    put32(&central, 0);
    put32(&central, 0);
    put16(&central, static_cast<std::uint16_t>(name.size()));
    put16(&central, 0);
    put16(&central, 0);
    put16(&central, 0);
    put16(&central, 0);
    put32(&central, 0);
    put32(&central, 0);  // local header offset
    central += name;

    std::string eocd;
    put32(&eocd, 0x06054b50u);
    put16(&eocd, 0);
    put16(&eocd, 0);
    put16(&eocd, 1);
    put16(&eocd, 1);
    put32(&eocd, static_cast<std::uint32_t>(central.size()));
    put32(&eocd, central_start);
    put16(&eocd, 0);

    return local + central + eocd;
  };

  const std::string dest = make_temp_dir("zip_reject_dest");
  TempDirGuard dest_guard(dest);

  SUBCASE("path traversal") {
    const std::string zip_path = make_temp_dir("zip_traversal") + ".zip";
    TempDirGuard zip_guard(zip_path);
    const std::string blob = build_single_entry_zip("../evil.txt", /*method=*/0);
    overwrite(zip_path, std::vector<std::uint8_t>(blob.begin(), blob.end()));

    const Status s = zip_import(zip_path, dest);
    CHECK_FALSE(s.ok());
    CHECK(s.error() == ScanError::kInvalidArgument);
  }

  SUBCASE("unsupported compression method") {
    const std::string zip_path = make_temp_dir("zip_deflate") + ".zip";
    TempDirGuard zip_guard(zip_path);
    const std::string blob = build_single_entry_zip("streams/lidar.bin", /*method=*/8);
    overwrite(zip_path, std::vector<std::uint8_t>(blob.begin(), blob.end()));

    const Status s = zip_import(zip_path, dest);
    CHECK_FALSE(s.ok());
    CHECK(s.error() == ScanError::kNotSupported);
  }
}
