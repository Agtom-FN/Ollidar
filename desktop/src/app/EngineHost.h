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

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <map>
#include <memory>
#include <string>

#include "scanengine/core/engine.h"

namespace lidarscan {

class SerialReader;

struct DeviceRow {
  scanengine::DeviceId id = scanengine::kInvalidDeviceId;
  scanengine::DeviceKind kind = scanengine::DeviceKind::kUnknown;
  scanengine::DeviceHealth health{};
};

class EngineHost : public QObject {
  Q_OBJECT
 public:
  // `live_max_pages` overrides the engine's PageStore ceiling (0 = the engine
  // default, 64 x 1 M points). It exists for one reason: --live-map-soak has to
  // reach the ceiling in seconds instead of the five minutes a real Mid-360
  // needs, and it must reach it through the SHIPPED store, not a mock.
  explicit EngineHost(QObject* parent = nullptr, quint32 live_max_pages = 0,
                      quint32 live_page_points = 0);
  ~EngineHost() override;

  bool ok() const { return engine_ != nullptr; }
  const QString& createError() const { return create_error_; }

  scanengine::Engine* engine() { return engine_.get(); }
  const scanengine::PageStore* points() const;

  // The live point window (engine ABI 7). OFF by default here, because this
  // Engine's PageStore is also what a replay, a merge preview and a loaded
  // result render out of — those must keep the hard cap and say when they
  // overran. CaptureWindow turns it ON while a device is armed and OFF again
  // when it disarms, which is the only period during which the store is a
  // live capture's moving window.
  bool setLivePageEviction(bool enabled);
  scanengine::PageStoreStats pageStats() const;

  QString versionString() const;

  // `live_slam` starts one LioOdometry for the session's Mid-360 stream
  // (SessionConfig::live_slam): its registered map is published on
  // StreamId::kSlamMap through this Engine's PageStore and its trajectory is
  // Engine::live_slam()->poses(). Round-5 item 18 (walkthrough-first) needs both
  // — a walked scan has to be registered as it goes, and the live trail is drawn
  // from those poses. Default false keeps every other caller (replay, C4/C5/C6
  // fixtures) on the Record-only path they were verified with.
  //
  // `lio_min_range_m` overrides SessionConfig::lio.min_range_m when > 0. It
  // exists for the Mid-70 (A17) and nothing else: LioConfig defaults to a 0.5 m
  // near gate sized for the Mid-360, and the Mid-70's blind zone is 0.05 m —
  // in a tight room a 0.5 m gate throws away most of what the sensor returned,
  // which the ROS work measured before this port. 0 (the default) leaves the
  // engine's own value alone, so every existing caller is byte-for-byte
  // unchanged.
  bool startSession(const QString& lscan_dir, const QString& profile, bool record, QString* err,
                    bool live_slam = false, float lio_min_range_m = 0.0f);
  bool stopSession(QString* err);
  bool sessionActive() const;
  QString sessionDir() const { return session_dir_; }

  // Adds a device and returns its id (kInvalidDeviceId on failure).
  scanengine::DeviceId addD6(const scanengine::D6Config& cfg, QString* err);
  scanengine::DeviceId addMid360(const scanengine::Mid360Config& cfg, QString* err);
  // A17. Same shape as addMid360 — the Mid-70 is a UDP lidar with its own
  // config block on DeviceConfig, and nothing about the seam differs.
  scanengine::DeviceId addMid70(const scanengine::Mid70Config& cfg, QString* err);
  // A18. NOT the same shape, and the difference is the whole reason this
  // overload exists: an ImuSerialConfig carries a UsbSerialConfig, which
  // carries `write_fn` (a C function pointer + void*) and `port_name` (a bare
  // const char*). The driver copies the config by value and keeps both for its
  // whole life, so:
  //
  //   * the WRITE BRIDGE — a static trampoline whose user_data is a struct
  //     holding `reader` — must be installed BEFORE add_device(), because
  //     Engine starts a device added mid-session immediately and
  //     ImuSerialDriver::start() is what sends the 100 Hz report-rate frame; a
  //     write_fn installed afterwards would arrive one command too late; and
  //   * the port-name STRING must outlive the driver, so this owns a copy and
  //     repoints cfg.serial.port_name at it. A caller's QByteArray::constData()
  //     would dangle the moment the caller's stack frame ended.
  //
  // Both live in a bridge record owned by this object and released by
  // removeDevice() AFTER the engine has torn the driver down. `reader` must
  // outlive the device; CaptureWindow owns it and closes it on disarm.
  scanengine::DeviceId addImuSerial(const scanengine::ImuSerialConfig& cfg, SerialReader* reader,
                                    QString* err);
  bool removeDevice(scanengine::DeviceId id, QString* err);

  // App -> engine bytes, for every push-mode serial driver (D6, STL-27L, and
  // the A18 IMU). `t_mono_ns` is an engineNowNs() stamp taken by the reader at
  // arrival — NOT zero, which would make the engine stamp it later, after the
  // GUI thread has done whatever else it was doing. Returns false and fills
  // `err` on rejection; the caller decides how loudly to say so.
  bool pushBytes(scanengine::DeviceId id, const QByteArray& bytes, qint64 t_mono_ns,
                 QString* err = nullptr);

  // The engine's own monotonic clock (timesync/clock.h SteadyClock), so an
  // arrival stamped by the app lands on the same timeline as one stamped
  // inside the engine. Static: SerialReader needs it without holding a host.
  static qint64 engineNowNs();

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

  // What addImuSerial() installs as UsbSerialConfig::write_user_data. One per
  // serial device, kept alive by this object for exactly as long as the engine
  // holds the driver that points at it. See addImuSerial()'s comment.
  struct SerialWriteBridge {
    SerialReader* reader = nullptr;
    std::string port_name;  // backs UsbSerialConfig::port_name, a const char*
  };
  static scanengine::ScanError serialWriteTrampoline(const std::uint8_t* data, std::size_t len,
                                                     void* user_data);

  // DECLARED BEFORE engine_ ON PURPOSE. Members are destroyed in reverse
  // declaration order, so this outlives the Engine — and therefore outlives
  // every Driver the Engine owns, each of which may hold a write_fn whose
  // user_data points in here. The other order would leave a driver's teardown
  // dereferencing a freed bridge, and "the current driver happens not to write
  // on stop" is a property of one driver today, not a guarantee.
  std::map<scanengine::DeviceId, std::unique_ptr<SerialWriteBridge>> serial_bridges_;
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
