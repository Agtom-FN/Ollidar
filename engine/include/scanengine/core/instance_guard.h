// instance_guard.h — one LidarScan per machine (owner round-4 item 6).
//
// WHY: the field session's session-2 note, verbatim — "a first attempt left a
// port-holding process; later runs failed SdkInit until killed — single-
// instance guard worth considering". Every capture path in this engine binds
// FIXED UDP ports (56100–56501) or claims an exclusive serial handle. A second
// LidarScan does not fail cleanly: it fails deep inside SdkInit with a message
// about a socket, minutes into a setup the operator thinks is working.
//
// WHAT IT IS: an advisory lockfile, claimed once at app start and held for the
// process lifetime. Advisory, not mandatory — a guard is a courtesy to the
// operator, not a security boundary, and nothing here tries to stop a
// determined second process.
//
// WHO CALLS IT: the APPS, at startup, before creating an Engine.
// Engine::create() deliberately does NOT claim it (see docs/A16-discovery.md
// §6): the cloud worker runs several engines per host on purpose, the test
// binary builds dozens, and `engine_cli --post` is expected to run beside a
// live capture. Only a capture UI has the "one at a time" requirement.
//
// PLATFORM BEHAVIOUR
//   POSIX   flock(LOCK_EX|LOCK_NB) on the lockfile. The kernel drops the lock
//           when the process dies, including SIGKILL, so a crash cannot leave
//           a stale claim.
//   Windows LockFileEx(LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY) on
//           byte 0. Same crash semantics.
//   Neither Falls back to the recorded pid + a liveness check, which is what
//           the pid path below is for; it is also the path the tests drive.
//
// Owner: A16.
#ifndef SCANENGINE_CORE_INSTANCE_GUARD_H
#define SCANENGINE_CORE_INSTANCE_GUARD_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "scanengine/core/error.h"

namespace scanengine {

struct InstanceGuardOptions {
  // Identifies the lock, not the file. The default path is
  // <temp>/lidarscan-<app_id>.lock — /tmp on POSIX, %TEMP% on Windows.
  std::string app_id = "lidarscan";

  // Overrides the derived path entirely. Tests use this; so should anything
  // that wants a per-user rather than per-machine guard (point it inside
  // $XDG_RUNTIME_DIR / ~/Library/Application Support).
  std::string lock_path;

  // Take the OS advisory lock. Tests set this false to exercise the pid
  // fallback deterministically; production code should not.
  bool use_os_lock = true;

  // How "is that pid still alive?" is answered. Default: kill(pid, 0) on
  // POSIX, OpenProcess on Windows. Injectable so a test can simulate a
  // FOREIGN live holder without spawning one.
  std::function<bool(std::int64_t pid)> pid_is_alive;
};

// RAII. Acquire in main(), keep it alive for the process, let the destructor
// release it. Movable so it can live in a unique_ptr or be returned.
class InstanceGuard {
 public:
  InstanceGuard();
  ~InstanceGuard();
  InstanceGuard(InstanceGuard&&) noexcept;
  InstanceGuard& operator=(InstanceGuard&&) noexcept;
  InstanceGuard(const InstanceGuard&) = delete;
  InstanceGuard& operator=(const InstanceGuard&) = delete;

  // kOk           the claim is ours (or was already ours — see below)
  // kBusy         another LIVE process holds it; holder_pid() says which, and
  //               last_error_message() is the operator-facing sentence
  //               "another LidarScan is running (pid 4242)"
  // kFileError    the lock directory is not writable
  //
  // SAME-PROCESS RE-ENTRY IS kOk. Two guards on one path in one process is
  // not a violation of "one LidarScan per machine" — it is a library being
  // initialized twice, an app with a plugin, or the test suite. The claim is
  // refcounted per path per process; the OS lock is taken once by the first
  // and released by the last.
  Status Acquire(const InstanceGuardOptions& opt = {});

  // Idempotent; the destructor calls it.
  void Release();

  bool held() const;
  // Set when Acquire() returned kBusy: the pid recorded in the lockfile, or 0
  // if the file carried none.
  std::int64_t holder_pid() const;
  // True when this guard joined a claim its own process already held.
  bool same_process() const;
  const std::string& lock_path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// This process's pid, as the guard records it.
std::int64_t CurrentProcessId();

}  // namespace scanengine

#endif  // SCANENGINE_CORE_INSTANCE_GUARD_H
