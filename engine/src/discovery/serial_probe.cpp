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
