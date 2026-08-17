// probe_fake_selftest.cpp -- standalone verification tool for
// scanengine::discovery::ProbeSerialD6 / ProbeSerialUm982 against the
// pty-based fakes in serial_probe_fakes.py (docs/design/REVIEW_FEEDBACK.md
// 2026-08-17 round 4 item 5's "D6/UM982 probe paths against pty-based
// fakes if feasible").
//
// NOT part of the desktop app build (desktop/CMakeLists.txt is untouched by
// this file) -- a one-off evidence tool, compiled and run by hand:
//
//   python3 desktop/scripts/serial_probe_fakes.py d6 --speed 4 > /tmp/d6.pty &
//   sleep 1
//   c++ -std=c++20 -I engine/include desktop/scripts/probe_fake_selftest.cpp \
//       desktop/build/engine/libscanengine.a -o /tmp/probe_selftest \
//       <whatever extra system libs the link step reports>
//   /tmp/probe_selftest d6 "$(cat /tmp/d6.pty)"
//
// Prints the D6Probe/Um982Probe fields on a hit, or "no identification"
// on a miss, and exits 0/1 accordingly so it is scriptable.
#include <cstdio>
#include <string>
#include <vector>

#include "scanengine/discovery/discovery.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s d6|um982 PTY_SLAVE_PATH\n", argv[0]);
    return 2;
  }
  const std::string kind = argv[1];
  const std::vector<std::string> ports = {argv[2]};

  if (kind == "d6") {
    const auto probe = scanengine::discovery::ProbeSerialD6(ports, 3000);
    if (!probe) {
      std::fprintf(stderr, "ProbeSerialD6(%s): no identification\n", argv[2]);
      return 1;
    }
    std::printf(
        "ProbeSerialD6 HIT: port=%s baud=%u packets_ok=%u packets_bad_checksum=%u "
        "used_start_command=%s\n",
        probe->port.c_str(), probe->baud, probe->packets_ok, probe->packets_bad_checksum,
        probe->used_start_command ? "yes" : "no");
    return 0;
  }
  if (kind == "um982") {
    const auto probe = scanengine::discovery::ProbeSerialUm982(ports, 3000);
    if (!probe) {
      std::fprintf(stderr, "ProbeSerialUm982(%s): no identification\n", argv[2]);
      return 1;
    }
    std::printf(
        "ProbeSerialUm982 HIT: port=%s baud=%u has_heading=%s sentences_ok=%u "
        "sentences_bad=%u\n",
        probe->port.c_str(), probe->baud, probe->has_heading ? "yes" : "no",
        probe->sentences_ok, probe->sentences_bad);
    return 0;
  }
  std::fprintf(stderr, "unknown kind '%s' (want d6|um982)\n", kind.c_str());
  return 2;
}
