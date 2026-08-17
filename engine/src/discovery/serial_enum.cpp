// serial_enum.cpp — what serial ports does this machine have?
//
// Enumeration is a HINT, never an identification. The field session had four
// candidate /dev/cu.* on one Mac: the D6, the UM982, an unrelated ESP32 water
// logger, and the built-in Bluetooth port. Only the probe in
// serial_probe.cpp can tell them apart. This file's whole job is to hand that
// probe a short, plausible list instead of making the operator type a path.
//
// Owner: A16.
#include "scanengine/discovery/discovery.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace scanengine {
namespace discovery {
namespace {

bool starts_with(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

#if !defined(_WIN32)
// Ports that exist on every Mac, belong to the OS, and never carry a sensor.
// Opening them is harmless but slow (each costs a probe dwell), and
// cu.Bluetooth-Incoming-Port in particular can block.
bool is_noise_macos(const std::string& name) {
  return name.find("Bluetooth") != std::string::npos ||
         name.find("debug-console") != std::string::npos ||
         name.find("wlan-debug") != std::string::npos;
}
#endif

}  // namespace

std::vector<std::string> EnumerateSerialPorts() {
  std::vector<std::string> out;

#if defined(_WIN32)
  // QueryDosDevice over the whole device namespace: one call, no SetupAPI, no
  // extra import library, and it reports exactly the COM names the CreateFile
  // path accepts. (SetupAPI would additionally give friendly names — which we
  // deliberately do not need, because we identify by protocol.)
  std::vector<char> buf(65536);
  DWORD n = ::QueryDosDeviceA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  if (n == 0 && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
    buf.resize(1 << 20);
    n = ::QueryDosDeviceA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
  }
  if (n != 0) {
    // A NUL-separated, double-NUL-terminated list.
    for (const char* p = buf.data(); *p != '\0'; p += std::strlen(p) + 1) {
      const std::string name(p);
      if (!starts_with(name, "COM")) continue;
      // "COM" followed by digits only — excludes COMx-shaped driver names.
      if (name.size() < 4) continue;
      bool digits = true;
      for (std::size_t i = 3; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') digits = false;
      }
      if (digits) out.push_back(name);
    }
  }
  if (out.empty()) {
    // Fallback for a locked-down session where the bulk query is refused.
    for (int i = 1; i <= 255; ++i) {
      char name[16];
      std::snprintf(name, sizeof(name), "COM%d", i);
      char target[256];
      if (::QueryDosDeviceA(name, target, sizeof(target)) != 0) out.emplace_back(name);
    }
  }
  // COM2 before COM10: numeric order, not lexicographic.
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return std::atoi(a.c_str() + 3) < std::atoi(b.c_str() + 3);
  });

#else
  DIR* dir = ::opendir("/dev");
  if (dir == nullptr) return out;  // unreadable /dev is an empty list, not an error
  while (dirent* e = ::readdir(dir)) {
    const std::string name(e->d_name);
    bool want = false;
#if defined(__APPLE__)
    // cu.* (call-out), never tty.* — opening tty.* on Darwin blocks waiting
    // for DCD, which is exactly the hang a discovery scan must not have.
    want = starts_with(name, "cu.") && !is_noise_macos(name);
#else
    // The three that carry USB serial adapters. /dev/ttyS* is deliberately
    // excluded: on a typical PC it is 32 phantom nodes, each costing a full
    // probe dwell for a port that has never had a device on it.
    want = starts_with(name, "ttyUSB") || starts_with(name, "ttyACM") ||
           starts_with(name, "ttyAMA");
#endif
    if (want) out.push_back("/dev/" + name);
  }
  ::closedir(dir);
  std::sort(out.begin(), out.end());
#endif

  return out;
}

}  // namespace discovery
}  // namespace scanengine
