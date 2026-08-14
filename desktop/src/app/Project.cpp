#include "app/Project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vector>

#include "scanengine/core/engine.h"

namespace lidarscan {
namespace {

QString streamName(scanengine::StreamId s) {
  switch (s) {
    case scanengine::StreamId::kLidarD6: return "lidar (COIN-D6 raw)";
    case scanengine::StreamId::kLidarMid360: return "lidar (Mid-360 packets)";
    case scanengine::StreamId::kImu: return "imu";
    case scanengine::StreamId::kPoseAr: return "poses (ARCore)";
    case scanengine::StreamId::kGnss: return "gnss";
    case scanengine::StreamId::kCameraFrames: return "camera keyframes";
    case scanengine::StreamId::kPoseFused: return "poses (fused)";
    case scanengine::StreamId::kUnknown:
    default: return "unknown";
  }
}

QString jsonStringValue(const QString& json, const QString& key) {
  const QString needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return QString();
  p = json.indexOf(':', p + needle.size());
  if (p < 0) return QString();
  int q = json.indexOf('"', p);
  if (q < 0) return QString();
  const int end = json.indexOf('"', q + 1);
  if (end < 0) return QString();
  return json.mid(q + 1, end - q - 1);
}

bool jsonBoolValue(const QString& json, const QString& key) {
  const QString needle = "\"" + key + "\"";
  int p = json.indexOf(needle);
  if (p < 0) return false;
  p = json.indexOf(':', p + needle.size());
  if (p < 0) return false;
  return json.mid(p + 1, 8).contains("true");
}

}  // namespace

ProjectInfo readProject(const QString& dir) {
  ProjectInfo info;
  info.dir = QDir(dir).absolutePath();
  info.name = QFileInfo(info.dir).fileName();

  scanengine::lscan::FileRecordReader reader;
  const auto st = reader.open(info.dir.toStdString());
  if (!st.ok()) {
    info.error = QString("%1 (%2)")
                     .arg(scanengine::error_str(st.error()))
                     .arg(QString::fromUtf8(scanengine::last_error_message()));
    return info;
  }
  info.valid = true;
  info.manifest_present = reader.manifest_present();
  info.manifest_ok = reader.manifest_ok();
  info.manifest_raw = QString::fromStdString(reader.manifest_raw());
  info.sealed = jsonBoolValue(info.manifest_raw, "sealed");
  info.profile = jsonStringValue(info.manifest_raw, "profile");

  qint64 t_first = 0, t_last = 0;
  bool have_t = false;
  for (const auto& s : reader.stream_summaries()) {
    StreamInfo si;
    si.id = s.stream;
    si.name = streamName(s.stream);
    si.chunks = s.chunk_count;
    si.bytes = s.bytes;
    si.t_first_ns = s.t_first_ns;
    si.t_last_ns = s.t_last_ns;
    info.streams.push_back(si);
    info.total_chunks += s.chunk_count;
    info.total_bytes += s.bytes;
    if (s.chunk_count > 0) {
      if (!have_t) {
        t_first = s.t_first_ns;
        t_last = s.t_last_ns;
        have_t = true;
      } else {
        t_first = std::min<qint64>(t_first, s.t_first_ns);
        t_last = std::max<qint64>(t_last, s.t_last_ns);
      }
    }
    if (s.stream == scanengine::StreamId::kLidarD6 && s.chunk_count > 0) info.has_d6_raw = true;
  }
  info.duration_s = have_t && t_last > t_first ? double(t_last - t_first) / 1e9 : 0.0;

  const auto& w = reader.warnings();
  info.truncated_tail_chunks = w.truncated_tail_chunks;
  info.crc_mismatch_chunks = w.crc_mismatch_chunks;
  info.unreadable_streams = w.unreadable_streams;

  (void)reader.close();
  return info;
}

bool createProject(const QString& dir, const QString& profile, QString* err) {
  QDir d(dir);
  if (d.exists() && !d.isEmpty()) {
    if (err) *err = QString("%1 already exists and is not empty").arg(dir);
    return false;
  }
  scanengine::lscan::FileRecordWriter w;
  w.set_profile(profile.toStdString());
  const auto st = w.open(QDir(dir).absolutePath().toStdString());
  if (!st.ok()) {
    if (err) {
      *err = QString("%1 (%2)")
                 .arg(scanengine::error_str(st.error()))
                 .arg(QString::fromUtf8(scanengine::last_error_message()));
    }
    return false;
  }
  const auto cs = w.close();
  if (!cs.ok()) {
    if (err) *err = scanengine::error_str(cs.error());
    return false;
  }
  return true;
}

bool importRawD6(const QString& raw_file, const QString& project_dir, const QString& profile,
                 QString* err, quint64* bytes_in, quint64* points_out) {
  QFile f(raw_file);
  if (!f.open(QIODevice::ReadOnly)) {
    if (err) *err = QString("cannot read %1").arg(raw_file);
    return false;
  }

  // A dedicated Engine for the import: the app's own Engine may be mid-session,
  // and importing must not disturb it.
  scanengine::EngineConfig ecfg;
  ecfg.app_name = "LidarScan import";
  auto res = scanengine::Engine::create(ecfg);
  if (!res.ok()) {
    if (err) *err = QString("Engine::create: %1").arg(scanengine::error_str(res.error()));
    return false;
  }
  std::unique_ptr<scanengine::Engine> eng = std::move(res).value();

  scanengine::SessionConfig scfg;
  scfg.lscan_dir = QDir(project_dir).absolutePath().toStdString();
  scfg.profile = profile.toStdString();
  scfg.record = true;
  const auto ss = eng->start_session(scfg);
  if (!ss.ok()) {
    if (err) {
      *err = QString("start_session: %1 (%2)")
                 .arg(scanengine::error_str(ss.error()))
                 .arg(QString::fromUtf8(scanengine::last_error_message()));
    }
    return false;
  }

  scanengine::DeviceConfig dc;
  dc.kind = scanengine::DeviceKind::kD6;
  // No transport write function exists for a file import, so the driver must
  // not wait for start/stop ACKs it can never receive.
  dc.d6.send_start_stop_commands = false;
  dc.d6.require_start_ack = false;
  dc.d6.serial.port_name = "import";
  auto dev = eng->add_device(dc);
  if (!dev.ok()) {
    if (err) *err = QString("add_device: %1").arg(scanengine::error_str(dev.error()));
    (void)eng->stop_session();
    return false;
  }
  const scanengine::DeviceId id = dev.value();

  // 230400 baud, 8N1 = 10 bits per byte on the wire = 23,040 bytes/s.
  constexpr double kBytesPerSecond = 230400.0 / 10.0;
  constexpr qint64 kChunk = 2048;
  qint64 total = 0;
  std::vector<std::uint8_t> buf(kChunk);
  const qint64 t0 = 1'000'000'000LL;  // arbitrary monotonic origin
  while (true) {
    const qint64 got = f.read(reinterpret_cast<char*>(buf.data()), kChunk);
    if (got <= 0) break;
    const qint64 t = t0 + qint64(double(total) / kBytesPerSecond * 1e9);
    const auto st = eng->push_serial_bytes(
        id, scanengine::ByteSpan(buf.data(), std::size_t(got)), scanengine::TimePoint{t});
    if (!st.ok() && st.error() != scanengine::ScanError::kAgain) {
      if (err) *err = QString("push_serial_bytes: %1").arg(scanengine::error_str(st.error()));
      (void)eng->stop_session();
      return false;
    }
    total += got;
  }

  if (bytes_in) *bytes_in = quint64(total);
  auto h = eng->device_health(id);
  if (points_out) *points_out = h.ok() ? h.value().points_out : 0;

  const auto st = eng->stop_session();  // seals the manifest
  if (!st.ok()) {
    if (err) *err = QString("stop_session: %1").arg(scanengine::error_str(st.error()));
    return false;
  }
  return true;
}

}  // namespace lidarscan
