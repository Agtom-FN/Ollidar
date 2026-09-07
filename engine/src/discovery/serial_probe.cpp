// serial_probe.cpp — identify a serial device by what it SAYS, not by what
// it is called.
//
// This is the production sibling of tools/fieldtest-kit's probe logic, and it
// exists because of one line in the field session: "/dev/cu.usbmodem2111101 =
// unrelated ESP32 (agri-IoT water-flow logger) on same Mac". Four candidate
// ports, three of them wrong, names that differ by one digit. Any heuristic
// built on the path string picks the wrong one eventually; a wire signature
// cannot.
//
// The two signatures, both closed on real hardware on 2026-08-17:
//   D6     230400 8N1, AA 55 framing, VENDOR checksum variant (2430/2430 —
//          the spec-literal reading scored 143 and is wrong)
//   UM982  NMEA 0183 at 230400, NOT the documented 115200 default; 7 sentence
//          types at 1 Hz including GPTHS, so dual-antenna heading is
//          detectable passively
//
// Owner: A16.
#include "scanengine/discovery/discovery.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "scanengine/core/log.h"
#include "scanengine/drivers/d6/commands.h"
#include "scanengine/drivers/d6/d6_parser.h"
#include "scanengine/drivers/stl27l/stl27l_parser.h"
#include "scanengine/gnss/nmea.h"
#include "scanengine/timesync/clock.h"
#include "serial_port.h"

namespace scanengine {
namespace discovery {
namespace {

constexpr const char* kMod = "discovery";
constexpr std::size_t kReadChunk = 4096;

std::int64_t now_ms() { return SteadyClock::now().nanos / 1000000; }

bool is_textish(std::uint8_t c) {
  return (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
}

}  // namespace

// ===========================================================================
// D6
// ===========================================================================

struct D6Sniffer::Impl {
  d6::Parser parser;
  std::uint32_t text_run = 0;
  bool text = false;
  // Cross-chunk tail, so a "$G" split across two reads still latches.
  char tail[4] = {0, 0, 0, 0};

