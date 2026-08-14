#include "app/EngineHost.h"

#include <QDateTime>

#include "scanengine/core/event.h"

namespace lidarscan {
namespace {

QString describeEvent(const scanengine::Event& ev) {
  using scanengine::EventType;
  switch (ev.type) {
    case EventType::kEventsDropped:
      return QString("events dropped: %1 (total %2)")
          .arg(ev.payload.dropped.count)
          .arg(ev.payload.dropped.total);
    case EventType::kEngineState:
      return QString("engine %1 -> %2")
          .arg(scanengine::to_string(ev.payload.engine_state.previous))
          .arg(scanengine::to_string(ev.payload.engine_state.state));
    case EventType::kSessionState:
      return QString("session %1 recording=%2 bytes=%3")
          .arg(ev.payload.session.session_id)
          .arg(ev.payload.session.recording ? "yes" : "no")
          .arg(ev.payload.session.bytes_written);
    case EventType::kDeviceState:
      return QString("device %1 (%2) %3 -> %4%5")
          .arg(ev.payload.device.device)
          .arg(scanengine::to_string(ev.payload.device.kind))
          .arg(scanengine::to_string(ev.payload.device.previous))
          .arg(scanengine::to_string(ev.payload.device.state))
          .arg(ev.payload.device.error == scanengine::ScanError::kOk
                   ? QString()
                   : QString(" [%1]").arg(scanengine::error_str(ev.payload.device.error)));
    case EventType::kRotation:
      return QString("rotation %1: %2 pts @ %3 Hz")
          .arg(ev.payload.rotation.rotation_index)
          .arg(ev.payload.rotation.points_in_rotation)
          .arg(ev.payload.rotation.rotation_hz, 0, 'f', 2);
    case EventType::kError:
      return QString("error: %1").arg(scanengine::error_str(ev.payload.error.error));
    default:
      return QString();  // kPointsAvailable and friends are too chatty for a log
  }
}

}  // namespace

EngineHost::EngineHost(QObject* parent) : QObject(parent) {
  scanengine::EngineConfig cfg;
  cfg.app_name = "LidarScan Desktop";
  auto res = scanengine::Engine::create(cfg);
  if (!res.ok()) {
    create_error_ = QString("Engine::create failed: %1").arg(scanengine::error_str(res.error()));
    return;
  }
  engine_ = std::move(res).value();

  scanengine::SubscriptionOptions opts;
  opts.category_mask = scanengine::mask_of(scanengine::EventCategory::kAll);
  opts.capacity = 4096;
  opts.policy = scanengine::OverflowPolicy::kDropOldest;
  auto sub = engine_->events().subscribe(opts);
  if (sub.ok()) sub_ = sub.value();

  pump_timer_.setTimerType(Qt::CoarseTimer);
  connect(&pump_timer_, &QTimer::timeout, this, &EngineHost::pump);
  pump_timer_.start(33);  // ~30 Hz; the viewport has its own display-link clock
}

EngineHost::~EngineHost() {
  pump_timer_.stop();
  if (engine_) {
    if (sub_) (void)engine_->events().unsubscribe(sub_);
    if (engine_->session_active()) (void)engine_->stop_session();
  }
}

const scanengine::PageStore* EngineHost::points() const {
  return engine_ ? &const_cast<scanengine::Engine*>(engine_.get())->points() : nullptr;
}

QString EngineHost::versionString() const {
  return QString::fromUtf8(scanengine::engine_version_string());
}

bool EngineHost::startSession(const QString& lscan_dir, const QString& profile, bool record,
                              QString* err) {
  if (!engine_) {
    if (err) *err = create_error_;
    return false;
  }
  scanengine::SessionConfig cfg;
  cfg.lscan_dir = lscan_dir.toStdString();
  cfg.profile = profile.toStdString();
  cfg.record = record;
  const auto st = engine_->start_session(cfg);
  if (!st.ok()) {
    if (err) {
      *err = QString("start_session: %1 (%2)")
                 .arg(scanengine::error_str(st.error()))
                 .arg(QString::fromUtf8(scanengine::last_error_message()));
    }
    return false;
  }
  session_dir_ = lscan_dir;
  Q_EMIT sessionChanged();
  Q_EMIT logLine(QString("session started%1")
                   .arg(lscan_dir.isEmpty() ? QString(" (live preview, not recording)")
                                            : QString(" -> %1").arg(lscan_dir)));
  return true;
}

bool EngineHost::stopSession(QString* err) {
  if (!engine_) return false;
  const auto st = engine_->stop_session();
  session_dir_.clear();
  Q_EMIT sessionChanged();
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    return false;
  }
  Q_EMIT logLine("session stopped");
  return true;
}

