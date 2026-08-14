// clock.h — the single monotonic engine clock (Tech Spec §3.2).
//
// Migrated verbatim-in-behaviour from the S7 spike
// (spikes/s7-windows-toolchain/engine-stub/include/scanengine/clock.hpp),
// where the four-platform selection was proven to compile and run in CI.
//
// Rule for every module: sample time ONCE, at arrival, with
// SteadyClock::now(), and pass that TimePoint down. Never call now() deeper
// in the stack "to get roughly the same time" — the per-stream offset
// estimators in timesync/offset_estimator.h assume arrival stamps.
//
// Owner: A1 (facade) / A4 (offset estimation, IMU ingestion).
#ifndef SCANENGINE_TIMESYNC_CLOCK_H
#define SCANENGINE_TIMESYNC_CLOCK_H

#include <chrono>
#include <cstdint>

#if defined(_WIN32)
#define SCANENGINE_OS_WINDOWS 1
#elif defined(__ANDROID__)
#define SCANENGINE_OS_ANDROID 1
#elif defined(__APPLE__)
#define SCANENGINE_OS_MACOS 1
#elif defined(__linux__)
#define SCANENGINE_OS_LINUX 1
#else
#error "scanengine: unsupported platform"
#endif

#if defined(SCANENGINE_OS_WINDOWS)
// MSVC / clang-cl: QueryPerformanceCounter, which MSVC's STL uses to back
// std::chrono::steady_clock (VS2015+).
namespace scanengine {
inline constexpr const char* kClockBackendName = "QueryPerformanceCounter";
}
#elif defined(SCANENGINE_OS_MACOS)
// libc++'s steady_clock is mach_absolute_time-backed.
namespace scanengine {
inline constexpr const char* kClockBackendName = "mach_absolute_time";
}
#elif defined(SCANENGINE_OS_ANDROID) || defined(SCANENGINE_OS_LINUX)
// CLOCK_BOOTTIME survives suspend, unlike CLOCK_MONOTONIC — required for a
// capture device that may sleep mid-scan. glibc/bionic back steady_clock
// with CLOCK_MONOTONIC, so we call clock_gettime directly.
#include <ctime>
namespace scanengine {
inline constexpr const char* kClockBackendName = "CLOCK_BOOTTIME";
}
#endif

namespace scanengine {

// Nanoseconds since an unspecified epoch. Monotonic, never wall-clock, never
// persisted as an absolute value (only differences are meaningful across
// runs; .lscan stores it as the in-session timeline).
struct TimePoint {
  std::int64_t nanos = 0;

  friend bool operator==(const TimePoint&, const TimePoint&) = default;
  friend bool operator<(const TimePoint& a, const TimePoint& b) { return a.nanos < b.nanos; }
  friend bool operator>(const TimePoint& a, const TimePoint& b) { return b < a; }
  friend bool operator<=(const TimePoint& a, const TimePoint& b) { return !(b < a); }
  friend bool operator>=(const TimePoint& a, const TimePoint& b) { return !(a < b); }
};

class SteadyClock {
 public:
  static TimePoint now() noexcept {
#if defined(SCANENGINE_OS_ANDROID) || defined(SCANENGINE_OS_LINUX)
    struct timespec ts {};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return TimePoint{static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec};
#else
    const auto n = std::chrono::steady_clock::now().time_since_epoch();
    return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(n).count()};
#endif
  }

  static const char* backend_name() noexcept { return kClockBackendName; }
};

// Test/replay seam: a clock a test can drive. Drivers take a ClockFn so a
// replay run (engine_cli --replay, E2 golden datasets) can reproduce
// timestamps exactly instead of sampling the host clock.
using ClockFn = TimePoint (*)();
inline TimePoint steady_now() noexcept { return SteadyClock::now(); }

}  // namespace scanengine

#endif  // SCANENGINE_TIMESYNC_CLOCK_H