  Impl() {
    d6::Config cfg;
    cfg.checksum = d6::ChecksumVariant::kVendorSdk;  // FIELD-PROVEN, not the spec reading
    cfg.emit_bad_checksum_points = false;
    // A probe wants packet accounting, not points; dropping zero-range points
    // keeps the internal queue from growing during a 1-second dwell.
    cfg.drop_zero_range = true;
    parser.set_config(cfg);
    // Swallow the points. Without a callback the parser queues them, and a
    // 230400-baud second is ~4000 points we would only throw away.
    parser.set_point_callback([](const d6::Point&) {});
  }
};

D6Sniffer::D6Sniffer() : impl_(new Impl) {}
D6Sniffer::~D6Sniffer() = default;

void D6Sniffer::Feed(const std::uint8_t* data, std::size_t n) {
  if (data == nullptr || n == 0) return;

  // --- the text latch ------------------------------------------------------
  //
  // Two independent triggers, because both matter:
  //   * a long printable run (any text protocol, including ones we have never
  //     heard of — a modem's AT banner, a bootloader prompt)
  //   * an explicit NMEA / Unicore start ("$G", "#UNI"), which identifies the
  //     UM982 in the first few bytes and must gate stage 2 immediately
  // Once latched it never clears: the port has proven it is somebody else's.
  for (std::size_t i = 0; i < n && !impl_->text; ++i) {
    impl_->text_run = is_textish(data[i]) ? impl_->text_run + 1 : 0;
    if (impl_->text_run >= 32) impl_->text = true;
    impl_->tail[0] = impl_->tail[1];
    impl_->tail[1] = impl_->tail[2];
    impl_->tail[2] = impl_->tail[3];
    impl_->tail[3] = static_cast<char>(data[i]);
    if (impl_->tail[2] == '$' && (impl_->tail[3] == 'G' || impl_->tail[3] == 'P')) {
      impl_->text = true;
    }
    if (std::memcmp(impl_->tail, "#UNI", 4) == 0) impl_->text = true;
  }

  impl_->parser.feed(data, n);
}

bool D6Sniffer::Identified() const {
  return impl_->parser.stats().packets_ok >= kPacketsToIdentify;
}
std::uint32_t D6Sniffer::packets_ok() const {
  return static_cast<std::uint32_t>(impl_->parser.stats().packets_ok);
}
std::uint32_t D6Sniffer::packets_bad_checksum() const {
  return static_cast<std::uint32_t>(impl_->parser.stats().packets_bad_checksum);
}
bool D6Sniffer::LooksLikeText() const { return impl_->text; }

void D6Sniffer::Reset() {
  impl_->parser.reset();
  impl_->text_run = 0;
  impl_->text = false;
  std::memset(impl_->tail, 0, sizeof(impl_->tail));
}

// ===========================================================================
// STL-27L  (ITEM 119)
// ===========================================================================
//
// PROTOCOL-DERIVED, NOT OBSERVED. No STL-27L hardware exists on this project;
// the bands below come from the public LD-series references and the
// datasheet's rates, not from a capture. They are deliberately GENEROUS —
// a probe that refuses to identify real hardware is a worse failure than one
// that takes an extra packet to be sure — and every one of them is named so
// that a first-contact session can widen exactly the one that was wrong.

namespace {

// Header sanity, on top of the CRC. The CRC alone is eight bits; these turn
// a 1-in-256 coincidence into a 1-in-millions one, and they cost four
// comparisons.
bool stl27l_packet_looks_sane(const stl27l::Packet& p) {
  // Spin rate. The datasheet's nominal is 10 Hz = 3600 deg/s; the PWM input
  // takes it roughly 5-13 Hz. The band is wider than that on both sides.
  if (p.speed_dps < 600 || p.speed_dps > 9000) return false;
  // Angles are 0.01 deg and decode() already divided by 100, so both must be
  // inside one revolution.
  if (!(p.start_angle_deg >= 0.f && p.start_angle_deg < 360.f)) return false;
  if (!(p.end_angle_deg >= 0.f && p.end_angle_deg < 360.f)) return false;
  // Twelve points at the datasheet rate span 360*12/2160 = 2 degrees. Allow
  // anything from a hair above zero to 30 degrees, which covers a unit spun
  // far faster than spec, and reject a span of exactly zero (a stuck encoder
  // or, far more likely, a false header made of repeated bytes).
  float end = p.end_angle_deg;
  if (end < p.start_angle_deg) end += 360.f;
  const float span = end - p.start_angle_deg;
  if (!(span > 0.001f && span <= 30.f)) return false;
  return true;
}

}  // namespace

struct Stl27lSniffer::Impl {
  stl27l::Parser parser;
  // The COIN-D6 cross-check. Its own parser, fed the same bytes, so "this is
  // the other lidar" is a decode and not a heuristic.
  d6::Parser d6_parser;
  std::uint32_t sane_packets = 0;
  std::uint16_t speed_dps = 0;
  std::uint32_t text_run = 0;
  bool text = false;
  char tail[4] = {0, 0, 0, 0};