bool EngineHost::sessionActive() const { return engine_ && engine_->session_active(); }

scanengine::DeviceId EngineHost::addD6(const scanengine::D6Config& cfg, QString* err) {
  if (!engine_) return scanengine::kInvalidDeviceId;
  scanengine::DeviceConfig dc;
  dc.kind = scanengine::DeviceKind::kD6;
  dc.d6 = cfg;
  auto r = engine_->add_device(dc);
  if (!r.ok()) {
    if (err) {
      *err = QString("add_device(D6): %1 (%2)")
                 .arg(scanengine::error_str(r.error()))
                 .arg(QString::fromUtf8(scanengine::last_error_message()));
    }
    return scanengine::kInvalidDeviceId;
  }
  Q_EMIT devicesChanged();
  return r.value();
}

scanengine::DeviceId EngineHost::addMid360(const scanengine::Mid360Config& cfg, QString* err) {
  if (!engine_) return scanengine::kInvalidDeviceId;
  scanengine::DeviceConfig dc;
  dc.kind = scanengine::DeviceKind::kMid360;
  dc.mid360 = cfg;
  auto r = engine_->add_device(dc);
  if (!r.ok()) {
    if (err) {
      *err = QString("add_device(Mid-360): %1 (%2)")
                 .arg(scanengine::error_str(r.error()))
                 .arg(QString::fromUtf8(scanengine::last_error_message()));
    }
    return scanengine::kInvalidDeviceId;
  }
  Q_EMIT devicesChanged();
  return r.value();
}

bool EngineHost::removeDevice(scanengine::DeviceId id, QString* err) {
  if (!engine_) return false;
  const auto st = engine_->remove_device(id);
  Q_EMIT devicesChanged();
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    return false;
  }
  return true;
}

QVector<DeviceRow> EngineHost::devices() const {
  QVector<DeviceRow> out;
  if (!engine_) return out;
  auto* e = const_cast<scanengine::Engine*>(engine_.get());
  for (auto id : e->device_ids()) {
    auto h = e->device_health(id);
    DeviceRow row;
    row.id = id;
    if (h.ok()) {
      row.health = h.value();
      row.kind = row.health.kind;
    }
    out.push_back(row);
  }
  return out;
}

QString EngineHost::healthLine() const {
  if (!engine_) return QString("engine unavailable: %1").arg(create_error_);
  auto* e = const_cast<scanengine::Engine*>(engine_.get());
  const auto& store = e->points();
  QString devs;
  for (const auto& d : devices()) {
    if (!devs.isEmpty()) devs += ", ";
    devs += QString("#%1 %2 %3 (%4 pts, %5%)")
                .arg(d.id)
                .arg(scanengine::to_string(d.kind))
                .arg(scanengine::to_string(d.health.state))
                .arg(d.health.points_out)
                .arg(d.health.checksum_pass_rate * 100.0, 0, 'f', 1);
  }
  if (devs.isEmpty()) devs = "no devices";
  return QString("engine %1 · %2 · %3 · store %4 pts / %5 pages%6")
      .arg(scanengine::to_string(e->state()))
      .arg(sessionActive() ? (session_dir_.isEmpty() ? "live preview" : "recording") : "idle")
      .arg(devs)
      .arg(store.total_points())
      .arg(store.page_count())
      .arg(store.dropped_points() ? QString(" · DROPPED %1").arg(store.dropped_points())
                                  : QString());
}

void EngineHost::pump() {
  if (!engine_ || !sub_) return;

  scanengine::Event buf[64];
  std::size_t n = 0;
  while ((n = engine_->events().drain(sub_, buf, 64)) > 0) {
    for (std::size_t i = 0; i < n; ++i) {
      ++events_seen_;
      if (buf[i].type == scanengine::EventType::kEventsDropped) {
        events_dropped_ += buf[i].payload.dropped.count;
      }
      const QString line = describeEvent(buf[i]);
      if (!line.isEmpty()) Q_EMIT logLine(line);
    }
    if (n < 64) break;
  }

  // A5's documented integration point: bound the crash-loss window through
  // idle periods by flushing on the UI's existing poll cadence.
  if (sessionActive() && !session_dir_.isEmpty()) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_flush_ms_ > 1000) {
      last_flush_ms_ = now;
      (void)engine_->recorder().flush();
    }
  }

  Q_EMIT tick();
}

}  // namespace lidarscan
