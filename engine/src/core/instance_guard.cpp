// instance_guard.cpp — the advisory single-instance lock.
//
// Two layers, because neither alone is enough:
//
//   1. An OS ADVISORY LOCK (flock / LockFileEx). This is the authority. Its
//      one property that a pid file cannot match: the kernel releases it when
//      the process dies, however it dies. A SIGKILL'd LidarScan does not lock
//      the operator out of their own machine.
//   2. A RECORDED PID inside the file. This is what turns "busy" into a
//      sentence an operator can act on — "another LidarScan is running (pid
//      4242)" is actionable; "resource busy" is not. It is also the fallback
//      on a filesystem where the OS lock is unavailable or a lie (network
//      shares), where the liveness check does the deciding.
//
// Owner: A16. See include/scanengine/core/instance_guard.h for the contract
// and docs/A16-discovery.md §6 for why Engine::create() does not call this.
#include "scanengine/core/instance_guard.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "scanengine/core/log.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scanengine {
namespace {

constexpr const char* kMod = "instance";

#if defined(_WIN32)
using native_handle_t = HANDLE;
constexpr native_handle_t kInvalidHandle = INVALID_HANDLE_VALUE;
#else
using native_handle_t = int;
constexpr native_handle_t kInvalidHandle = -1;
#endif

// One claim per PATH per PROCESS, refcounted. This is what makes a second
// InstanceGuard in the same process succeed instead of deadlocking against
// its own flock: on POSIX a second flock() from the same process on a
// DIFFERENT file description fails with EWOULDBLOCK, which would otherwise
// report the process as its own rival.
struct Claim {
  int refs = 0;
  native_handle_t handle = kInvalidHandle;
};

std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, Claim>& registry() {
  static std::map<std::string, Claim> r;
  return r;
}

std::string temp_dir() {
#if defined(_WIN32)
  char buf[MAX_PATH + 1] = {0};
  const DWORD n = ::GetTempPathA(sizeof(buf), buf);
  if (n > 0 && n < sizeof(buf)) return buf;
  return ".\\";
#else
  if (const char* t = std::getenv("TMPDIR")) {
    if (t[0] != '\0') {
      std::string s = t;
      if (s.back() != '/') s += '/';
      return s;
    }
  }
  return "/tmp/";
#endif
}

// Keep the derived filename to characters that are legal everywhere, so an
// app_id with a slash, a colon or a ".." cannot escape the temp directory.
// '.' is deliberately NOT in the allowed set: separators alone would already
// make traversal impossible, but a filename containing ".." reads like a
// traversal in a log and invites the next reader to "fix" the wrong thing.
std::string sanitize(const std::string& id) {
  std::string s;
  for (const char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    s += ok ? c : '_';
  }
  if (s.empty()) s = "lidarscan";
  return s;
}

bool default_pid_is_alive(std::int64_t pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  DWORD code = 0;
  const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  ::CloseHandle(h);
  return alive;
#else
  // EPERM means it exists and belongs to somebody else — still alive, and
  // still a reason to refuse.
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#endif
}

std::int64_t read_recorded_pid(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return 0;
  char buf[64] = {0};
  const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  if (n == 0) return 0;
  return static_cast<std::int64_t>(std::strtoll(buf, nullptr, 10));
}

}  // namespace

std::int64_t CurrentProcessId() {
#if defined(_WIN32)
  return static_cast<std::int64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::int64_t>(::getpid());
#endif
}

struct InstanceGuard::Impl {
  std::string path;
  bool held = false;
  bool same_process = false;
  std::int64_t holder_pid = 0;
};