  Impl() {
    stl27l::Config cfg;
    cfg.drop_zero_range = true;
    cfg.emit_bad_crc_points = false;
    // A probe reads packet accounting, never points; a false header must cost
    // one byte, not 47 (Config::consume_packet_on_bad_crc).
    cfg.consume_packet_on_bad_crc = false;
    // The probe has no use for timestamps and no wire model it can trust
    // (a discovery read is bursty by nature), so leave that machinery off.
    cfg.per_sample_timestamps = false;
    parser.set_config(cfg);
    parser.set_point_callback([](const stl27l::Point&) {});
    parser.set_packet_callback([this](const stl27l::Packet& p) {
      if (stl27l_packet_looks_sane(p)) {
        ++sane_packets;
        speed_dps = p.speed_dps;
      }
    });

    d6::Config dcfg;
    dcfg.checksum = d6::ChecksumVariant::kVendorSdk;  // field-proven, see above
    dcfg.drop_zero_range = true;
    d6_parser.set_config(dcfg);
    d6_parser.set_point_callback([](const d6::Point&) {});
  }
};

Stl27lSniffer::Stl27lSniffer() : impl_(new Impl) {}
Stl27lSniffer::~Stl27lSniffer() = default;

void Stl27lSniffer::Feed(const std::uint8_t* data, std::size_t n) {
  if (data == nullptr || n == 0) return;

  // The text latch, identical in shape to D6Sniffer's and there for the same
  // reason: a UM982 or a console banner must never read as a lidar.
  for (std::size_t i = 0; i < n && !impl_->text; ++i) {
    impl_->text_run = is_textish(data[i]) ? impl_->text_run + 1 : 0;
    if (impl_->text_run >= 32) impl_->text = true;
    impl_->tail[0] = impl_->tail[1];
    impl_->tail[1] = impl_->tail[2];
    impl_->tail[2] = impl_->tail[3];
    impl_->tail[3] = static_cast<char>(data[i]);
    if (impl_->tail[2] == '$' && (impl_->tail[3] == 'G' || impl_->tail[3] == 'P')) {
      impl_->text = true;
    }
    if (std::memcmp(impl_->tail, "#UNI", 4) == 0) impl_->text = true;
  }

  impl_->parser.feed(data, n);
  impl_->d6_parser.feed(data, n);
}

bool Stl27lSniffer::Identified() const {
  if (impl_->text) return false;
  if (LooksLikeD6()) return false;  // the other lidar owns this port
  return impl_->sane_packets >= kPacketsToIdentify;
}
std::uint32_t Stl27lSniffer::packets_ok() const {
  return static_cast<std::uint32_t>(impl_->parser.stats().packets_ok);
}
std::uint32_t Stl27lSniffer::packets_bad_crc() const {
  return static_cast<std::uint32_t>(impl_->parser.stats().packets_bad_crc);
}
std::uint16_t Stl27lSniffer::speed_dps() const { return impl_->speed_dps; }
bool Stl27lSniffer::LooksLikeText() const { return impl_->text; }
bool Stl27lSniffer::LooksLikeD6() const {
  return impl_->d6_parser.stats().packets_ok >= D6Sniffer::kPacketsToIdentify;
}

void Stl27lSniffer::Reset() {
  impl_->parser.reset();
  impl_->d6_parser.reset();
  impl_->sane_packets = 0;
  impl_->speed_dps = 0;
  impl_->text_run = 0;
  impl_->text = false;
  std::memset(impl_->tail, 0, sizeof(impl_->tail));
}

// ===========================================================================
// JuxiTech IMU module  (A18)
// ===========================================================================
//
// PROTOCOL FROM THE VENDOR'S OWN DRIVER, not from a capture: the reference is
// imu_uart_driver.cpp / .hpp shipped with the module (IMU_UART_Process()'s
// five-state machine and IMU_UART_SendCommand()'s checksum). No JuxiTech
// hardware has been on this Mac yet, so the state machine below is a port of
// that logic and the numbers it accepts are the vendor's, not observations.
// A REAL capture — tests/integration/data/juxi_imu_60s.bin, the plan's Phase
// 0 fixture — is still owed and will be the thing that closes this.
//
// THE FRAME
//   7E 23 <len> <func> <payload …> <sum8>
//   len   the WHOLE frame length, head bytes and checksum included. So the
//         payload is len-5 bytes, and the vendor's "data_length = len-4"
//         counts the checksum as part of the data section.
//   sum8  the low byte of the arithmetic sum of every preceding byte,
//         0x7E + 0x23 + len + func + payload.
//   func  0x04 raw IMU (18-byte payload: accel/gyro/mag, 9 × int16 LE)
//         0x16 quaternion (16), 0x26 Euler (12), 0x32 barometer (16)
//         0x01 version (3: high, mid, low) — only ever sent on request, and
//              this probe never requests anything. Parsed if it appears.
//   The module emits the first four UNPROMPTED at 25 Hz by default (up to
//   100 Hz once configured), which is what makes a passive probe possible.

namespace {

constexpr std::uint8_t kJuxiHead1 = 0x7E;
constexpr std::uint8_t kJuxiHead2 = 0x23;

constexpr std::uint8_t kJuxiFuncVersion = 0x01;
constexpr std::uint8_t kJuxiFuncRaw = 0x04;
constexpr std::uint8_t kJuxiFuncQuat = 0x16;
constexpr std::uint8_t kJuxiFuncEuler = 0x26;
constexpr std::uint8_t kJuxiFuncBaro = 0x32;

// The vendor's own frame_buffer is 64 bytes; the largest frame it can
// describe is len = 68. Anything claiming more is noise that happened to
// land on a 7E 23 pair.
constexpr std::uint8_t kJuxiMinFrameLen = 5;   // head1+head2+len+func+sum8
constexpr std::uint8_t kJuxiMaxFrameLen = 68;

}  // namespace

struct JuxiImuSniffer::Impl {
  enum class State { kHead1, kHead2, kLength, kFunc, kData };

  State state = State::kHead1;
  std::uint8_t frame_len = 0;
  std::uint8_t func = 0;
  std::uint8_t data[kJuxiMaxFrameLen] = {0};
  std::uint16_t index = 0;

  std::uint32_t ok = 0;
  std::uint32_t bad = 0;
  std::uint32_t raw = 0;
  std::uint32_t quat = 0;
  std::uint32_t euler = 0;
  std::uint32_t baro = 0;

