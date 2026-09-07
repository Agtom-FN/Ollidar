#include "app/FieldLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QtEndian>
#include <QtGlobal>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "app/EngineHost.h"
#include "app/Project.h"

#include "scanengine/core/engine.h"
#include "scanengine/core/log.h"
#include "scanengine/core/types.h"
#include "scanengine/drivers/mid70/mid70_packets.h"

namespace lidarscan {
namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
//
// DELIBERATELY LEAKED. NOTES.md §16.3's static-destruction lesson applies here
// with more force than it did to the instance guard: the crash handler holds a
// raw fd out of this struct, the engine's log sink points into it, and both can
// fire during process teardown. A function-local static would be destroyed part
// way through that teardown and turn a crash we wanted to record into a second
// crash inside the recorder. One allocation, never freed, is the correct trade.
struct State {
  QMutex mutex;

  int fd = -1;
  QString path;
  QString dir;
  QString run_stamp;
  QElapsedTimer uptime;
  QStringList files;  // everything this run created under `dir`

  // capture_mid70.py's LX70_CAP container, opened on the first datagram.
  int m70_fd = -1;
  QString m70_path;
  quint64 m70_datagrams = 0;
  quint64 m70_bytes = 0;

  // capture_serial_imu.py's IMUSRCAP container, opened with the port.
  int imu_fd = -1;
  QString imu_path;
  QString imu_port;
  int imu_baud = 0;
  quint64 imu_records = 0;
  quint64 imu_bytes = 0;
  qint64 imu_first_ns = 0;
  int imu_open_count = 0;

