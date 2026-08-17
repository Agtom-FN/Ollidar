// DisplayAwake.h — keep the screen (and the machine) awake while a capture is
// live. Round-5 item 18, "walkthrough-first": the operator is WALKING the space
// with the rig, not sitting at the keyboard, so there is no input for minutes at
// a time — and a display that sleeps mid-scan hides the very live preview the
// round-5 flow is built around. Worse, on macOS an idle-sleeping machine
// suspends the app's threads, which is a data-loss risk for an open recording.
//
// SCOPE: this holds the assertion for as long as a device is armed (live preview
// or recording). It does NOT prevent a user-initiated sleep (closing the lid,
// choosing Sleep) anywhere — no platform API here can, and none should.
//
// PER-PLATFORM HONESTY (the reason this is one small class with three bodies
// rather than a promise):
//   * macOS   — REAL. IOPMAssertionCreateWithName with
//               kIOPMAssertionTypePreventUserIdleDisplaySleep, released by id.
//               IOKit is already linked (desktop/CMakeLists.txt), and IOPMLib.h
//               is a plain C header, so this needs no Objective-C++.
//   * Windows — REAL. SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED
//               | ES_SYSTEM_REQUIRED), cleared with ES_CONTINUOUS. Documented
//               caveat: it is per-THREAD state, so it must be set from the same
//               thread that clears it — this class is GUI-thread only.
//   * Linux   — NOT IMPLEMENTED, and says so out loud (reason() below and one
//               log line). The portable route is a DBus inhibit
//               (org.freedesktop.ScreenSaver / login1 Inhibit) or shelling out
//               to systemd-inhibit; Qt has no cross-desktop API for it and this
//               task is not the place to add a DBus dependency. NOTES.md §17
//               records this as a real gap rather than implying coverage.
//
// Owner: round-5 workflow pass.
#pragma once

#include <QString>

namespace lidarscan {

class DisplayAwake {
 public:
  DisplayAwake() = default;
  ~DisplayAwake();  // releases if still held — a capture window that is
                    // destroyed mid-session must not leak the assertion

  DisplayAwake(const DisplayAwake&) = delete;
  DisplayAwake& operator=(const DisplayAwake&) = delete;

  // Idempotent both ways. Returns true if the platform is actually holding
  // something off; false means "not supported here" (see reason()).
  bool acquire(const QString& why);
  void release();

  bool held() const { return held_; }
  // Human-readable, for the log and the capture panel's own hint: what this
  // platform did or did not manage.
  QString reason() const { return reason_; }

 private:
  bool held_ = false;
  QString reason_;
#if defined(Q_OS_MACOS)
  unsigned int assertion_id_ = 0;  // IOPMAssertionID
#endif
};

}  // namespace lidarscan
