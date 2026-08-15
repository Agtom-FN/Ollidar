// ProcessingDock.h — C4: the processing queue dock. A job list (kind, state,
// progress bar + stage label, cancel button) driven by A15's JobQueue on its
// own worker thread, with actions to enqueue every job kind the task calls
// for: Post-process, Colorize, Export (chained from a finished job), Transfer
// bundle, Submit to cloud.
//
// OWNERSHIP OF THE QUEUE. `Engine::jobs()` (INT-34, landed mid-task per
// NOTES.md's §9) is THE queue for the whole app: lazily constructed on first
// call, wired to the engine's own EventBus, destroyed at the top of
// ~Engine. This dock does not own a JobQueue — every call goes through
// `host_->engine()->jobs()` — so a future consumer (e.g. a "processing"
// entry in CaptureWindow) shares the exact same worker thread and progress
// stream rather than inventing a second one. This dock reads progress by
// POLLING JobQueue::list() on a QTimer (the same shape ExportDialog/
// ReplayController already use for their own worker threads) rather than
// subscribing to the event bus a second time — simpler, and correct because
// list() already reflects every update the instant it lands.
//
// LIFETIME OF PER-JOB INPUTS. A ColorizeParams::colorizer is a caller-owned
// raw pointer (jobs/job_types.h) that must outlive the job's run() call;
// colorizers_ keeps every in-flight PointColorizer alive by job id and drops
// it once the job has reached a terminal state.
//
// Owner: C4.
#pragma once

#include <QDockWidget>
#include <QString>

#include <map>
#include <memory>

#include "scanengine/color/colorizer.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/jobs/http_transport.h"
#include "scanengine/jobs/job_queue.h"

class QTableWidget;
class QTimer;

namespace lidarscan {

class EngineHost;

class ProcessingDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit ProcessingDock(EngineHost* host, QWidget* parent = nullptr);
  ~ProcessingDock() override;

  // Kept in sync by MainWindow so the job-submission dialogs default
  // sensibly; not a live binding to anything (same posture ExportDialog
  // documents for its own DisplayParams snapshot).
  void setProjectDir(const QString& dir) { project_dir_ = dir; }
  void setCurrentStore(const scanengine::PageStore* store) { current_store_ = store; }

  // Engine::jobs() — see the class comment above for why this dock does not
  // own its own JobQueue.
  scanengine::jobs::JobQueue& queue();

 Q_SIGNALS:
  void logLine(const QString& line);
  // A completed kPostProcess job's result, offered via the row's "Load
  // result" button. `store` is the SAME PageStore the job published
  // kSlamMap pages into (JobQueue::produced_store()) — see NOTES.md for why
  // this is the mechanism rather than "a fresh engine/replay of processed/":
  // A7's own doc (§7) documents that the final cloud is not written back
  // into the .lscan, so there is nothing under processed/ to replay.
  void loadResultRequested(std::shared_ptr<scanengine::PageStore> store, quint64 jobId);

 private:
  void buildUi();
  void refresh();
  void rebuildRow(int row, const scanengine::jobs::Job& job);

  void onPostProcess();
  void onColorize();
  void onExportChain();
  void onTransferBundle();
  void onSubmitCloud();
  void onCancelJob(quint64 jobId);
  void onLoadResult(quint64 jobId);

  std::shared_ptr<scanengine::PageStore> currentStoreShared() const;

  EngineHost* host_ = nullptr;
  std::unique_ptr<scanengine::jobs::HttpTransport> cloud_transport_;

  QTableWidget* table_ = nullptr;
  QTimer* poll_timer_ = nullptr;

  QString project_dir_;
  const scanengine::PageStore* current_store_ = nullptr;

  std::map<quint64, std::shared_ptr<scanengine::color::PointColorizer>> colorizers_;
  std::map<quint64, int> row_of_job_;  // job id -> table row, kept in sync by refresh()
};

}  // namespace lidarscan