  QtMessageHandler prev_qt_handler = nullptr;
  // What the engine's stderr sink was filtering at BEFORE we raised the level
  // to debug. The file gets everything; stderr and therefore the on-screen
  // pane keep the filter they had, which is the whole of "raise to debug for
  // the file only".
  scanengine::LogLevel engine_stderr_min = scanengine::LogLevel::kInfo;
  bool engine_sink_installed = false;
  bool open_done = false;
};

State* state() {
  static State* s = new State();
  return s;
}

// --- the crash hook's own storage ------------------------------------------
//
// Everything the handler touches is a plain POD with static storage duration,
// set up long before any signal can arrive. `g_crash_fd` is read, not locked:
// a torn read of an int is not possible on any platform this ships to, and the
// alternative (a mutex) is exactly what an async-signal-safe handler may not do.
volatile sig_atomic_t g_crash_fd = -1;
char g_crash_line[] = "\n!! FATAL SIGNAL 00 — LidarScan died here; nothing after this line !!\n";
std::size_t g_crash_len = sizeof(g_crash_line) - 1;
std::size_t g_crash_d0 = 0;  // index of the tens digit
std::size_t g_crash_d1 = 0;  // index of the units digit

extern "C" void fieldLogCrashHandler(int sig) {
  const int fd = int(g_crash_fd);
  if (fd >= 0) {
    const int s = (sig < 0 || sig > 99) ? 99 : sig;
    g_crash_line[g_crash_d0] = char('0' + (s / 10));
    g_crash_line[g_crash_d1] = char('0' + (s % 10));
    // One write(2) of a prebuilt buffer. No formatting, no allocation, no lock.
    // A short write here is not worth a retry loop: we are about to die and a
    // partial line is still the evidence that we did.
    const ssize_t n = ::write(fd, g_crash_line, g_crash_len);
    (void)n;
  }
  // Re-raise so the OS still produces its own crash report and the exit status
  // still says "killed by signal N". Swallowing the signal would hide the
  // crash from everything except this file.
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool writeAll(int fd, const char* data, std::size_t len) {
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    off += std::size_t(n);
  }
  return true;
}

const char* levelName(FieldLog::Level l) {
  switch (l) {
    case FieldLog::Level::kDebug: return "debug";
    case FieldLog::Level::kInfo: return "info";
    case FieldLog::Level::kWarn: return "warn";
    case FieldLog::Level::kError: return "error";
  }
  return "?";
}

QString nowStamp() { return QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss.zzz"); }

qint64 epochNs() { return QDateTime::currentMSecsSinceEpoch() * 1000000LL; }

void appendLE(QByteArray& b, quint16 v) {
  char t[2];
  qToLittleEndian(v, t);
  b.append(t, 2);
}
void appendLE(QByteArray& b, quint32 v) {
  char t[4];
  qToLittleEndian(v, t);
  b.append(t, 4);
}
void appendLE(QByteArray& b, quint64 v) {
  char t[8];
  qToLittleEndian(v, t);
  b.append(t, 8);
}

// `<QHI` from both capture scripts: 8-byte epoch ns, 2-byte port index,
// 4-byte length — 14 bytes, no padding, little-endian.
QByteArray dumpRecordHeader(qint64 t_ns, quint16 port_idx, quint32 len) {
  QByteArray r;
  r.reserve(14);
  appendLE(r, quint64(t_ns));
  appendLE(r, port_idx);
  appendLE(r, len);
  return r;
}

// `<8sHH` + N x u32 port table.
QByteArray dumpFileHeader(const char magic[8], quint32 port_entry) {
  QByteArray h;
  h.append(magic, 8);
  appendLE(h, quint16(1));  // version
  appendLE(h, quint16(1));  // num_ports
  appendLE(h, port_entry);
  return h;
}

int openForAppend(const QString& path) {
  return ::open(path.toLocal8Bit().constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

QString humanBytes(quint64 n) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = double(n);
  int i = 0;
  while (v >= 1024.0 && i < 4) {
    v /= 1024.0;
    ++i;
  }
  return QString("%1 %2").arg(v, 0, 'f', i == 0 ? 0 : 1).arg(u[i]);
}

// ---------------------------------------------------------------------------
// sinks
// ---------------------------------------------------------------------------

void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
  FieldLog::Level lvl = FieldLog::Level::kInfo;
  switch (type) {
    case QtDebugMsg: lvl = FieldLog::Level::kDebug; break;
    case QtInfoMsg: lvl = FieldLog::Level::kInfo; break;
    case QtWarningMsg: lvl = FieldLog::Level::kWarn; break;
    case QtCriticalMsg:
    case QtFatalMsg: lvl = FieldLog::Level::kError; break;
  }
  QString line = msg;
  if (ctx.file != nullptr && ctx.file[0] != '\0') {
    line += QString(" (%1:%2)").arg(QString::fromUtf8(ctx.file)).arg(ctx.line);
  }
  if (ctx.category != nullptr && std::strcmp(ctx.category, "default") != 0) {
    line = QString("[%1] %2").arg(QString::fromUtf8(ctx.category), line);
  }
  FieldLog::write(lvl, "qt", line);

  // The stream Qt would have produced without us still has to reach stderr:
  // every existing evidence hook in this app greps that stream.
  State* s = state();
  if (s->prev_qt_handler != nullptr) {
    s->prev_qt_handler(type, ctx, msg);
  } else {
    std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    std::fflush(stderr);
  }
}

void engineLogSink(scanengine::LogLevel level, const char* module, const char* message, void*) {
  FieldLog::Level lvl = FieldLog::Level::kInfo;
  switch (level) {
    case scanengine::LogLevel::kTrace:
    case scanengine::LogLevel::kDebug: lvl = FieldLog::Level::kDebug; break;
    case scanengine::LogLevel::kInfo: lvl = FieldLog::Level::kInfo; break;
    case scanengine::LogLevel::kWarn: lvl = FieldLog::Level::kWarn; break;
    case scanengine::LogLevel::kError: lvl = FieldLog::Level::kError; break;
    case scanengine::LogLevel::kOff: return;
  }
  const QString mod = QString("engine:%1").arg(QString::fromUtf8(module ? module : "?"));
  FieldLog::write(lvl, mod, QString::fromUtf8(message ? message : ""));

  // Reproduce log.cpp's default sink for anything that would have been printed
  // BEFORE we raised the min level — so the file gains debug lines and stderr
  // (and therefore the app's on-screen log pane) gains nothing.
  State* s = state();
  if (int(level) >= int(s->engine_stderr_min)) {
    std::fprintf(stderr, "[scanengine][%s][%s] %s\n", scanengine::to_string(level),
                 module ? module : "?", message ? message : "");
  }
}

// ---------------------------------------------------------------------------
// diagnostics-bundle helpers
// ---------------------------------------------------------------------------

QString runCommand(const QString& program, const QStringList& args, int timeout_ms) {
  QProcess p;
  p.setProcessChannelMode(QProcess::MergedChannels);
  p.start(program, args);
  if (!p.waitForStarted(5000)) {
    return QString("<could not start %1: %2>\n").arg(program, p.errorString());
  }
  if (!p.waitForFinished(timeout_ms)) {
    const QString partial = QString::fromUtf8(p.readAll());
    p.kill();
    p.waitForFinished(2000);
    return partial + QString("\n<TIMED OUT after %1 s>\n").arg(timeout_ms / 1000);
  }
  return QString::fromUtf8(p.readAll());
}

quint64 dirSizeBytes(const QString& path, quint64 stop_above) {
  quint64 total = 0;
  QDir root(path);
  const auto entries = root.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                          QDir::Hidden | QDir::System);
  for (const QFileInfo& fi : entries) {
    if (fi.isDir()) {
      total += dirSizeBytes(fi.absoluteFilePath(), stop_above > total ? stop_above - total : 0);
    } else {
      total += quint64(fi.size());
    }
    // Cheap early-out: the caller only ever asks "is this over the cap?".
    if (stop_above > 0 && total > stop_above) return total;
  }
  return total;
}

bool copyTree(const QString& src, const QString& dst, QString* err) {
  QDir().mkpath(dst);
  QDir sd(src);
  const auto entries =
      sd.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
  for (const QFileInfo& fi : entries) {
    const QString to = QDir(dst).filePath(fi.fileName());
    if (fi.isDir()) {
      if (!copyTree(fi.absoluteFilePath(), to, err)) return false;
    } else {
      QFile::remove(to);
      if (!QFile::copy(fi.absoluteFilePath(), to)) {
        if (err) *err = QString("could not copy %1").arg(fi.absoluteFilePath());
        return false;
      }
    }
  }
  return true;
}

// The .lscan the operator most recently touched. QSettings("recentProjects") is
// the library MainWindow maintains; the capture root is the fallback for a run
// that recorded but never opened the project in the Projects workspace.
QString lastSessionLscan() {
  const QStringList recents = QSettings().value("recentProjects").toStringList();
  for (const QString& r : recents) {
    if (!r.isEmpty() && QFileInfo(r).isDir()) return r;
  }
  QString root = QSettings().value("capture/root").toString();
  if (root.isEmpty()) {
    root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
           "/LidarScan Projects";
  }
  QDir rd(root);
  if (!rd.exists()) return QString();
  const auto dirs = rd.entryInfoList(QStringList() << "*.lscan", QDir::Dirs | QDir::NoDotAndDotDot,
                                     QDir::Time);
  return dirs.isEmpty() ? QString() : dirs.front().absoluteFilePath();
}

}  // namespace

// ===========================================================================
// FieldLog
// ===========================================================================

