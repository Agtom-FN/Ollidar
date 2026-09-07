// FieldLog.h — the always-on field-test log, and the two raw fixture writers
// that ride along with it.
//
// WHY THIS EXISTS
//
// The Mid-70 + serial-IMU rig (NOTES.md §20) has never met real hardware. The
// first person to run it will be the owner, on a DIFFERENT Mac, from a DMG,
// with no terminal, no debugger and no second chance at the moment a thing goes
// wrong. Everything this app knows at that moment — what discovery heard, what
// config was armed, what the driver's own counters said two seconds before the
// failure, and the raw bytes on the wire — has to already be on disk when they
// think to ask for it. So there is no "enable logging" switch: the file is
// opened in main() before the MainWindow exists and it is flushed on every
// line, because the honest expectation is that the app will be force-quit
// mid-test rather than closed politely.
//
// THREE THINGS, ONE FILE PAIR
//
//   1. The log itself. `~/Library/Logs/LidarScan/lidarscan-<ts>.log`, one line
//      per event, `<iso8601 ms> [level][source] message`. Sources: Qt's own
//      message stream, the ENGINE's log sink (raised to debug for the file
//      only — the stderr/on-screen filter is left exactly where it was),
//      CaptureWindow::log(), EngineHost::logLine, discovery, arm/record/replay
//      events, and a 2 s stats snapshot per armed device.
//
//   2. Raw fixtures. The Mid-70's broadcast datagrams and the IMU module's
//      serial bytes, in the EXACT containers tools/remote-capture/
//      capture_mid70.py and capture_serial_imu.py write, so a field capture
//      can be replayed by the engine's own tests with no conversion step and
//      no second tool on the rig. §20.3 lists these fixtures as the things
//      that do not exist yet; this is how they get home.
//
//   3. The diagnostics bundle. One Help-menu click (or --save-diagnostics)
//      collects the log, the fixtures, the last .lscan and a machine profile
//      into a folder on the Desktop that can be zipped and sent.
//
// THREADING. Every static here is safe from any thread: the engine's log sink
// fires on driver threads, the discovery raw sink on the discovery worker, and
// SerialReader on the GUI thread. One mutex covers the file descriptors; the
// writes themselves are write(2), so a line is on disk when the call returns.
//
// THE CRASH HOOK is the one part that is NOT allowed to take that mutex.
// installCrashHandlers() arms SIGSEGV/SIGBUS/SIGABRT/SIGFPE with a handler that
// does exactly three async-signal-safe things — patch two digits into a
// prebuilt buffer, write(2) it to the already-open fd, re-raise — and nothing
// else. No malloc, no Qt, no locking. A handler that formatted a nice message
// would deadlock against whichever thread was mid-write when the fault hit.
//
// Owner: the Mid-70 field-test wave (desktop only).
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstddef>
#include <cstdint>

namespace lidarscan {

class EngineHost;

class FieldLog {
 public:
  enum class Level { kDebug = 0, kInfo, kWarn, kError };

  // ---- lifecycle ---------------------------------------------------------

  // Opens the log, writes the header, installs the Qt message handler, the
  // engine log sink and the crash handlers. Idempotent; returns false only if
  // no writable directory could be found at all (in which case every other
  // call here is a cheap no-op and the app runs exactly as it did before).
  static bool open();

  // Footer (uptime + fixture byte counts) and close. Called from main() on the
  // way out — NOT from a static destructor, which would run after the objects
  // whose teardown we still want to see.
  static void close();

  static bool isOpen();
  static QString path();     // "" when not open
  static QString logDir();   // the directory, even when the log is not open
  // Every file THIS RUN created under logDir(): the log plus any fixtures.
  static QStringList filesThisRun();

  // ---- writing -----------------------------------------------------------

  static void write(Level lvl, const char* source, const QString& msg);
  // Same, for a source computed at run time (the engine's module name).
  static void write(Level lvl, const QString& source, const QString& msg);
  static void debug(const char* src, const QString& m) { write(Level::kDebug, src, m); }
  static void info(const char* src, const QString& m) { write(Level::kInfo, src, m); }
  static void warn(const char* src, const QString& m) { write(Level::kWarn, src, m); }
  static void error(const char* src, const QString& m) { write(Level::kError, src, m); }

  // ---- Part 2: raw fixtures ---------------------------------------------

  // One Mid-70 SDK v1 broadcast datagram, verbatim, in capture_mid70.py's
  // LX70_CAP container. The file is created lazily on the FIRST datagram (a run
  // that never hears one leaves no empty file behind) and closed by close().
  // Called from the discovery worker thread via DiscoverOptions::raw_sink.
  static void mid70Datagram(const std::uint8_t* data, std::size_t len, const char* source_ip,
                            std::uint16_t port);
  static std::uint64_t mid70Datagrams();

  // One serial-IMU dump per port open, in capture_serial_imu.py's IMUSRCAP
  // container. `baud` is the container's whole port table (there is no port
  // number in that format — see the script's docstring).
  static void openImuDump(const QString& port_name, int baud);
  static void imuBytes(const char* data, std::size_t len);
  static void closeImuDump(const QString& why);

  // ---- Part 3: the diagnostics bundle ------------------------------------

  // Builds ~/Desktop/LidarScan-diagnostics-<ts>/ and returns its path ("" on
  // failure, with *err set). `reveal` opens it in Finder — true for the Help
  // menu, false for the headless CLI hook.
  static QString saveDiagnosticsBundle(bool reveal, QString* err);

 private:
  FieldLog() = delete;
};

// The 2 s stats snapshot (spec item f). Deliberately NOT a QObject subclass
// with signals of its own: it owns one QTimer, reads the host it was given and
// writes lines. main() owns one for the whole process, which is also why it
// keeps working after a CaptureWindow has been created and destroyed.
class FieldLogStats {
 public:
  explicit FieldLogStats(EngineHost* host);

  void start(int interval_ms = 2000);
  void stop();

  // One snapshot, now. Writes nothing when no device is registered — an idle
  // app must not fill the log with "nothing is armed" every two seconds.
  void snapshotNow();

 private:
  EngineHost* host_ = nullptr;
  QTimer timer_;
  bool was_armed_ = false;
};

}  // namespace lidarscan