  std::uint8_t version[3] = {0, 0, 0};
  bool have_version = false;

  std::uint32_t text_run = 0;
  bool text = false;
  char tail[4] = {0, 0, 0, 0};

  void accept(std::uint8_t f, const std::uint8_t* payload, std::size_t n) {
    ++ok;
    switch (f) {
      case kJuxiFuncRaw:
        // The payload length is checked because a checksum-valid frame with
        // the right func and the WRONG length is not this device — and
        // raw_frames is what the caller uses to decide the module is
        // actually streaming IMU rather than only barometer.
        if (n == 18) ++raw;
        break;
      case kJuxiFuncQuat: if (n == 16) ++quat; break;
      case kJuxiFuncEuler: if (n == 12) ++euler; break;
      case kJuxiFuncBaro: if (n == 16) ++baro; break;
      case kJuxiFuncVersion:
        if (n >= 3) {
          version[0] = payload[0];
          version[1] = payload[1];
          version[2] = payload[2];
          have_version = true;
        }
        break;
      default:
        break;  // a func we do not model still counts as a valid frame
    }
  }
};

JuxiImuSniffer::JuxiImuSniffer() : impl_(new Impl) {}
JuxiImuSniffer::~JuxiImuSniffer() = default;

void JuxiImuSniffer::Feed(const std::uint8_t* data, std::size_t n) {
  if (data == nullptr || n == 0) return;

  // The text latch, identical in shape to D6Sniffer's. Here it carries more
  // weight than in either lidar probe: this probe runs BEFORE the UM982's,
  // at 115200, which is the UM982's DOCUMENTED default rate. Without this,
  // a receiver that happens to be at 115200 would be read as noise for a
  // full dwell and — far worse — a future relaxation of the frame bar could
  // let it be claimed outright.
  for (std::size_t i = 0; i < n && !impl_->text; ++i) {
    impl_->text_run = is_textish(data[i]) ? impl_->text_run + 1 : 0;
    if (impl_->text_run >= 32) impl_->text = true;
    impl_->tail[0] = impl_->tail[1];
    impl_->tail[1] = impl_->tail[2];
    impl_->tail[2] = impl_->tail[3];
    impl_->tail[3] = static_cast<char>(data[i]);
    if (impl_->tail[2] == '$' && (impl_->tail[3] == 'G' || impl_->tail[3] == 'P')) {
      impl_->text = true;
    }
    if (std::memcmp(impl_->tail, "#UNI", 4) == 0) impl_->text = true;
  }

  // The vendor's five-state machine, byte at a time, so a frame split across
  // any number of read() boundaries reassembles — which is the whole reason
  // this is a state machine and not a memchr over the buffer.
  using State = Impl::State;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t c = data[i];
    switch (impl_->state) {
      case State::kHead1:
        if (c == kJuxiHead1) impl_->state = State::kHead2;
        break;

      case State::kHead2:
        // Not head2: resync. A 7E 7E pair must re-enter kHead2 rather than
        // drop both bytes — the vendor's own loop returns to kHead1 here and
        // would lose the second 7E's frame. One byte of resync accuracy for
        // free.
        if (c == kJuxiHead2) impl_->state = State::kLength;
        else if (c == kJuxiHead1) impl_->state = State::kHead2;
        else impl_->state = State::kHead1;
        break;

      case State::kLength:
        if (c < kJuxiMinFrameLen || c > kJuxiMaxFrameLen) {
          impl_->state = State::kHead1;  // not a frame length; resync
          break;
        }
        impl_->frame_len = c;
        impl_->state = State::kFunc;
        break;

      case State::kFunc:
        impl_->func = c;
        impl_->index = 0;
        impl_->state = State::kData;
        break;

      case State::kData: {
        // data_length counts the checksum, exactly as the vendor's driver
        // does. kLength already guaranteed frame_len >= 5, so this is >= 1.
        const std::uint16_t data_length = static_cast<std::uint16_t>(impl_->frame_len - 4);
        impl_->data[impl_->index++] = c;
        if (impl_->index < data_length) break;

        std::uint32_t sum = static_cast<std::uint32_t>(kJuxiHead1) + kJuxiHead2 +
                            impl_->frame_len + impl_->func;
        for (std::uint16_t k = 0; k + 1 < data_length; ++k) sum += impl_->data[k];
        const std::uint8_t want = impl_->data[data_length - 1];
        if (static_cast<std::uint8_t>(sum & 0xFFu) == want) {
          impl_->accept(impl_->func, impl_->data,
                        static_cast<std::size_t>(data_length - 1));
        } else {
          ++impl_->bad;
        }
        impl_->state = State::kHead1;
        break;
      }
    }
  }
}

bool JuxiImuSniffer::Identified() const {
  if (impl_->text) return false;  // somebody else's port
  return impl_->ok >= kFramesToIdentify;
}
std::uint32_t JuxiImuSniffer::frames_ok() const { return impl_->ok; }
std::uint32_t JuxiImuSniffer::frames_bad_checksum() const { return impl_->bad; }
std::uint32_t JuxiImuSniffer::raw_frames() const { return impl_->raw; }
std::uint32_t JuxiImuSniffer::quat_frames() const { return impl_->quat; }
std::uint32_t JuxiImuSniffer::euler_frames() const { return impl_->euler; }
std::uint32_t JuxiImuSniffer::baro_frames() const { return impl_->baro; }
bool JuxiImuSniffer::version_known() const { return impl_->have_version; }
const std::uint8_t* JuxiImuSniffer::version() const { return impl_->version; }
bool JuxiImuSniffer::LooksLikeText() const { return impl_->text; }

void JuxiImuSniffer::Reset() {
  impl_->state = Impl::State::kHead1;
  impl_->frame_len = 0;
  impl_->func = 0;
  impl_->index = 0;
  impl_->ok = 0;
  impl_->bad = 0;
  impl_->raw = 0;
  impl_->quat = 0;
  impl_->euler = 0;
  impl_->baro = 0;
  impl_->version[0] = impl_->version[1] = impl_->version[2] = 0;
  impl_->have_version = false;
  impl_->text_run = 0;
  impl_->text = false;
  std::memset(impl_->tail, 0, sizeof(impl_->tail));
}

// ===========================================================================
// UM982
// ===========================================================================

struct Um982Sniffer::Impl {
  std::string line;
  std::uint32_t ok = 0;
  std::uint32_t bad = 0;
  bool heading = false;