bool FieldLog::open() {
  State* s = state();
  if (s->open_done) return s->fd >= 0;
  s->open_done = true;

  s->run_stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");

  // ~/Library/Logs/LidarScan on macOS. QStandardPaths is the documented
  // fallback for every other case — a sandboxed container, a home directory
  // that is not writable, a CI box with no HOME — and a log that cannot be
  // opened must degrade to "no log", never to a refused launch.
  QStringList candidates;
#if defined(Q_OS_MACOS)
  candidates << QDir::homePath() + "/Library/Logs/LidarScan";
#endif
  candidates << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs"
             << QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/LidarScan-logs";

  for (const QString& c : candidates) {
    if (c.isEmpty()) continue;
    if (!QDir().mkpath(c)) continue;
    // The stamp has second resolution, and two launches inside one second is
    // an ordinary field event (a crash and an immediate relaunch, a launcher
    // that retries). O_APPEND would interleave them into one file that reads
    // like one run with two headers, so the pid disambiguates instead.
    QString p = QDir(c).filePath(QString("lidarscan-%1.log").arg(s->run_stamp));
    if (QFileInfo::exists(p)) {
      s->run_stamp += QString("-%1").arg(QCoreApplication::applicationPid());
      p = QDir(c).filePath(QString("lidarscan-%1.log").arg(s->run_stamp));
    }
    const int fd = openForAppend(p);
    if (fd < 0) continue;
    s->dir = c;
    s->path = p;
    s->fd = fd;
    break;
  }
  if (s->fd < 0) {
    std::fprintf(stderr, "[lidarscan] FIELD LOG UNAVAILABLE — no writable log directory\n");
    return false;
  }
  s->files << s->path;
  s->uptime.start();

  // The crash handler's fd and the two digit slots it patches, resolved HERE
  // rather than in the handler (strchr in a signal handler is legal but
  // pointless when the answer cannot change).
  g_crash_fd = s->fd;
  const char* zeros = std::strstr(g_crash_line, "00");
  if (zeros != nullptr) {
    g_crash_d0 = std::size_t(zeros - g_crash_line);
    g_crash_d1 = g_crash_d0 + 1;
  }
  ::signal(SIGSEGV, fieldLogCrashHandler);
  ::signal(SIGBUS, fieldLogCrashHandler);
  ::signal(SIGABRT, fieldLogCrashHandler);
  ::signal(SIGFPE, fieldLogCrashHandler);

  // --- header -------------------------------------------------------------
  write(Level::kInfo, "field-log", "=== LidarScan field test log ===");
  write(Level::kInfo, "field-log",
        QString("app=%1 name=%2")
            .arg(QCoreApplication::applicationVersion(), QCoreApplication::applicationName()));
  write(Level::kInfo, "field-log",
        QString("engine=%1 abi=%2")
            .arg(QString::fromUtf8(scanengine::engine_version_string()))
            .arg(scanengine::kEngineAbiVersion));
  write(Level::kInfo, "field-log",
        QString("os=%1 kernel=%2 %3 product=%4")
            .arg(QSysInfo::prettyProductName(), QSysInfo::kernelType(),
                 QSysInfo::kernelVersion(), QSysInfo::productVersion()));
  write(Level::kInfo, "field-log",
        QString("machine=%1 cpu_now=%2 cpu_built_for=%3")
            .arg(QSysInfo::machineHostName(), QSysInfo::currentCpuArchitecture(),
                 QSysInfo::buildCpuArchitecture()));
  write(Level::kInfo, "field-log",
        QString("binary=%1 pid=%2 qt=%3")
            .arg(QCoreApplication::applicationFilePath())
            .arg(QCoreApplication::applicationPid())
            .arg(QString::fromUtf8(qVersion())));
  write(Level::kInfo, "field-log", QString("log=%1").arg(s->path));

  // --- sinks --------------------------------------------------------------
  s->prev_qt_handler = qInstallMessageHandler(qtMessageHandler);

  s->engine_stderr_min = scanengine::log_min_level();
  scanengine::set_log_sink(&engineLogSink, nullptr);
  scanengine::set_log_min_level(scanengine::LogLevel::kDebug);
  s->engine_sink_installed = true;
  write(Level::kInfo, "field-log",
        QString("engine log sink installed — file level=debug, stderr level unchanged at %1")
            .arg(QString::fromUtf8(scanengine::to_string(s->engine_stderr_min))));

  std::fprintf(stderr, "[lidarscan] field test log: %s\n", s->path.toUtf8().constData());
  std::fflush(stderr);
  return true;
}

void FieldLog::close() {
  State* s = state();
  if (s->fd < 0) return;

  closeImuDump("app exiting");
  {
    QMutexLocker lock(&s->mutex);
    if (s->m70_fd >= 0) {
      ::fsync(s->m70_fd);
      ::close(s->m70_fd);
      s->m70_fd = -1;
    }
  }
  write(Level::kInfo, "field-log",
        QString("mid70 broadcast dump: %1 datagram(s), %2 — %3")
            .arg(s->m70_datagrams)
            .arg(humanBytes(s->m70_bytes))
            .arg(s->m70_path.isEmpty() ? QString("no file written (none heard)") : s->m70_path));

  // The engine may still log during its own teardown, which happens AFTER
  // main() calls this. Put its sink back to the default before we close the
  // fd, so those lines go to stderr instead of into a closed descriptor.
  if (s->engine_sink_installed) {
    scanengine::set_log_sink(nullptr, nullptr);
    scanengine::set_log_min_level(s->engine_stderr_min);
    s->engine_sink_installed = false;
  }
  qInstallMessageHandler(s->prev_qt_handler);

  const double up = double(s->uptime.elapsed()) / 1000.0;
  write(Level::kInfo, "field-log",
        QString("=== clean exit after %1 s uptime ===").arg(up, 0, 'f', 2));

  QMutexLocker lock(&s->mutex);
  ::fsync(s->fd);
  g_crash_fd = -1;
  ::close(s->fd);
  s->fd = -1;
}

bool FieldLog::isOpen() { return state()->fd >= 0; }
QString FieldLog::path() { return state()->path; }

QString FieldLog::logDir() {
  State* s = state();
  if (!s->dir.isEmpty()) return s->dir;
#if defined(Q_OS_MACOS)
  return QDir::homePath() + "/Library/Logs/LidarScan";
#else
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
#endif
}

QStringList FieldLog::filesThisRun() { return state()->files; }

void FieldLog::write(Level lvl, const char* source, const QString& msg) {
  write(lvl, QString::fromUtf8(source ? source : "?"), msg);
}

