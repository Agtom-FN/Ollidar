#include "scanengine/core/error.h"

#include <cstdarg>
#include <cstdio>

namespace scanengine {
namespace {

constexpr std::size_t kDetailCap = 512;

struct ErrorSlot {
  ScanError code = ScanError::kOk;
  char detail[kDetailCap] = {0};
};

// One slot per thread; see error.h for the errno-style contract.
ErrorSlot& slot() {
  thread_local ErrorSlot s;
  return s;
}

}  // namespace

const char* error_str(ScanError e) noexcept {
  switch (e) {
    case ScanError::kOk: return "ok";
    case ScanError::kUnknown: return "unknown error";
    case ScanError::kInvalidArgument: return "invalid argument";
    case ScanError::kInvalidState: return "invalid state";
    case ScanError::kNotFound: return "not found";
    case ScanError::kAlreadyExists: return "already exists";
    case ScanError::kNotSupported: return "not supported";
    case ScanError::kUnimplemented: return "unimplemented";
    case ScanError::kOutOfMemory: return "out of memory";
    case ScanError::kCancelled: return "cancelled";
    case ScanError::kTimeout: return "timeout";
    case ScanError::kBusy: return "busy";
    case ScanError::kAgain: return "try again";
    case ScanError::kCapacityExceeded: return "capacity exceeded";
    case ScanError::kIoError: return "I/O error";
    case ScanError::kDisconnected: return "disconnected";
    case ScanError::kPermissionDenied: return "permission denied";
    case ScanError::kNetworkError: return "network error";
    case ScanError::kDeviceNotResponding: return "device not responding";
    case ScanError::kProtocolError: return "protocol error";
    case ScanError::kChecksumFailed: return "checksum failed";
    case ScanError::kDeviceFault: return "device fault";
    case ScanError::kCorruptData: return "corrupt data";
    case ScanError::kVersionMismatch: return "version mismatch";
    case ScanError::kFileError: return "file error";
  }
  return "unrecognized error code";
}

ScanError set_last_error(ScanError e, const char* fmt, ...) {
  ErrorSlot& s = slot();
  s.code = e;
  if (fmt == nullptr) {
    s.detail[0] = '\0';
    return e;
  }
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(s.detail, sizeof(s.detail), fmt, ap);
  va_end(ap);
  return e;
}

const char* last_error_message() noexcept {
  const ErrorSlot& s = slot();
  return s.detail[0] != '\0' ? s.detail : error_str(s.code);
}

ScanError last_error_code() noexcept { return slot().code; }

void clear_last_error() noexcept {
  ErrorSlot& s = slot();
  s.code = ScanError::kOk;
  s.detail[0] = '\0';
}

}  // namespace scanengine
