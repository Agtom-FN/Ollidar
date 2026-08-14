// log.h — leveled logging facade with an app-installed sink.
//
// Why not spdlog/fmt: the engine ships inside an APK and inside a Qt desktop
// app; both already own a logging destination (Logcat / QLoggingCategory /
// a file). A third-party logger would (a) add a vcpkg dependency to every
// platform leg of CI for no functional gain, (b) fight the host app's own
// sink. So: printf-style formatting into a fixed buffer, one global sink
// callback, no allocation on the hot path.
//
// Threading: set_sink()/set_min_level() are guarded by a mutex and are
// expected at startup. Emission takes the same mutex around the sink call,
// so a sink implementation does NOT need to be thread-safe itself, but it
// MUST be quick and MUST NOT call back into the engine (deadlock).
//
// Owner: A1. Every module logs with SCAN_LOG_*(module, ...) where `module`
// is the directory name ("d6", "mid360", "record", ...).
#ifndef SCANENGINE_CORE_LOG_H
#define SCANENGINE_CORE_LOG_H

#include <cstdint>

namespace scanengine {

enum class LogLevel : std::int32_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kOff = 5,  // only valid as a min-level
};

const char* to_string(LogLevel l) noexcept;

// `module` and `message` are NUL-terminated and valid only for the duration
// of the call — copy if you keep them.
using LogSink = void (*)(LogLevel level, const char* module, const char* message,
                         void* user_data);

// Install the app's sink. nullptr restores the default (stderr) sink.
void set_log_sink(LogSink sink, void* user_data) noexcept;
void set_log_min_level(LogLevel level) noexcept;
LogLevel log_min_level() noexcept;

// Cheap pre-check so callers can skip formatting.
inline bool log_enabled(LogLevel l) noexcept {
  return static_cast<std::int32_t>(l) >= static_cast<std::int32_t>(log_min_level());
}

void log_message(LogLevel level, const char* module, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define SCAN_LOG(level, module, ...)                              \
  do {                                                            \
    if (::scanengine::log_enabled(level))                         \
      ::scanengine::log_message((level), (module), __VA_ARGS__);  \
  } while (0)

#define SCAN_LOG_TRACE(module, ...) SCAN_LOG(::scanengine::LogLevel::kTrace, module, __VA_ARGS__)
#define SCAN_LOG_DEBUG(module, ...) SCAN_LOG(::scanengine::LogLevel::kDebug, module, __VA_ARGS__)
#define SCAN_LOG_INFO(module, ...) SCAN_LOG(::scanengine::LogLevel::kInfo, module, __VA_ARGS__)
#define SCAN_LOG_WARN(module, ...) SCAN_LOG(::scanengine::LogLevel::kWarn, module, __VA_ARGS__)
#define SCAN_LOG_ERROR(module, ...) SCAN_LOG(::scanengine::LogLevel::kError, module, __VA_ARGS__)

}  // namespace scanengine

#endif  // SCANENGINE_CORE_LOG_H