void FieldLog::write(Level lvl, const QString& source, const QString& msg) {
  State* s = state();
  if (s->fd < 0) return;
  // ONE LINE PER EVENT, always: an embedded newline would break every grep and
  // every awk-through-the-stats-lines this file exists to serve.
  QString flat = msg;
  flat.replace('\n', "\\n").replace('\r', "\\r");
  const QByteArray line =
      QString("%1 [%2][%3] %4\n").arg(nowStamp(), QString::fromUtf8(levelName(lvl)), source, flat)
          .toUtf8();

  QMutexLocker lock(&s->mutex);
  if (s->fd < 0) return;
  writeAll(s->fd, line.constData(), std::size_t(line.size()));
  // write(2) already put the bytes in the page cache, which survives a killed
  // process. An ERROR line is the one case worth surviving a killed MACHINE
  // too, which is what fsync buys and why it is not paid on every line.
  if (lvl == Level::kError) ::fsync(s->fd);
}

// ---------------------------------------------------------------------------
// Part 2 — the Mid-70 broadcast fixture
// ---------------------------------------------------------------------------

void FieldLog::mid70Datagram(const std::uint8_t* data, std::size_t len, const char* source_ip,
                             std::uint16_t port) {
  State* s = state();
  if (s->fd < 0 || data == nullptr || len == 0) return;
  const qint64 t_ns = epochNs();

  QString opened_path;
  {
    QMutexLocker lock(&s->mutex);
    if (s->m70_fd < 0) {
      // Lazily, on the FIRST datagram: a run that hears nothing (every run on
      // this machine) must not leave a header-only file that looks like a
      // capture of silence.
      const QString p =
          QDir(s->dir).filePath(QString("mid70_broadcast-%1.livoxdump").arg(s->run_stamp));
      const int fd = openForAppend(p);
      if (fd < 0) return;
      // Port table of one entry. capture_mid70.py's broadcast mode writes
      // exactly this: [55000], the port the beacon lands on.
      // The port table is the port the listen actually bound — 55000 for a
      // broadcast pass, which is what capture_mid70.py writes there too.
      const QByteArray h = dumpFileHeader("LX70_CAP", quint32(port));
      if (!writeAll(fd, h.constData(), std::size_t(h.size()))) {
        ::close(fd);
        return;
      }
      s->m70_fd = fd;
      s->m70_path = p;
      s->files << p;
      opened_path = p;
    }
    const QByteArray rh = dumpRecordHeader(t_ns, 0, quint32(len));
    if (writeAll(s->m70_fd, rh.constData(), std::size_t(rh.size())) &&
        writeAll(s->m70_fd, reinterpret_cast<const char*>(data), len)) {
      ++s->m70_datagrams;
      s->m70_bytes += quint64(len);
    }
  }

  if (!opened_path.isEmpty()) {
    write(Level::kInfo, "mid70-dump",
          QString("event=dump_open file=%1 format=LX70_CAP port_table=%2").arg(opened_path).arg(port));
  }
  // Every datagram is logged: at ~1 Hz for the seconds a discovery pass runs
  // this is a handful of lines, and "which datagrams, from where, how big" is
  // the first question anybody asks of a broadcast that did not parse.
  write(Level::kDebug, "mid70-dump",
        QString("event=datagram n=%1 src=%2 port=%3 len=%4 t_ns=%5")
            .arg(s->m70_datagrams)
            .arg(QString::fromUtf8(source_ip ? source_ip : "?"))
            .arg(port)
            .arg(len)
            .arg(t_ns));
}

std::uint64_t FieldLog::mid70Datagrams() { return state()->m70_datagrams; }

// ---------------------------------------------------------------------------
// Part 2 — the serial IMU fixture
// ---------------------------------------------------------------------------

void FieldLog::openImuDump(const QString& port_name, int baud) {
  State* s = state();
  if (s->fd < 0) return;
  closeImuDump("re-opening for a new port");

  QString p;
  {
    QMutexLocker lock(&s->mutex);
    ++s->imu_open_count;
    p = QDir(s->dir).filePath(s->imu_open_count <= 1
                                  ? QString("imu_serial-%1.imudump").arg(s->run_stamp)
                                  : QString("imu_serial-%1-%2.imudump")
                                        .arg(s->run_stamp)
                                        .arg(s->imu_open_count));
    const int fd = openForAppend(p);
    if (fd < 0) return;
    // IMUSRCAP's "port table" is the BAUD RATE — the format has no port number
    // (capture_serial_imu.py's docstring says so outright).
    const QByteArray h = dumpFileHeader("IMUSRCAP", quint32(baud));
    if (!writeAll(fd, h.constData(), std::size_t(h.size()))) {
      ::close(fd);
      return;
    }
    s->imu_fd = fd;
    s->imu_path = p;
    s->imu_port = port_name;
    s->imu_baud = baud;
    s->imu_records = 0;
    s->imu_bytes = 0;
    s->imu_first_ns = 0;
    s->files << p;
  }
  write(Level::kInfo, "imu-dump",
        QString("event=dump_open file=%1 format=IMUSRCAP port=%2 baud=%3").arg(p, port_name).arg(baud));
}

void FieldLog::imuBytes(const char* data, std::size_t len) {
  State* s = state();
  if (s->imu_fd < 0 || data == nullptr || len == 0) return;
  const qint64 t_ns = epochNs();

  bool first = false;
  {
    QMutexLocker lock(&s->mutex);
    if (s->imu_fd < 0) return;
    const QByteArray rh = dumpRecordHeader(t_ns, 0, quint32(len));
    if (!writeAll(s->imu_fd, rh.constData(), std::size_t(rh.size()))) return;
    if (!writeAll(s->imu_fd, data, len)) return;
    ++s->imu_records;
    s->imu_bytes += quint64(len);
    if (s->imu_first_ns == 0) {
      s->imu_first_ns = t_ns;
      first = true;
    }
  }
  // ONE line for the first slice and nothing after it. At 100 Hz a per-slice
  // line would be 100 lines a second of "bytes arrived", which would bury
  // everything else in this file inside a minute. The counts are reported on
  // close and in every 2 s stats snapshot instead.
  if (first) {
    write(Level::kInfo, "imu-dump",
          QString("event=first_bytes port=%1 len=%2 t_ns=%3").arg(s->imu_port).arg(len).arg(t_ns));
  }
}

