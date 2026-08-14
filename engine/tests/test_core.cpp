// Error model, logging facade, clock, timesync registry.
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "scanengine/core/error.h"
#include "scanengine/core/log.h"
#include "scanengine/timesync/offset_estimator.h"

using namespace scanengine;

TEST_CASE("error/status_is_ok_by_default") {
  Status s;
  CHECK(s.ok());
  CHECK(s.error() == ScanError::kOk);
  CHECK(bool(s));
}

TEST_CASE("error/status_carries_the_code") {
  Status s = ScanError::kTimeout;
  CHECK_FALSE(s.ok());
  CHECK(s.error() == ScanError::kTimeout);
  CHECK(std::string(s.message()) == "timeout");
}

TEST_CASE("error/every_enumerator_has_a_distinct_stable_string") {
  const ScanError all[] = {
      ScanError::kOk, ScanError::kUnknown, ScanError::kInvalidArgument,
      ScanError::kInvalidState, ScanError::kNotFound, ScanError::kAlreadyExists,
      ScanError::kNotSupported, ScanError::kUnimplemented, ScanError::kOutOfMemory,
      ScanError::kCancelled, ScanError::kTimeout, ScanError::kBusy, ScanError::kAgain,
      ScanError::kCapacityExceeded, ScanError::kIoError, ScanError::kDisconnected,
      ScanError::kPermissionDenied, ScanError::kNetworkError, ScanError::kDeviceNotResponding,
      ScanError::kProtocolError, ScanError::kChecksumFailed, ScanError::kDeviceFault,
      ScanError::kCorruptData, ScanError::kVersionMismatch, ScanError::kFileError};
  std::vector<std::string> seen;
  for (ScanError e : all) {
    const std::string s = error_str(e);
    CHECK(s != "unrecognized error code");
    for (const auto& prev : seen) CHECK(prev != s);
    seen.push_back(s);
  }
  // Numeric values are part of the ABI: spot-check the anchors.
  CHECK(static_cast<int>(ScanError::kOk) == 0);
  CHECK(static_cast<int>(ScanError::kIoError) == 20);
  CHECK(static_cast<int>(ScanError::kDeviceNotResponding) == 30);
  CHECK(static_cast<int>(ScanError::kCorruptData) == 40);
}

TEST_CASE("error/result_holds_a_value_or_an_error") {
  Result<int> good(42);
  CHECK(good.ok());
  CHECK(good.value() == 42);

  Result<int> bad(ScanError::kNotFound);
  CHECK_FALSE(bad.ok());
  CHECK(bad.error() == ScanError::kNotFound);
}

TEST_CASE("error/last_error_is_thread_local_and_formatted") {
  clear_last_error();
  const ScanError e = set_last_error(ScanError::kNotFound, "no device %u on bus %s", 7u, "usb");
  CHECK(e == ScanError::kNotFound);
  CHECK(std::string(last_error_message()) == "no device 7 on bus usb");
  CHECK(last_error_code() == ScanError::kNotFound);

  std::string other_thread_message;
  std::thread t([&] { other_thread_message = last_error_message(); });
  t.join();
  // A fresh thread has its own (empty) slot, so it reports the generic text.
  CHECK(other_thread_message == "ok");
  // ...and the original thread is untouched.
  CHECK(std::string(last_error_message()) == "no device 7 on bus usb");

  clear_last_error();
  CHECK(last_error_code() == ScanError::kOk);
}

TEST_CASE("error/SCAN_TRY_propagates") {
  auto inner = [](bool fail) -> Status {
    if (fail) return ScanError::kBusy;
    return kOkStatus;
  };
  auto outer = [&](bool fail) -> Status {
    SCAN_TRY(inner(fail));
    return kOkStatus;
  };
  CHECK(outer(false).ok());
  CHECK(outer(true).error() == ScanError::kBusy);
}

namespace {
struct LogCapture {
  std::vector<std::string> lines;
  static void sink(LogLevel level, const char* module, const char* message, void* user) {
    auto* self = static_cast<LogCapture*>(user);
    self->lines.push_back(std::string(to_string(level)) + "|" + module + "|" + message);
  }
};
}  // namespace

TEST_CASE("log/sink_receives_formatted_lines_and_level_filters") {
  LogCapture cap;
  set_log_sink(&LogCapture::sink, &cap);
  set_log_min_level(LogLevel::kInfo);

  SCAN_LOG_DEBUG("test", "invisible %d", 1);
  SCAN_LOG_INFO("test", "hello %s %d", "world", 42);
  SCAN_LOG_ERROR("test", "boom");

  CHECK(cap.lines.size() == 2);
  CHECK(cap.lines[0] == "info|test|hello world 42");
  CHECK(cap.lines[1] == "error|test|boom");

  set_log_min_level(LogLevel::kOff);
  SCAN_LOG_ERROR("test", "silenced");
  CHECK(cap.lines.size() == 2);

  set_log_sink(nullptr, nullptr);
  set_log_min_level(LogLevel::kInfo);
}

TEST_CASE("clock/is_monotonic_and_names_its_backend") {
  const TimePoint a = SteadyClock::now();
  const TimePoint b = SteadyClock::now();
  CHECK(b.nanos >= a.nanos);
  CHECK(a.nanos > 0);
  CHECK(std::string(SteadyClock::backend_name()).size() > 0);
}

TEST_CASE("timesync/registry_creates_a_passthrough_estimator_per_stream") {
  TimeSync ts;
  CHECK_FALSE(ts.estimate(StreamId::kLidarMid360).valid);

  OffsetEstimator& est = ts.estimator(StreamId::kLidarMid360);
  est.add_pair(1000, TimePoint{5000});
  const OffsetEstimate e = ts.estimate(StreamId::kLidarMid360);
  CHECK(e.valid);
  CHECK(e.offset_ns == 4000);
  CHECK(e.samples == 1);
  CHECK(est.to_engine_time(2000) == 6000);

  // Jitter is the residual against the previous estimate.
  est.add_pair(2000, TimePoint{6100});
  CHECK(ts.estimate(StreamId::kLidarMid360).jitter_ns == 100);

  ts.reset_all();
  CHECK_FALSE(ts.estimate(StreamId::kLidarMid360).valid);
}