  void finish_line() {
    if (line.empty()) {
      return;
    }
    const std::string l = line;
    line.clear();
    if (l.size() > 512) return;

    if (l[0] == '$') {
      nmea::Sentence s;
      nmea::ParseOptions opt;
      opt.allow_missing_checksum = false;  // a probe insists on the checksum
      opt.max_bytes = 256;
      const nmea::NmeaError e = nmea::parse_sentence(l, &s, opt);
      if (e == nmea::NmeaError::kOk) {
        ++ok;
        // Dual-antenna heading. THS is what the real unit emitted (7 types
        // @ 1 Hz including GPTHS); HDT/ROT are what other firmware builds
        // use for the same thing.
        if (s.type == "THS" || s.type == "HDT" || s.type == "ROT") heading = true;
      } else if (e != nmea::NmeaError::kNoStart && e != nmea::NmeaError::kEmpty) {
        ++bad;
      }
      return;
    }

    // Unicore's own ASCII logs: "#UNIHEADINGA,...;...*a1b2c3d4". Not NMEA —
    // '#' start, 8-hex CRC32 — so the NMEA parser cannot see them, and they
    // are the strongest possible evidence the device is a Unicore board.
    if (l[0] == '#' && l.size() > 12) {
      const std::size_t star = l.rfind('*');
      if (star != std::string::npos && l.size() - star == 9) {
        bool hex = true;
        for (std::size_t i = star + 1; i < l.size(); ++i) {
          const char c = l[i];
          const bool h = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
          if (!h) hex = false;
        }
        if (hex) {
          ++ok;
          if (l.find("HEADING") != std::string::npos) heading = true;
        }
      }
    }
  }
};

Um982Sniffer::Um982Sniffer() : impl_(new Impl) {}
Um982Sniffer::~Um982Sniffer() = default;

void Um982Sniffer::Feed(const std::uint8_t* data, std::size_t n) {
  if (data == nullptr) return;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\r' || c == '\n') {
      impl_->finish_line();
      continue;
    }
    // At the WRONG baud rate the line never terminates and just grows; cap it
    // so a 460800-vs-9600 mismatch costs bounded memory and resyncs.
    if (impl_->line.size() > 600) impl_->line.clear();
    impl_->line += c;
  }
}

bool Um982Sniffer::Identified() const { return impl_->ok >= kSentencesToIdentify; }
bool Um982Sniffer::has_heading() const { return impl_->heading; }
std::uint32_t Um982Sniffer::sentences_ok() const { return impl_->ok; }
std::uint32_t Um982Sniffer::sentences_bad() const { return impl_->bad; }

