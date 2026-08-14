// clock.hpp — per-OS monotonic clock backend, per Tech Spec §3.2:
// "Single monotonic engine clock (CLOCK_BOOTTIME / mach_absolute_time /
// QueryPerformanceCounter)."
//
// This header proves the engine can select a platform-appropriate monotonic
// time source at compile time on every target OS (Android, macOS, Windows,
// Linux) and exposes it through one portable type, `scanengine::SteadyClock`,
// so downstream code (timesync/, record/) never needs #ifdef.
#pragma once

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
// MSVC / clang-cl path: QueryPerformanceCounter, wrapped by
// std::chrono::steady_clock in every conforming STL (MSVC's steady_clock is
// QPC-backed since VS2015). We still declare the platform tag explicitly so
// timesync/ can log which backend is in effect, per the spec's per-OS clock
// requirement.
namespace scanengine {
inline constexpr const char* kClockBackendName = "QueryPerformanceCounter";
}
#elif defined(SCANENGINE_OS_MACOS)
// macOS/iOS path: mach_absolute_time(). libc++'s steady_clock is
// mach_absolute_time-backed, so std::chrono::steady_clock is sufficient and
// portable here too; we still surface the backend name for parity with the
// other platforms.
namespace scanengine {
inline constexpr const char* kClockBackendName = "mach_absolute_time";
}
#elif defined(SCANENGINE_OS_ANDROID) || defined(SCANENGINE_OS_LINUX)
// Android/Linux path: CLOCK_BOOTTIME (survives suspend, unlike
// CLOCK_MONOTONIC — important for a capture device that may sleep mid-scan).
// std::chrono::steady_clock on glibc/bionic is CLOCK_MONOTONIC, NOT
// CLOCK_BOOTTIME, so we go straight to clock_gettime here.
#include <ctime>
namespace scanengine {
inline constexpr const char* kClockBackendName = "CLOCK_BOOTTIME";
}
#endif

namespace scanengine {

// A single monotonic time point, expressed in nanoseconds since an
// unspecified epoch (never wall-clock, never persisted — matches spec
// §3.2's "arrival-stamping" model).
struct TimePoint {
    std::int64_t nanos = 0;

    friend bool operator==(const TimePoint&, const TimePoint&) = default;
    friend bool operator<(const TimePoint& a, const TimePoint& b) {
        return a.nanos < b.nanos;
    }
};

// SteadyClock::now() is the one function every driver/, timesync/, and
// record/ module call to timestamp an arriving sample.
class SteadyClock {
public:
    static TimePoint now() noexcept {
#if defined(SCANENGINE_OS_ANDROID) || defined(SCANENGINE_OS_LINUX)
        struct timespec ts{};
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return TimePoint{static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL +
                          ts.tv_nsec};
#else
        // Windows (QPC) and macOS (mach_absolute_time) are both already
        // wrapped correctly by libc++/MSVC-STL's steady_clock.
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return TimePoint{std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()};
#endif
    }

    static const char* backend_name() noexcept { return kClockBackendName; }
};

}  // namespace scanengine