void FieldLog::closeImuDump(const QString& why) {
  State* s = state();
  QString p;
  quint64 records = 0, bytes = 0;
  qint64 first = 0;
  {
    QMutexLocker lock(&s->mutex);
    if (s->imu_fd < 0) return;
    ::fsync(s->imu_fd);
    ::close(s->imu_fd);
    s->imu_fd = -1;
    p = s->imu_path;
    records = s->imu_records;
    bytes = s->imu_bytes;
    first = s->imu_first_ns;
  }
  write(Level::kInfo, "imu-dump",
        QString("event=dump_close file=%1 why=%2 records=%3 bytes=%4 first_bytes_t_ns=%5")
            .arg(p, why)
            .arg(records)
            .arg(bytes)
            .arg(first));
}

// ---------------------------------------------------------------------------
// Part 3 — the diagnostics bundle
// ---------------------------------------------------------------------------

QString FieldLog::saveDiagnosticsBundle(bool reveal, QString* err) {
  const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
  QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  if (desktop.isEmpty()) desktop = QDir::homePath();
  const QString out = QDir(desktop).filePath(QString("LidarScan-diagnostics-%1").arg(ts));
  if (!QDir().mkpath(out)) {
    if (err) *err = QString("could not create %1").arg(out);
    return QString();
  }
  info("diagnostics", QString("event=bundle_start dir=%1").arg(out));

  QStringList notes;

  // --- 1. this run's log + fixtures ---------------------------------------
  int copied = 0;
  for (const QString& f : filesThisRun()) {
    const QFileInfo fi(f);
    if (!fi.exists()) continue;
    const QString to = QDir(out).filePath(fi.fileName());
    QFile::remove(to);
    if (QFile::copy(f, to)) ++copied;
  }
  notes << QString("%1 file(s) from this run").arg(copied);

  // --- 1b. recent PREVIOUS runs -------------------------------------------
  //
  // A crash is the case this bundle exists for, and after a crash "this run" is
  // the relaunch — the file that matters was written by the run that died. So
  // anything in the log directory touched in the last day comes too, in its own
  // folder so it is never confused with the current run's evidence.
  {
    // ONE call, into a named local. `QSet(filesThisRun().begin(),
    // filesThisRun().end())` looks equivalent and is not: filesThisRun()
    // returns by value, so the two iterators would point into two different
    // temporaries. That crashed here, and the crash hook above is what said so.
    const QStringList this_run = filesThisRun();
    const QSet<QString> mine(this_run.begin(), this_run.end());
    QDir ld(logDir());
    int prev = 0;
    const auto entries = ld.entryInfoList(QDir::Files, QDir::Time);
    for (const QFileInfo& fi : entries) {
      if (mine.contains(fi.absoluteFilePath())) continue;
      if (fi.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 3600) continue;
      const QString sub = QDir(out).filePath("previous-runs");
      QDir().mkpath(sub);
      if (QFile::copy(fi.absoluteFilePath(), QDir(sub).filePath(fi.fileName()))) ++prev;
    }
    if (prev > 0) notes << QString("%1 file(s) from earlier runs in the last 24 h").arg(prev);
  }

  // --- 2. the last .lscan --------------------------------------------------
  const QString lscan = lastSessionLscan();
  if (lscan.isEmpty()) {
    notes << "no .lscan project found to include";
  } else {
    constexpr quint64 kCap = 1024ULL * 1024ULL * 1024ULL;  // 1 GB
    const quint64 sz = dirSizeBytes(lscan, kCap);
    const QString name = QFileInfo(lscan).fileName();
    if (sz <= kCap) {
      QString cerr;
      if (copyTree(lscan, QDir(out).filePath(name), &cerr)) {
        notes << QString("%1 copied whole (%2)").arg(name, humanBytes(sz));
      } else {
        notes << QString("%1 could not be copied: %2").arg(name, cerr);
      }
    } else {
      // Too big to mail. The manifest plus a size listing is what actually
      // answers "did it record, and what streams" — which is the question,
      // not "please send me 4 GB of points".
      const QString sub = QDir(out).filePath(name + " (manifest only)");
      QDir().mkpath(sub);
      QFile::copy(QDir(lscan).filePath("manifest.json"), QDir(sub).filePath("manifest.json"));
      QString sizes = QString("%1\nTotal: %2 — TOO LARGE to include (cap 1 GB).\n"
                              "Only manifest.json was copied. Stream sizes:\n\n")
                          .arg(lscan, humanBytes(sz));
      QDir sd(QDir(lscan).filePath("streams"));
      const auto files = sd.entryInfoList(QDir::Files, QDir::Name);
      for (const QFileInfo& fi : files) {
        sizes += QString("  streams/%1  %2 (%3 bytes)\n")
                     .arg(fi.fileName(), humanBytes(quint64(fi.size())))
                     .arg(fi.size());
      }
      if (files.isEmpty()) sizes += "  (no streams/ files)\n";
      QFile sf(QDir(sub).filePath("SIZES.txt"));
      if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) sf.write(sizes.toUtf8());
      notes << QString("%1 was %2 — manifest.json + SIZES.txt only").arg(name, humanBytes(sz));
    }
  }

  // --- 3. system.txt -------------------------------------------------------
  {
    constexpr int kT = 20000;  // 20 s per command, the task's budget
    QString sys;
    sys += "=== sw_vers ===\n" + runCommand("/usr/bin/sw_vers", {}, kT);
    sys += "\n=== uname -a ===\n" + runCommand("/usr/bin/uname", {"-a"}, kT);
    sys += "\n=== ifconfig ===\n" + runCommand("/sbin/ifconfig", {}, kT);
    // A glob needs a shell; /dev/cu.* is also the ONE listing that says whether
    // the USB-serial adapter is even enumerated.
    sys += "\n=== ls -l /dev/cu.* ===\n" +
           runCommand("/bin/sh", {"-c", "ls -l /dev/cu.* 2>&1"}, kT);
    sys += "\n=== system_profiler SPUSBDataType SPNetworkDataType ===\n" +
           runCommand("/usr/sbin/system_profiler", {"SPUSBDataType", "SPNetworkDataType"}, kT);
    QFile f(QDir(out).filePath("system.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(sys.toUtf8());
  }

  // --- 4. README.txt -------------------------------------------------------
  {
    QString readme;
    readme += "LidarScan field-test diagnostics\n";
    readme += "================================\n\n";
    readme += QString("Collected %1\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    readme += QString("App      : %1\n").arg(QCoreApplication::applicationVersion());
    readme += QString("Engine   : %1 (ABI %2)\n")
                  .arg(QString::fromUtf8(scanengine::engine_version_string()))
                  .arg(scanengine::kEngineAbiVersion);
    readme += QString("Machine  : %1, %2, %3\n\n")
                  .arg(QSysInfo::machineHostName(), QSysInfo::prettyProductName(),
                       QSysInfo::currentCpuArchitecture());
    readme +=
        "WHAT TO SEND\n"
        "------------\n"
        "Zip this whole folder (right-click -> Compress) and send the .zip.\n"
        "Nothing here is collected in the background: the folder was created\n"
        "the moment you asked for it, and it contains only what is listed\n"
        "below.\n\n"
        "WHAT IS IN IT\n"
        "-------------\n"
        "  lidarscan-<time>.log        Everything the app and the engine said\n"
        "                              during this run, one line per event,\n"
        "                              including a stats snapshot every 2 s\n"
        "                              for each armed device.\n"
        "  mid70_broadcast-*.livoxdump Raw Livox SDK v1 broadcast datagrams,\n"
        "                              exactly as tools/remote-capture/\n"
        "                              capture_mid70.py records them. Present\n"
        "                              only if a Mid-70 was heard.\n"
        "  imu_serial-*.imudump        Raw bytes from the serial IMU module,\n"
        "                              exactly as capture_serial_imu.py records\n"
        "                              them. Present only if the port opened.\n"
        "  <name>.lscan/               The last scan, whole if it is under 1 GB;\n"
        "                              otherwise just its manifest.json plus a\n"
        "                              SIZES.txt listing what was left out.\n"
        "  previous-runs/              Logs and dumps from earlier runs in the\n"
        "                              last 24 hours - this is where the file\n"
        "                              from a run that CRASHED will be.\n"
        "  system.txt                  sw_vers, uname, ifconfig, /dev/cu.*, and\n"
        "                              the USB + network sections of\n"
        "                              system_profiler.\n\n"
        "PRIVACY\n"
        "-------\n"
        "system.txt lists this Mac's network interfaces and IP addresses and\n"
        "the USB devices attached to it, because a lidar that will not connect\n"
        "is almost always a subnet or a cable. Read it before you send it if\n"
        "that matters to you; deleting it still leaves a useful bundle.\n\n"
        "CONTENTS OF THIS BUNDLE\n"
        "-----------------------\n";
    for (const QString& n : notes) readme += "  * " + n + "\n";
    QFile f(QDir(out).filePath("README.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) f.write(readme.toUtf8());
  }

  info("diagnostics",
       QString("event=bundle_done dir=%1 notes=%2").arg(out, notes.join("; ")));
  if (reveal) QDesktopServices::openUrl(QUrl::fromLocalFile(out));
  return out;
}

// ===========================================================================
// FieldLogStats — the 2 s snapshot
// ===========================================================================

namespace {

void kv(QStringList& out, const char* key, const QString& v) {
  out << QString("%1=%2").arg(QString::fromUtf8(key), v.isEmpty() ? QStringLiteral("-") : v);
}
void kv(QStringList& out, const char* key, quint64 v) {
  out << QString("%1=%2").arg(QString::fromUtf8(key)).arg(v);
}
void kv(QStringList& out, const char* key, qint64 v) {
  out << QString("%1=%2").arg(QString::fromUtf8(key)).arg(v);
}
void kv(QStringList& out, const char* key, int v) {
  out << QString("%1=%2").arg(QString::fromUtf8(key)).arg(v);
}
void kv(QStringList& out, const char* key, bool v) {
  out << QString("%1=%2").arg(QString::fromUtf8(key), v ? QStringLiteral("1") : QStringLiteral("0"));
}
void kvf(QStringList& out, const char* key, double v, int prec = 3) {
  out << QString("%1=%2").arg(QString::fromUtf8(key), QString::number(v, 'f', prec));
}

void appendMid70(QStringList& f, const scanengine::Mid70Stats& m) {
  kv(f, "m70_link", QString::fromUtf8(scanengine::to_string(m.link)));
  kv(f, "m70_state", QString::fromUtf8(scanengine::to_string(m.state)));
  kvf(f, "m70_points_per_sec", m.points_per_sec, 1);
  kvf(f, "m70_points_appended_per_sec", m.points_appended_per_sec, 1);
  kvf(f, "m70_loss_pct_window", m.loss_pct_window, 3);
  kvf(f, "m70_loss_pct_total", m.loss_pct_total, 3);
  kv(f, "m70_packets_lost", quint64(m.packets_lost));
  kv(f, "m70_packets_duplicated", quint64(m.packets_duplicated));
  kv(f, "m70_counter_resets", quint64(m.counter_resets));
  kv(f, "m70_point_packets", quint64(m.point_packets));
  kv(f, "m70_points_received", quint64(m.points_received));
  kv(f, "m70_points_kept", quint64(m.points_kept));
  kv(f, "m70_points_appended", quint64(m.points_appended));
  kv(f, "m70_points_dropped_store", quint64(m.points_dropped_store));
  kv(f, "m70_bad_packets", quint64(m.bad_packets));
  kv(f, "m70_unexpected_imu_packets", quint64(m.unexpected_imu_packets));
  kv(f, "m70_filter_seen", quint64(m.filter.seen));
  kv(f, "m70_filter_kept", quint64(m.filter.kept));
  kv(f, "m70_filter_dropped_no_return", quint64(m.filter.dropped_no_return));
  kv(f, "m70_filter_dropped_tag", quint64(m.filter.dropped_tag));
  kv(f, "m70_filter_dropped_range", quint64(m.filter.dropped_range));
  kv(f, "m70_filter_dropped_reflectivity", quint64(m.filter.dropped_reflectivity));
  kv(f, "m70_watchdog_trips", quint64(m.watchdog_trips));
  kv(f, "m70_clean_resumes", quint64(m.clean_resumes));
  kv(f, "m70_forced_reinits", quint64(m.forced_reinits));
  kv(f, "m70_reinit_failures", quint64(m.reinit_failures));
  kv(f, "m70_t_last_point_ns", qint64(m.t_last_point_ns));
  kv(f, "m70_t_silent_since_ns", qint64(m.t_silent_since_ns));
  kv(f, "m70_t_device_last_ns", qint64(m.t_device_last_ns));
  kv(f, "m70_device_stamp_decodable", m.device_stamp_decodable);
  kv(f, "m70_data_type", int(m.data_type));
  kv(f, "m70_timestamp_type", int(m.timestamp_type));
  kv(f, "m70_timestamp_type_name",
     QString::fromUtf8(scanengine::mid70::to_string_timestamp_type(m.timestamp_type)));
  kv(f, "m70_err_code_raw", quint64(m.err_code_raw));
  // Every err.* field, always — including the normal ones. A health ROW hides
  // "normal" so an operator keeps reading it (CaptureWindow::mid70HealthText);
  // a LOG must print it, because "temp_status was 0 the whole time" is an
  // answer and a missing key is not.
  kv(f, "m70_err_pps_ok", m.err.pps_ok);
  kv(f, "m70_err_ptp_ok", m.err.ptp_ok);
  kv(f, "m70_err_time_sync_status", int(m.err.time_sync_status));
  kv(f, "m70_err_time_sync_name",
     QString::fromUtf8(scanengine::mid70::to_string_time_sync_status(m.err.time_sync_status)));
  kv(f, "m70_err_self_heating", m.err.self_heating);
  kv(f, "m70_err_temp_status", int(m.err.temp_status));
  kv(f, "m70_err_volt_status", int(m.err.volt_status));
  kv(f, "m70_err_motor_status", int(m.err.motor_status));
  kv(f, "m70_err_dirty_warn", int(m.err.dirty_warn));
  kv(f, "m70_err_firmware_err", m.err.firmware_err);
  kv(f, "m70_err_fan_warn", m.err.fan_warn);
  kv(f, "m70_err_device_lifetime_warn", m.err.device_lifetime_warn);
  kv(f, "m70_err_system_status", int(m.err.system_status));
  kv(f, "m70_broadcast_code", QString::fromStdString(m.broadcast_code));
  kv(f, "m70_device_ip", QString::fromStdString(m.device_ip));
  kv(f, "m70_firmware", QString::fromStdString(m.firmware));
}

void appendImuSerial(QStringList& f, const scanengine::ImuSerialStats& s) {
  kv(f, "imu_state", QString::fromUtf8(scanengine::to_string(s.state)));
  kvf(f, "imu_rate_hz", s.rate_hz, 2);
  kv(f, "imu_samples", quint64(s.samples));
  kv(f, "imu_samples_dropped", quint64(s.samples_dropped));
  kv(f, "imu_blackouts", quint64(s.blackouts));
  kv(f, "imu_blackout_in_progress", s.blackout_in_progress);
  kv(f, "imu_worst_blackout_ns", qint64(s.worst_blackout_ns));
  // Both units on purpose. The ns figure is what the driver measured; the
  // seconds figure is the key FIELD_TEST_MID70.md's reference table tells the
  // owner to grep for ("worst_blackout_s="), and a doc that names a key the log
  // does not write is worse than no doc.
  kvf(f, "imu_worst_blackout_s", double(s.worst_blackout_ns) / 1e9, 3);
  kv(f, "imu_t_last_sample_ns", qint64(s.t_last_sample_ns));
  kv(f, "imu_t_last_bytes_ns", qint64(s.t_last_bytes_ns));
  kv(f, "imu_frames_bytes_in", quint64(s.frames.bytes_in));
  kv(f, "imu_frames_raw", quint64(s.frames.raw_frames));
  kv(f, "imu_frames_quaternion", quint64(s.frames.quaternion_frames));
  kv(f, "imu_frames_euler", quint64(s.frames.euler_frames));
  kv(f, "imu_frames_barometer", quint64(s.frames.barometer_frames));
  kv(f, "imu_frames_version", quint64(s.frames.version_frames));
  kv(f, "imu_frames_state", quint64(s.frames.state_frames));
  kv(f, "imu_frames_unknown", quint64(s.frames.unknown_frames));
  kv(f, "imu_frames_checksum_failures", quint64(s.frames.checksum_failures));
  kv(f, "imu_frames_resyncs", quint64(s.frames.resyncs));
  kv(f, "imu_frames_ok", quint64(s.frames.frames_ok()));
  kv(f, "imu_frames_seen", quint64(s.frames.frames_seen()));
  kvf(f, "imu_checksum_pass_rate", s.frames.checksum_pass_rate(), 4);
  kv(f, "imu_stamper_samples", quint64(s.stamper.samples));
  kv(f, "imu_stamper_gap_snaps", quint64(s.stamper.gap_snaps));
  kv(f, "imu_stamper_worst_gap_ns", qint64(s.stamper.worst_gap_ns));
  kv(f, "imu_stamper_clock_step_backs", quint64(s.stamper.clock_step_backs));
  kv(f, "imu_stamper_worst_step_back_ns", qint64(s.stamper.worst_step_back_ns));
  kv(f, "imu_stamper_deep_bursts", quint64(s.stamper.deep_bursts));
  kv(f, "imu_stamper_clamps", quint64(s.stamper.clamps));
  kv(f, "imu_stamper_learned_period_ns", qint64(s.stamper.learned_period_ns));
  kv(f, "imu_stamper_resyncing", s.stamper.resyncing);
}

}  // namespace

FieldLogStats::FieldLogStats(EngineHost* host) : host_(host) {
  timer_.setTimerType(Qt::CoarseTimer);
  QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] { snapshotNow(); });
}

void FieldLogStats::start(int interval_ms) {
  if (!FieldLog::isOpen()) return;
  timer_.start(interval_ms);
  FieldLog::info("stats", QString("event=snapshot_pump_started interval_ms=%1").arg(interval_ms));
}

void FieldLogStats::stop() { timer_.stop(); }

void FieldLogStats::snapshotNow() {
  if (host_ == nullptr || !host_->ok() || !FieldLog::isOpen()) return;
  scanengine::Engine* e = host_->engine();
  if (e == nullptr) return;

  const QVector<DeviceRow> rows = host_->devices();
  if (rows.isEmpty()) {
    if (was_armed_) {
      was_armed_ = false;
      FieldLog::info("stats", "event=no_devices — snapshots paused until something is armed");
    }
    return;
  }
  was_armed_ = true;

  for (const DeviceRow& r : rows) {
    QStringList f;
    kv(f, "dev", quint64(r.id));
    kv(f, "kind", QString::fromUtf8(scanengine::to_string(r.kind)));
    kv(f, "state", QString::fromUtf8(scanengine::to_string(r.health.state)));
    kv(f, "last_error", QString::fromUtf8(scanengine::error_str(r.health.last_error)));
    kv(f, "points_out", quint64(r.health.points_out));
    kv(f, "packets_ok", quint64(r.health.packets_ok));
    kv(f, "packets_bad", quint64(r.health.packets_bad));
    kvf(f, "checksum_rate", r.health.checksum_pass_rate, 4);
    kv(f, "drops", quint64(r.health.drops));
    kv(f, "bytes_in", quint64(r.health.bytes_in));
    kvf(f, "points_per_sec", r.health.points_per_sec, 1);
    kvf(f, "rotation_hz", r.health.rotation_hz, 2);
    kv(f, "t_last_data_ns", qint64(r.health.t_last_data_ns));

    if (r.kind == scanengine::DeviceKind::kMid70) {
      const auto m = e->mid70_stats(r.id);
      if (m.ok()) {
        appendMid70(f, m.value());
      } else {
        kv(f, "m70_stats_error", QString::fromUtf8(scanengine::error_str(m.error())));
      }
    } else if (r.kind == scanengine::DeviceKind::kImuSerial) {
      const auto s = e->imu_serial_stats(r.id);
      if (s.ok()) {
        appendImuSerial(f, s.value());
      } else {
        kv(f, "imu_stats_error", QString::fromUtf8(scanengine::error_str(s.error())));
      }
    }
    FieldLog::write(FieldLog::Level::kInfo, "stats", "event=device " + f.join(' '));
  }

  // The page store, once per snapshot: field bug D (§19.1) was a store that
  // filled and silently refused every point after it, and `evicting` plus
  // `dropped_points` are the two numbers that tell those apart.
  {
    const scanengine::PageStoreStats ps = host_->pageStats();
    QStringList f;
    kv(f, "pages", quint64(ps.pages));
    kv(f, "max_pages", quint64(ps.max_pages));
    kv(f, "resident_points", quint64(ps.resident_points));
    kv(f, "total_points", quint64(ps.total_points));
    kv(f, "dropped_points", quint64(ps.dropped_points));
    kv(f, "evicted_pages", quint64(ps.evicted_pages));
    kv(f, "evicted_points", quint64(ps.evicted_points));
    kv(f, "evicting", ps.evicting);
    FieldLog::write(FieldLog::Level::kInfo, "stats", "event=store " + f.join(' '));
  }

  // Live SLAM, when the session started one. Same accessor CaptureWindow's LIO
  // status line reads (Engine::live_slam()), so a disagreement between the log
  // and the screen is impossible by construction.
  if (scanengine::LioOdometry* lio = e->live_slam()) {
    const scanengine::LioStats l = lio->stats();
    QStringList f;
    kv(f, "initialized", l.initialized);
    kv(f, "diverged", l.diverged);
    kv(f, "imu_samples", quint64(l.imu_samples));
    kv(f, "imu_gaps", quint64(l.imu_gaps));
    kv(f, "points_in", quint64(l.points_in));
    kv(f, "points_kept", quint64(l.points_kept));
    kv(f, "points_mapped", quint64(l.points_mapped));
    kv(f, "points_late", quint64(l.points_late));
    kv(f, "store_appends_failed", quint64(l.store_appends_failed));
    kv(f, "scans", quint64(l.scans));
    kv(f, "scans_skipped", quint64(l.scans_skipped));
    kv(f, "residuals_last", quint64(l.residuals_last));
    kv(f, "iterations_last", quint64(l.iterations_last));
    kv(f, "residuals_total", quint64(l.residuals_total));
    kv(f, "iterations_total", quint64(l.iterations_total));
    kvf(f, "residual_rms_m", l.residual_rms_m, 4);
    kvf(f, "scan_ms_last", l.scan_ms_last, 2);
    kvf(f, "scan_ms_mean", l.scan_ms_mean, 2);
    kvf(f, "scan_ms_p50", l.scan_ms_p50, 2);
    kvf(f, "scan_ms_p95", l.scan_ms_p95, 2);
    kvf(f, "scan_ms_max", l.scan_ms_max, 2);
    kvf(f, "cpu_budget_used", l.cpu_budget_used, 3);
    kv(f, "map_voxels", quint64(l.map_voxels));
    kv(f, "map_points", quint64(l.map_points));
    kvf(f, "trajectory_length_m", l.trajectory_length_m, 3);
    kvf(f, "gravity_m_s2", l.gravity_m_s2, 4);
    scanengine::Pose latest{};
    if (lio->poses().latest(&latest)) {
      kv(f, "pose_t_mono_ns", qint64(latest.t_mono_ns));
      kvf(f, "pose_x", latest.position[0], 4);
      kvf(f, "pose_y", latest.position[1], 4);
      kvf(f, "pose_z", latest.position[2], 4);
      kv(f, "pose_tracking_lost", int(latest.tracking_lost));
    } else {
      kv(f, "pose", QString("none"));
    }
    FieldLog::write(FieldLog::Level::kInfo, "stats", "event=live_slam " + f.join(' '));
  }
}

}  // namespace lidarscan