void Um982Sniffer::Reset() {
  impl_->line.clear();
  impl_->ok = 0;
  impl_->bad = 0;
  impl_->heading = false;
}

// ===========================================================================
// The port-walking probes
// ===========================================================================

std::optional<D6Probe> ProbeSerialD6(const std::vector<std::string>& port_paths,
                                     int per_port_ms) {
  const int budget = per_port_ms > 0 ? per_port_ms : 1000;
  // Two thirds passive, one third for the start-command stage. A D6 that is
  // already running is found in the first few hundred milliseconds; one that
  // is idle needs the command and then a revolution to answer.
  const int stage1_ms = std::max(120, (budget * 2) / 3);
  const int stage2_ms = std::max(120, budget - stage1_ms);

  for (const std::string& path : port_paths) {
    discovery_serial::SerialPort port;
    const discovery_serial::OpenResult r = port.Open(path, 230400);
    if (r != discovery_serial::OpenResult::kOk) {
      // A busy port is SKIPPED SILENTLY: on macOS the app's own capture
      // session, or a leftover process, holds it, and shouting about it in a
      // discovery scan is noise. Everything else is worth a debug line.
      if (r == discovery_serial::OpenResult::kBusy) {
        SCAN_LOG_DEBUG(kMod, "d6 probe: %s is busy — skipping", path.c_str());
      } else {
        SCAN_LOG_DEBUG(kMod, "d6 probe: %s not usable (%s)", path.c_str(),
                       discovery_serial::to_string(r));
      }
      continue;
    }

    D6Sniffer sniffer;
    std::vector<std::uint8_t> buf(kReadChunk);

    // --- stage 1: listen -----------------------------------------------
    const std::int64_t t1_end = now_ms() + stage1_ms;
    while (now_ms() < t1_end && !sniffer.Identified()) {
      const int n = port.Read(buf.data(), buf.size(), 50);
      if (n < 0) break;
      if (n > 0) sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
    }
    if (sniffer.Identified()) {
      D6Probe p;
      p.port = path;
      p.baud = 230400;
      p.packets_ok = sniffer.packets_ok();
      p.packets_bad_checksum = sniffer.packets_bad_checksum();
      p.used_start_command = false;
      SCAN_LOG_INFO(kMod, "d6 found on %s (%u packets, passive)", path.c_str(), p.packets_ok);
      return p;
    }

    // --- stage 2: ask, once, and only when stage 1 was inconclusive -----
    //
    // The single write this module is allowed to make. Gated on "no text seen"
    // so a GNSS receiver, a modem or a console never receives AA 55 F0 0F.
    if (sniffer.LooksLikeText()) {
      SCAN_LOG_DEBUG(kMod, "d6 probe: %s is a text protocol — not probing further",
                     path.c_str());
      continue;
    }
    if (!port.Write(d6::kCmdStart, sizeof(d6::kCmdStart))) continue;

    const std::int64_t t2_end = now_ms() + stage2_ms;
    while (now_ms() < t2_end && !sniffer.Identified()) {
      const int n = port.Read(buf.data(), buf.size(), 50);
      if (n < 0) break;
      if (n > 0) sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
    }
    // Leave the device as we found it, win or lose. The field session saw no
    // stop-ACK from this unit; we do not wait for one.
    (void)port.Write(d6::kCmdStop, sizeof(d6::kCmdStop));

    if (sniffer.Identified()) {
      D6Probe p;
      p.port = path;
      p.baud = 230400;
      p.packets_ok = sniffer.packets_ok();
      p.packets_bad_checksum = sniffer.packets_bad_checksum();
      p.used_start_command = true;
      SCAN_LOG_INFO(kMod, "d6 found on %s (%u packets, after start command)", path.c_str(),
                    p.packets_ok);
      return p;
    }
  }
  return std::nullopt;
}

