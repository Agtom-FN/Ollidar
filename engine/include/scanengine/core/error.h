// error.h — the engine's one error model.
//
// Design constraints (this type crosses the C ABI into JNI and Qt):
//   1. ONE flat enum. No nested domains, no error categories, no
//      std::error_code. A JNI/Qt/FFI caller sees a plain int32_t.
//   2. Values are STABLE and APPEND-ONLY. Never renumber, never reuse a
//      retired value — a shipped Android app may hold an older header.
//      capi/scanengine_c.cpp static_asserts that the C mirror of this enum
//      still agrees value-for-value, so drift is a compile error.
//   3. No exceptions cross a public API boundary. Internally the engine may
//      throw (std::bad_alloc); every public entry point converts to
//      ScanError. The C ABI additionally wraps every call in a catch-all.
//   4. The enum carries the *class* of failure; a human-readable detail
//      string is attached out-of-band (thread-local last_error()) so the
//      enum never has to grow a value just to explain a variant.
//
// Owner: A1. Adding a value: append at the end, add it to error_str(), add
// the SCAN_ERR_* mirror in capi/scanengine_c.h. That is the whole procedure.
#ifndef SCANENGINE_CORE_ERROR_H
#define SCANENGINE_CORE_ERROR_H

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace scanengine {

enum class ScanError : std::int32_t {
  kOk = 0,

  // --- generic --------------------------------------------------------
  kUnknown = 1,           // last resort; always accompanied by a detail string
  kInvalidArgument = 2,   // caller passed null/out-of-range/nonsense
  kInvalidState = 3,      // legal call, wrong lifecycle state (see EngineState)
  kNotFound = 4,          // handle/id/name does not resolve
  kAlreadyExists = 5,
  kNotSupported = 6,      // not possible on this platform/build/device
  kUnimplemented = 7,     // a seam a later workstream task fills in
  kOutOfMemory = 8,
  kCancelled = 9,
  kTimeout = 10,
  kBusy = 11,             // resource in use; retry later
  kAgain = 12,            // non-fatal "nothing right now" (empty event queue)
  kCapacityExceeded = 13, // bounded queue/store full; data was dropped

  // --- I/O and transport ----------------------------------------------
  kIoError = 20,
  kDisconnected = 21,     // link was up and went away
  kPermissionDenied = 22, // USB permission, socket bind, file ACL
  kNetworkError = 23,

  // --- device / protocol ----------------------------------------------
  kDeviceNotResponding = 30,  // no ACK / no data within the driver's deadline
  kProtocolError = 31,        // framing/sequence violated the device spec
  kChecksumFailed = 32,
  kDeviceFault = 33,          // device reported an error state (e.g. D6 error ACK)

  // --- persistence / data ---------------------------------------------
  kCorruptData = 40,
  kVersionMismatch = 41,      // .lscan / ABI version the code cannot read
  kFileError = 42,
};

// Human-readable, never localized, safe to log. Stable strings — tests and
// CI logs match on them.
const char* error_str(ScanError e) noexcept;

inline bool ok(ScanError e) noexcept { return e == ScanError::kOk; }

// --- thread-local error detail ------------------------------------------
//
// The detail string belongs to the *calling thread*, is overwritten by the
// next failing call on that thread, and is valid until then. This is the
// errno/GetLastError contract, chosen because it needs no allocation
// discipline across the C ABI: `scan_engine_last_error()` returns a pointer
// the caller must copy before its next engine call.
ScanError set_last_error(ScanError e, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
const char* last_error_message() noexcept;
ScanError last_error_code() noexcept;
void clear_last_error() noexcept;

// --- Status / Result ------------------------------------------------------
//
// Status is a [[nodiscard]] ScanError wrapper: a returned failure cannot be
// silently ignored. Result<T> is Status plus a value. Deliberately tiny — no
// monadic chaining, no allocation, no exceptions.
class [[nodiscard]] Status {
 public:
  Status() = default;  // OK
  Status(ScanError e) : err_(e) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return err_ == ScanError::kOk; }
  explicit operator bool() const noexcept { return ok(); }
  ScanError error() const noexcept { return err_; }
  const char* message() const noexcept { return error_str(err_); }

  friend bool operator==(Status a, Status b) noexcept { return a.err_ == b.err_; }
  friend bool operator!=(Status a, Status b) noexcept { return a.err_ != b.err_; }

 private:
  ScanError err_ = ScanError::kOk;
};

inline const Status kOkStatus{};

template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(ScanError e) : err_(e) {}               // NOLINT(google-explicit-constructor)
  Result(Status s) : err_(s.error()) {}          // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return err_ == ScanError::kOk; }
  explicit operator bool() const noexcept { return ok(); }
  ScanError error() const noexcept { return err_; }
  Status status() const noexcept { return Status(err_); }

  // Precondition: ok(). Callers that ignore this get a default-constructed /
  // moved-from value, never UB from a null deref, because the value is a
  // member and not a pointer.
  T& value() & { return value_; }
  const T& value() const& { return value_; }
  T&& value() && { return std::move(value_); }

  T* operator->() { return &value_; }
  const T* operator->() const { return &value_; }
  T& operator*() & { return value_; }

 private:
  ScanError err_ = ScanError::kOk;
  T value_{};
};

// SCAN_TRY(expr) — propagate a failing Status from a function returning
// Status/Result. Keeps error plumbing from drowning out the logic.
#define SCAN_TRY(expr)                                     \
  do {                                                     \
    ::scanengine::Status scan_try_status_ = (expr);         \
    if (!scan_try_status_.ok()) return scan_try_status_;    \
  } while (0)

}  // namespace scanengine

#endif  // SCANENGINE_CORE_ERROR_H
