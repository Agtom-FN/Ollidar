#include "app/MergeSessionLoader.h"

#include "app/Project.h"
#include "scanengine/record/replay.h"

namespace lidarscan {

bool MergeSessionLoader::load(const QString& lscanDir, QString* err) {
  const ProjectInfo info = readProject(lscanDir);
  if (!info.valid) {
    if (err) *err = QString("%1: %2").arg(lscanDir, info.error);
    return false;
  }
  if (!info.has_d6_raw) {
    if (err) {
      *err =
          "this project has no D6 raw chunks. record/replay.h only forwards "
          "ChunkType::kD6Raw today, so a Mid-360-only project cannot be decoded back "
          "into a merge session yet (A3/A10 need an analogous Engine push entry point "
          "first — see engine/docs/A5-lscan.md §4/§8, same gate MainWindow::startReplay() "
          "already enforces for the ordinary replay path).";
    }
    return false;
  }

  scanengine::EngineConfig ecfg;
  ecfg.app_name = "LidarScan Desktop (merge session loader)";
  auto res = scanengine::Engine::create(ecfg);
  if (!res.ok()) {
    if (err) *err = QString("Engine::create failed: %1").arg(scanengine::error_str(res.error()));
    return false;
  }
  engine_ = std::move(res).value();

  // Live preview, same as ReplayController: an empty lscan_dir means nothing
  // is recorded, so replaying a project never appends bytes back into itself.
  scanengine::SessionConfig scfg;
  scfg.lscan_dir.clear();
  scfg.profile = "quickscan";
  scfg.record = false;
  const auto sst = engine_->start_session(scfg);
  if (!sst.ok()) {
    if (err) *err = QString("start_session: %1").arg(scanengine::error_str(sst.error()));
    engine_.reset();
    return false;
  }

  scanengine::D6Config d6;
  d6.send_start_stop_commands = false;  // no transport to write to
  d6.require_start_ack = false;
  d6.serial.port_name = "merge-replay";
  scanengine::DeviceConfig dc;
  dc.kind = scanengine::DeviceKind::kD6;
  dc.d6 = d6;
  const auto dres = engine_->add_device(dc);
  if (!dres.ok()) {
    if (err) *err = QString("add_device(D6): %1").arg(scanengine::error_str(dres.error()));
    engine_.reset();
    return false;
  }

  scanengine::lscan::ReplaySource source(*engine_);
  scanengine::lscan::ReplayConfig cfg;
  cfg.lscan_dir = lscanDir.toStdString();
  cfg.target_device = dres.value();
  cfg.chunk_type = scanengine::lscan::ChunkType::kD6Raw;
  cfg.speed = 0.0;  // unpaced: a merge workbench needs the whole cloud, not a live trickle

  const auto rst = source.run(cfg);
  if (!rst.ok()) {
    if (err) *err = QString("replay: %1").arg(scanengine::error_str(rst.error()));
    engine_.reset();
    return false;
  }
  chunks_replayed_ = source.stats().chunks_replayed;
  points_decoded_ = engine_->points().total_points();
  return true;
}

}  // namespace lidarscan
