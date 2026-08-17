#include "app/DisplayAwake.h"

#include <QtGlobal>

#if defined(Q_OS_MACOS)
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace lidarscan {

DisplayAwake::~DisplayAwake() { release(); }

bool DisplayAwake::acquire(const QString& why) {
  if (held_) return true;
#if defined(Q_OS_MACOS)
  CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, why.toUtf8().constData(),
                                               kCFStringEncodingUTF8);
  IOPMAssertionID id = 0;
  // PreventUserIdleDisplaySleep also implies system sleep prevention while the
  // display is on, which is exactly the pair a walking capture needs.
  const IOReturn rc = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                                  kIOPMAssertionLevelOn, name, &id);
  if (name) CFRelease(name);
  if (rc != kIOReturnSuccess) {
    reason_ = QString("macOS: IOPMAssertionCreateWithName failed (0x%1) — the display may "
                      "sleep during a capture")
                  .arg(quint32(rc), 0, 16);
    return false;
  }
  assertion_id_ = id;
  held_ = true;
  reason_ = "macOS: IOKit PreventUserIdleDisplaySleep assertion held";
  return true;
#elif defined(Q_OS_WIN)
  if (SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) == 0) {
    reason_ = "Windows: SetThreadExecutionState failed — the display may sleep during a capture";
    return false;
  }
  held_ = true;
  reason_ = "Windows: ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED held on the GUI thread";
  return true;
#else
  Q_UNUSED(why);
  // Deliberately not faking it: a wiggled cursor or a fake key event would be a
  // lie about what is being prevented, and would fight the user's own idle
  // settings. NOTES.md §17 carries this gap.
  reason_ =
      "Linux: not implemented — no DBus inhibit (org.freedesktop.ScreenSaver / login1) is "
      "wired up, so the display may sleep during a capture. Run with the screensaver off, "
      "or `systemd-inhibit --what=idle lidarscan`.";
  return false;
#endif
}

void DisplayAwake::release() {
  if (!held_) return;
  held_ = false;
#if defined(Q_OS_MACOS)
  if (assertion_id_ != 0) {
    IOPMAssertionRelease(assertion_id_);
    assertion_id_ = 0;
  }
  reason_ = "macOS: assertion released";
#elif defined(Q_OS_WIN)
  SetThreadExecutionState(ES_CONTINUOUS);
  reason_ = "Windows: execution state cleared";
#endif
}

}  // namespace lidarscan
