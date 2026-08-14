// .lscan container: CRC, chunk/stream framing, the null writer's accounting.
// These are FORMAT tests — A5 must keep them green while implementing the
// real writer, because they are what the Android app, the Qt app and the
// cloud worker all encode against.
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/record/lscan.h"

using namespace scanengine;
using namespace scanengine::lscan;

TEST_CASE("lscan/crc32_matches_the_IEEE_reference_vector") {
  const char* s = "123456789";
  const ByteSpan data(reinterpret_cast<const std::uint8_t*>(s), 9);
  CHECK(crc32(data) == 0xCBF43926u);
  CHECK(crc32(ByteSpan{}) == 0u);
}

TEST_CASE("lscan/chunk_header_round_trips_little_endian") {
  ChunkHeader h;
  h.payload_len = 0x00030201;  // must stay <= kMaxChunkPayload
  h.type = ChunkType::kD6Raw;
  h.flags = kFlagKeyChunk;
  h.t_mono_ns = -1234567890123LL;

  std::uint8_t buf[kChunkHeaderBytes];
  encode_chunk_header(h, buf);
  CHECK(buf[0] == 0x01);  // little-endian length
  CHECK(buf[1] == 0x02);
  CHECK(buf[2] == 0x03);
  CHECK(buf[3] == 0x00);
  CHECK(buf[4] == 1);     // type kD6Raw

  ChunkHeader back{};
  REQUIRE(decode_chunk_header(ByteSpan(buf, sizeof(buf)), &back));
  CHECK(back.payload_len == h.payload_len);
  CHECK(back.type == h.type);
  CHECK(back.flags == h.flags);
  CHECK(back.t_mono_ns == h.t_mono_ns);
}

TEST_CASE("lscan/oversized_chunk_length_is_rejected_on_decode") {
  ChunkHeader h;
  h.payload_len = kMaxChunkPayload + 1;
  std::uint8_t buf[kChunkHeaderBytes];
  encode_chunk_header(h, buf);
  ChunkHeader back{};
  CHECK_FALSE(decode_chunk_header(ByteSpan(buf, sizeof(buf)), &back));
}

TEST_CASE("lscan/stream_header_round_trips_and_checks_the_magic") {
  StreamFileHeader h;
  h.stream = StreamId::kImu;
  h.t_start_mono_ns = 42;
  h.t_start_utc_ns = 1755000000000000000LL;

  std::uint8_t buf[kStreamHeaderBytes];
  encode_stream_header(h, buf);
  CHECK(std::memcmp(buf, kMagic, 4) == 0);

  StreamFileHeader back{};
  REQUIRE(decode_stream_header(ByteSpan(buf, sizeof(buf)), &back));
  CHECK(back.format_version == kFormatVersion);
  CHECK(back.stream == StreamId::kImu);
  CHECK(back.t_start_mono_ns == 42);
  CHECK(back.t_start_utc_ns == h.t_start_utc_ns);

  buf[1] = 'X';
  CHECK_FALSE(decode_stream_header(ByteSpan(buf, sizeof(buf)), &back));
}

TEST_CASE("lscan/chunk_crc_covers_header_and_payload") {
  ChunkHeader h;
  h.payload_len = 4;
  h.type = ChunkType::kGnssNmea;
  h.t_mono_ns = 7;
  const std::uint8_t payload[4] = {1, 2, 3, 4};

  const std::uint32_t c = chunk_crc(h, ByteSpan(payload, 4));

  // Equals the CRC over the concatenation of the encoded header + payload.
  std::vector<std::uint8_t> concat(kChunkHeaderBytes + 4);
  encode_chunk_header(h, concat.data());
  std::memcpy(concat.data() + kChunkHeaderBytes, payload, 4);
  CHECK(c == crc32(ByteSpan(concat.data(), concat.size())));

  // A single flipped payload bit changes it (that is the truncation/corruption
  // guard A5's reader relies on).
  std::uint8_t bad[4] = {1, 2, 3, 5};
  CHECK(chunk_crc(h, ByteSpan(bad, 4)) != c);
}

TEST_CASE("lscan/chunk_types_map_to_stream_files") {
  CHECK(stream_of(ChunkType::kD6Raw) == StreamId::kLidarD6);
  CHECK(stream_of(ChunkType::kMid360Imu) == StreamId::kImu);
  CHECK(stream_of(ChunkType::kGnssRtcm) == StreamId::kGnss);
  CHECK(std::string(stream_file_of(StreamId::kImu)) == "streams/imu.bin");
  CHECK(std::string(stream_file_of(StreamId::kLidarMid360)) == "streams/lidar.bin");
  CHECK(std::string(stream_file_of(StreamId::kCameraFrames)) == "streams/frames/frames.idx");
  // Numeric stability of the format contract.
  CHECK(static_cast<int>(ChunkType::kD6Raw) == 1);
  CHECK(static_cast<int>(ChunkType::kCameraFrameIndex) == 7);
  CHECK(kChunkOverheadBytes == 20);
  CHECK(kFormatVersion == 1);
}

TEST_CASE("lscan/null_writer_validates_and_accounts") {
  NullRecordWriter w;
  const std::uint8_t payload[8] = {0};
  CHECK(w.write_chunk(ChunkType::kD6Raw, 1, ByteSpan(payload, 8)).error() ==
        ScanError::kInvalidState);

  CHECK(w.open("/tmp/does-not-need-to-exist.lscan").ok());
  CHECK(w.is_open());
  CHECK(w.open("/tmp/again.lscan").error() == ScanError::kInvalidState);

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
}
