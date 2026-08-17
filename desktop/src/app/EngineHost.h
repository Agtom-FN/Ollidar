// EngineHost.h — the desktop app's single owner of scanengine::Engine.
//
// Tech Spec §3 key rule 1: "the Qt app links the engine's C++ API directly
// (same process, no FFI)". This class is that linkage and nothing else — it
// creates the Engine, pumps its event bus on the GUI thread, polls device
// health, and exposes the PageStore the viewport mirrors. It contains no
// decode, no parsing and no device I/O: per DESIGN.md §2 the app owns the
// platform serial/socket code and the engine owns everything after the bytes.
//
// EVENT PUMP
//   The engine offers queued or callback delivery. Callback mode runs inline on
//   the publishing thread with the bus lock held and must not re-enter the
//   engine — useless for UI. So this uses a QUEUED subscription drained from a
//   QTimer on the GUI thread, which is exactly what event_bus.h says Qt should
//   do. kEventsDropped is surfaced rather than swallowed; the viewport does not
//   care (it re-reads pages every frame) but the log does.
//
// RECORDER FLUSH
//   A5 documents an unbounded data-loss window if input stalls with a partial
//   buffer, and names the fix: "the Android/Qt capture UIs already poll engine
//   state on a timer for other reasons; hooking flush() to the same cadence is
//   the intended integration". That is done here, once a second, while a
//   session is recording.
//
// Owner: C1.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <memory>

#include "scanengine/core/engine.h"

namespace lidarscan {

struct DeviceRow {
  scanengine::DeviceId id = scanengine::kInvalidDeviceId;
  scanengine::DeviceKind kind = scanengine::DeviceKind::kUnknown;
  scanengine::DeviceHealth health{};
};

class EngineHost : public QObject {
  Q_OBJECT
 public:
  explicit EngineHost(QObject* parent = nullptr);
  ~EngineHost() override;

  bool ok() const { return engine_ != nullptr; }
  const QString& createError() const { return create_error_; }

  scanengine::Engine* engine() { return engine_.get(); }
  const scanengine::PageStore* points() const;

  QString versionString() const;

  // `live_slam` starts one LioOdometry for the session's Mid-360 stream
  // (SessionConfig::live_slam): its registered map is published on
  // StreamId::kSlamMap through this Engine's PageStore and its trajectory is
  // Engine::live_slam()->poses(). Round-5 item 18 (walkthrough-first) needs both
  // — a walked scan has to be registered as it goes, and the live trail is drawn
  // from those poses. Default false keeps every other caller (replay, C4/C5/C6
  // fixtures) on the Record-only path they were verified with.
  bool startSession(const QString& lscan_dir, const QString& profile, bool record, QString* err,
                    bool live_slam = false);
  bool stopSession(QString* err);
  bool sessionActive() const;
  QString sessionDir() const { return session_dir_; }

  // Adds a device and returns its id (kInvalidDeviceId on failure).
  scanengine::DeviceId addD6(const scanengine::D6Config& cfg, QString* err);
  scanengine::DeviceId addMid360(const scanengine::Mid360Config& cfg, QString* err);
  bool removeDevice(scanengine::DeviceId id, QString* err);

  QVector<DeviceRow> devices() const;

  // One line for the status bar: engine state, devices, points, drops.
  QString healthLine() const;

 Q_SIGNALS:
  void logLine(const QString& line);
  void devicesChanged();
  void sessionChanged();
  void tick();  // once per pump, after events are drained

 private:
  void pump();

  std::unique_ptr<scanengine::Engine> engine_;
  QString create_error_;
  scanengine::SubscriptionId sub_ = 0;
  QTimer pump_timer_;
  QString session_dir_;
  qint64 last_flush_ms_ = 0;
  quint64 events_seen_ = 0;
  quint64 events_dropped_ = 0;
};

}  // namespace lidarscan