std::optional<Stl27lProbe> ProbeSerialStl27l(const std::vector<std::string>& port_paths,
                                             int per_port_ms) {
  // One stage, all of it passive. A powered STL-27L emits ~1800 packets per
  // second, so four of them arrive inside the first few milliseconds — the
  // budget here is dominated by open() latency, not by the dwell.
  const int budget = per_port_ms > 0 ? per_port_ms : 1000;

  for (const std::string& path : port_paths) {
    discovery_serial::SerialPort port;
    const discovery_serial::OpenResult r =
        port.Open(path, static_cast<int>(stl27l::kDefaultBaud));
    if (r != discovery_serial::OpenResult::kOk) {
      if (r == discovery_serial::OpenResult::kBusy) {
        SCAN_LOG_DEBUG(kMod, "stl27l probe: %s is busy — skipping", path.c_str());
      } else {
        SCAN_LOG_DEBUG(kMod, "stl27l probe: %s not usable (%s)", path.c_str(),
                       discovery_serial::to_string(r));
      }
      continue;
    }

    Stl27lSniffer sniffer;
    std::vector<std::uint8_t> buf(kReadChunk);
    const std::int64_t end = now_ms() + budget;
    while (now_ms() < end && !sniffer.Identified()) {
      if (sniffer.LooksLikeText() || sniffer.LooksLikeD6()) break;  // somebody else's port
      const int n = port.Read(buf.data(), buf.size(), 50);
      if (n < 0) break;
      if (n > 0) sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
    }

    if (sniffer.Identified()) {
      Stl27lProbe p;
      p.port = path;
      p.baud = stl27l::kDefaultBaud;
      p.packets_ok = sniffer.packets_ok();
      p.packets_bad_crc = sniffer.packets_bad_crc();
      p.speed_dps = sniffer.speed_dps();
      SCAN_LOG_INFO(kMod, "stl-27l found on %s (%u packets, %u deg/s, passive)", path.c_str(),
                    p.packets_ok, static_cast<unsigned>(p.speed_dps));
      return p;
    }
    if (sniffer.LooksLikeD6()) {
      SCAN_LOG_DEBUG(kMod, "stl27l probe: %s is speaking COIN-D6 — not ours", path.c_str());
    }
  }
  return std::nullopt;
}

std::optional<JuxiImuProbe> ProbeSerialJuxiImu(const std::vector<std::string>& port_paths,
                                               int per_port_ms) {
  // One stage, all of it passive, at ONE rate — the module has no other.
  //
  // The dwell has a FLOOR for the same reason the UM982's does, though a
  // gentler one. The module free-runs at 25 Hz in its default mode, so three
  // frames of any type arrive within ~40 ms and identification is quick; what
  // needs the floor is frame_rate_hz, which is meaningless measured over two
  // frame intervals. 400 ms buys ~10 raw frames at 25 Hz and ~40 at 100 Hz —
  // enough to tell those two modes apart, which is the whole point of
  // reporting the rate at all.
  const int budget = per_port_ms > 0 ? per_port_ms : 1000;
  const int dwell_ms = std::max(400, budget);

  for (const std::string& path : port_paths) {
    discovery_serial::SerialPort port;
    const discovery_serial::OpenResult r = port.Open(path, 115200);
    if (r != discovery_serial::OpenResult::kOk) {
      if (r == discovery_serial::OpenResult::kBusy) {
        SCAN_LOG_DEBUG(kMod, "juxi-imu probe: %s is busy — skipping", path.c_str());
      } else {
        SCAN_LOG_DEBUG(kMod, "juxi-imu probe: %s not usable (%s)", path.c_str(),
                       discovery_serial::to_string(r));
      }
      continue;
    }
    // Measure the LIVE stream, not the driver's backlog: a stale buffer
    // would inflate frame_rate_hz by however long the port sat open.
    port.FlushInput();

    JuxiImuSniffer sniffer;
    std::vector<std::uint8_t> buf(kReadChunk);
    // The rate window opens at the FIRST byte, not at open(): everything
    // before that is open() latency and the module's own silence, and
    // counting it would report a rate that is too low by whatever the OS
    // took to hand us the port.
    std::int64_t t_first = 0;
    std::int64_t t_last = 0;
    const std::int64_t end = now_ms() + dwell_ms;
    while (now_ms() < end) {
      if (sniffer.LooksLikeText()) break;  // an NMEA talker — leave it for the UM982 probe
      const int n = port.Read(buf.data(), buf.size(), 50);
      if (n < 0) break;
      if (n > 0) {
        const std::int64_t t = now_ms();
        if (t_first == 0) t_first = t;
        t_last = t;
        sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
      }
    }

    if (sniffer.LooksLikeText()) {
      SCAN_LOG_DEBUG(kMod, "juxi-imu probe: %s is a text protocol — not ours", path.c_str());
      continue;
    }
    if (!sniffer.Identified()) continue;

    JuxiImuProbe p;
    p.port_path = path;
    p.baud = 115200;
    p.frames_seen = sniffer.frames_ok();
    p.raw_frames = sniffer.raw_frames();
    // Guard the divide: a stream that delivered everything in one read has
    // t_last == t_first, and "infinity Hz" is a worse answer than "unknown".
    const std::int64_t span_ms = t_last - t_first;
    if (span_ms >= 100 && p.raw_frames > 0) {
      p.frame_rate_hz = static_cast<double>(p.raw_frames) * 1000.0 /
                        static_cast<double>(span_ms);
    }
    if (sniffer.version_known()) {
      p.version[0] = sniffer.version()[0];
      p.version[1] = sniffer.version()[1];
      p.version[2] = sniffer.version()[2];
      p.version_known = true;
    }
    SCAN_LOG_INFO(kMod,
                  "juxitech imu found on %s @ 115200 (%u frames, %u raw, %.1f Hz, passive)",
                  path.c_str(), p.frames_seen, p.raw_frames, p.frame_rate_hz);
    return p;
  }
  return std::nullopt;
}