InstanceGuard::InstanceGuard() : impl_(new Impl) {}
InstanceGuard::~InstanceGuard() { Release(); }
InstanceGuard::InstanceGuard(InstanceGuard&&) noexcept = default;
InstanceGuard& InstanceGuard::operator=(InstanceGuard&& other) noexcept {
  if (this != &other) {
    Release();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

bool InstanceGuard::held() const { return impl_ && impl_->held; }
std::int64_t InstanceGuard::holder_pid() const { return impl_ ? impl_->holder_pid : 0; }
bool InstanceGuard::same_process() const { return impl_ && impl_->same_process; }
const std::string& InstanceGuard::lock_path() const {
  static const std::string kEmpty;
  return impl_ ? impl_->path : kEmpty;
}

Status InstanceGuard::Acquire(const InstanceGuardOptions& opt) {
  if (!impl_) impl_.reset(new Impl);
  if (impl_->held) return kOkStatus;  // idempotent

  const std::string path =
      !opt.lock_path.empty() ? opt.lock_path
                             : temp_dir() + "lidarscan-" + sanitize(opt.app_id) + ".lock";
  impl_->path = path;
  impl_->holder_pid = 0;
  impl_->same_process = false;

  const std::function<bool(std::int64_t)> alive =
      opt.pid_is_alive ? opt.pid_is_alive
                       : std::function<bool(std::int64_t)>(&default_pid_is_alive);
  const std::int64_t me = CurrentProcessId();

  std::lock_guard<std::mutex> lock(registry_mutex());

  // --- already ours? -------------------------------------------------------
  auto it = registry().find(path);
  if (it != registry().end()) {
    ++it->second.refs;
    impl_->held = true;
    impl_->same_process = true;
    impl_->holder_pid = me;
    return kOkStatus;
  }

  // --- open (and create) the lockfile -------------------------------------
#if defined(_WIN32)
  // FILE_SHARE_READ|WRITE so the RIVAL can open it too and read our pid; the
  // exclusion comes from LockFileEx, not from the share mode.
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return set_last_error(ScanError::kFileError, "instance guard: cannot open %s (%lu)",
                          path.c_str(), static_cast<unsigned long>(::GetLastError()));
  }
  if (opt.use_os_lock) {
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    if (!::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
      ::CloseHandle(h);
      impl_->holder_pid = read_recorded_pid(path);
      return set_last_error(ScanError::kBusy, "another LidarScan is running (pid %lld)",
                            static_cast<long long>(impl_->holder_pid));
    }
  }
#else
  const int h = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (h < 0) {
    return set_last_error(ScanError::kFileError, "instance guard: cannot open %s (errno %d)",
                          path.c_str(), errno);
  }
  if (opt.use_os_lock) {
    if (::flock(h, LOCK_EX | LOCK_NB) != 0) {
      const int e = errno;
      if (e == EWOULDBLOCK || e == EAGAIN) {
        ::close(h);
        impl_->holder_pid = read_recorded_pid(path);
        return set_last_error(ScanError::kBusy, "another LidarScan is running (pid %lld)",
                              static_cast<long long>(impl_->holder_pid));
      }
      // flock is not supported here (some network filesystems). Keep the
      // descriptor and let the pid layer below decide, rather than refusing
      // to start at all because of a filesystem quirk.
      SCAN_LOG_WARN(kMod, "flock(%s) failed with errno %d; falling back to the pid check",
                    path.c_str(), e);
    }
  }
#endif

  // --- the pid layer -------------------------------------------------------
  //
  // Reached with the OS lock in hand (or deliberately skipped). A recorded pid
  // that is alive and is NOT us means the OS lock did not do its job — a
  // network filesystem, or a build with use_os_lock off. Believe the pid.
  const std::int64_t recorded = read_recorded_pid(path);
  if (recorded != 0 && recorded != me && alive(recorded)) {
#if defined(_WIN32)
    ::CloseHandle(h);
#else
    ::close(h);
#endif
    impl_->holder_pid = recorded;
    return set_last_error(ScanError::kBusy, "another LidarScan is running (pid %lld)",
                          static_cast<long long>(recorded));
  }

  // --- claim it ------------------------------------------------------------
  char rec[128];
  const int len = std::snprintf(rec, sizeof(rec), "%lld\n%s\n", static_cast<long long>(me),
                                sanitize(opt.app_id).c_str());
#if defined(_WIN32)
  ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
  ::SetEndOfFile(h);
  DWORD written = 0;
  (void)::WriteFile(h, rec, static_cast<DWORD>(len), &written, nullptr);
  (void)::FlushFileBuffers(h);
#else
  (void)::ftruncate(h, 0);
  (void)::lseek(h, 0, SEEK_SET);
  const ssize_t w = ::write(h, rec, static_cast<std::size_t>(len));
  (void)w;
#endif

  Claim c;
  c.refs = 1;
  c.handle = h;
  registry().emplace(path, c);
  impl_->held = true;
  impl_->holder_pid = me;
  SCAN_LOG_INFO(kMod, "single-instance lock held: %s (pid %lld)", path.c_str(),
                static_cast<long long>(me));
  return kOkStatus;
}

void InstanceGuard::Release() {
  if (!impl_ || !impl_->held) return;
  impl_->held = false;
  // A same-process joiner holds a reference like any other holder; the LAST
  // one out is the one that drops the OS lock.
  impl_->same_process = false;
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto it = registry().find(impl_->path);
  if (it == registry().end()) return;
  if (--it->second.refs > 0) return;
#if defined(_WIN32)
  if (it->second.handle != kInvalidHandle) {
    OVERLAPPED ov;
    std::memset(&ov, 0, sizeof(ov));
    (void)::UnlockFileEx(it->second.handle, 0, 1, 0, &ov);
    ::CloseHandle(it->second.handle);
  }
#else
  if (it->second.handle != kInvalidHandle) {
    (void)::flock(it->second.handle, LOCK_UN);
    ::close(it->second.handle);
  }
#endif
  // The FILE stays. Deleting it races a rival that has already opened it, and
  // a leftover file with a dead pid is harmless — that is exactly the case
  // the liveness check exists for.
  registry().erase(it);
}

}  // namespace scanengine
