#include "scanengine/core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace scanengine {
namespace {

struct LogState {
  std::mutex mutex;
  LogSink sink = nullptr;
  void* user = nullptr;
};

LogState& state() {
  static LogState s;
  return s;
}

// Read on every log call, so keep it lock-free.
std::atomic<std::int32_t>& min_level_atomic() {
  static std::atomic<std::int32_t> level{static_cast<std::int32_t>(LogLevel::kInfo)};
  return level;
}

void default_sink(LogLevel level, const char* module, const char* message, void*) {
  std::fprintf(stderr, "[scanengine][%s][%s] %s\n", to_string(level), module, message);
}

}  // namespace

const char* to_string(LogLevel l) noexcept {
  switch (l) {
    case LogLevel::kTrace: return "trace";
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
    case LogLevel::kOff: return "off";
  }
  return "?";
}

void set_log_sink(LogSink sink, void* user_data) noexcept {
  LogState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.sink = sink;
  s.user = user_data;
}

void set_log_min_level(LogLevel level) noexcept {
  min_level_atomic().store(static_cast<std::int32_t>(level), std::memory_order_relaxed);
}

LogLevel log_min_level() noexcept {
  return static_cast<LogLevel>(min_level_atomic().load(std::memory_order_relaxed));
}

void log_message(LogLevel level, const char* module, const char* fmt, ...) {
  if (!log_enabled(level)) return;

  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  LogState& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.sink != nullptr) {
    s.sink(level, module != nullptr ? module : "engine", buf, s.user);
  } else {
    default_sink(level, module != nullptr ? module : "engine", buf, nullptr);
  }
}

}  // namespace scanengine