std::optional<Um982Probe> ProbeSerialUm982(const std::vector<std::string>& port_paths,
                                           int per_port_ms) {
  const int budget = per_port_ms > 0 ? per_port_ms : 1000;
  // A 1 Hz receiver emits its whole sentence burst in a few tens of ms and
  // then goes SILENT for the rest of the second — so any dwell shorter than
  // one full period mostly samples the silence and misses the device (field
  // failure 2026-08-17: real UM982 @ 1 Hz missed by a 150 ms dwell). The
  // dwell must cover one period plus margin: 1100 ms, regardless of how
  // small the caller's budget is. Cost containment comes from the silent-
  // port fast-path below, not from shrinking the window.
  const int dwell_ms =
      std::max(1100, budget / static_cast<int>(kUm982BaudSweepCount));

  for (const std::string& path : port_paths) {
    bool saw_any_bytes = false;
    for (std::size_t bi = 0; bi < kUm982BaudSweepCount; ++bi) {
      const std::uint32_t baud = kUm982BaudSweep[bi];
      discovery_serial::SerialPort port;
      const discovery_serial::OpenResult r = port.Open(path, baud);
      if (r != discovery_serial::OpenResult::kOk) {
        if (r == discovery_serial::OpenResult::kBusy) {
          SCAN_LOG_DEBUG(kMod, "um982 probe: %s is busy — skipping", path.c_str());
          break;  // busy at one rate is busy at all of them
        }
        SCAN_LOG_DEBUG(kMod, "um982 probe: %s @ %u not usable (%s)", path.c_str(), baud,
                       discovery_serial::to_string(r));
        continue;
      }

      Um982Sniffer sniffer;
      std::vector<std::uint8_t> buf(kReadChunk);
      const std::int64_t end = now_ms() + dwell_ms;
      while (now_ms() < end && !sniffer.Identified()) {
        const int n = port.Read(buf.data(), buf.size(), 50);
        if (n < 0) break;
        if (n > 0) {
          saw_any_bytes = true;
          sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
        }
      }
      if (!sniffer.Identified()) {
        // A UM982 transmits continuously at SOME rate — wrong-baud garbage
        // still arrives as bytes. A port that stayed completely silent for a
        // full period has no free-running transmitter on it: skip the rest of
        // the sweep instead of spending four more dwells on a silent line.
        if (bi == 0 && !saw_any_bytes) break;
        continue;
      }

      // One more dwell at the winning rate, purely to see whether a heading
      // sentence is in the 1 Hz rotation — it is what decides whether the app
      // offers heading-aided georeferencing, and it may not be in the first
      // two sentences.
      const std::int64_t extra_end = now_ms() + dwell_ms;
      while (now_ms() < extra_end && !sniffer.has_heading()) {
        const int n = port.Read(buf.data(), buf.size(), 50);
        if (n < 0) break;
        if (n > 0) sniffer.Feed(buf.data(), static_cast<std::size_t>(n));
      }

      Um982Probe p;
      p.port = path;
      p.baud = baud;
      p.has_heading = sniffer.has_heading();
      p.sentences_ok = sniffer.sentences_ok();
      p.sentences_bad = sniffer.sentences_bad();
      SCAN_LOG_INFO(kMod, "um982 found on %s @ %u (%u sentences, heading %s)", path.c_str(),
                    baud, p.sentences_ok, p.has_heading ? "yes" : "no");
      return p;
    }
  }
  return std::nullopt;
}

}  // namespace discovery
}  // namespace scanengine
