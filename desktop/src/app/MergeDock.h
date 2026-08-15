// MergeDock.h — C6 §3.10 merge workbench: session list, coarse alignment
// (georeferenced auto / 3-point manual / yaw search), ICP refinement with a
// per-iteration residual chart, build + per-session-coloured viewport
// preview, export. Drives engine/include/scanengine/merge/merge.h's
// MergeProject directly — see engine/docs/A13-merge.md for the API this
// wraps and NOTES.md for the desktop-side design notes (why sessions are
// loaded the way they are, why 3-point picking reuses the C3 measure tool).
//
// SESSION SOURCES. Three ways a session's cloud gets into this dock, and all
// three end up as the same scanengine::merge::SessionInput shape:
//   * "Build synthetic fixture (evidence)" — MergeFixture.h, ported from
//     engine/tests/test_merge.cpp, three known-geometry overlapping sessions
//     built directly in memory (SessionCloud's supported, DOCUMENTED input
//     shape per A13-merge.md §2 — not a workaround).
//   * "Add from open project" / "Import .lscan project…" — both go through
//     MergeSessionLoader.h (a private Engine + unpaced ReplaySource over the
//     project's D6 raw chunks), differing only in which directory the button
//     prefills.
//
// 3-POINT MANUAL PICKING reuses ViewportWindow's existing measure-tool
// picking machinery (setMeasureMode/measurementsChanged/measurements()) —
// see the .cpp for the exact mechanics: the SOURCE session's cloud is shown
// for the first click of each pair, the TARGET (anchor) session's cloud is
// swapped in for the second click, and the completed two-click
// MeasureSegment becomes one PointCorrespondence. No new picking code, no
// second viewport.
//
// Owner: C6.
#pragma once

#include <QDockWidget>
#include <QString>

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/merge/merge.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QWidget;

namespace scanengine {
class Engine;
}

namespace lidarscan {

class ViewportWindow;

// A small QPainter line chart of one pair's ICP residual trace
// (scanengine::merge::IcpIteration::rms_m per iteration, with the
// correspondence-gate stage boundary marked) — A13-merge.md §5's "residual
// report" made visual. Deliberately raw QPainter, no charting library: one
// polyline and a handful of gridlines.
class MergeIcpChart : public QWidget {
  Q_OBJECT
 public:
  explicit MergeIcpChart(QWidget* parent = nullptr);
  void setTrace(const std::vector<scanengine::merge::IcpIteration>& trace, const QString& title);
  void clearTrace();

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  std::vector<scanengine::merge::IcpIteration> trace_;
  QString title_;
};

class MergeDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit MergeDock(ViewportWindow* viewport, QWidget* parent = nullptr);
  ~MergeDock() override;

  // Kept in sync by MainWindow (same posture as ProcessingDock::setProjectDir/
  // PlanDock::setProjectDir) so "Add from open project" has something to add
  // without this dock reaching back into MainWindow for it.
  void setOpenProjectDir(const QString& dir) { open_project_dir_ = dir; }

  // --- session sources (all funnel through addSession()) -------------------
  // Real .lscan project → MergeSessionLoader (Engine + unpaced ReplaySource).
  // Used by both "Add from open project" and "Import .lscan project…" (they
  // differ only in which directory is prefilled) and by
  // addProjectSessionForCli().
  bool addFromProject(const QString& lscanDir, const QString& provenanceIdHint, QString* err);

  // --- evidence/CLI hooks (mirror ProcessingDock/PlanDock's *ForCli posture:
  // same code the buttons call, minus the modal dialog / mouse I/O) ---------
  int addFixtureSessionsForCli(bool withGeoref);
  bool addProjectSessionForCli(const QString& lscanDir, const QString& provenanceId, QString* err);
  bool alignGeoreferencedForCli(scanengine::merge::MergeProject::GeorefAlignReport* out, QString* err);
  // Feeds the SAME buffer-fill/finalize path the interactive 3-click flow
  // uses (recordPick()) with caller-supplied correspondences — see the .cpp
  // for why CLI evidence supplies known coordinates rather than synthesizing
  // screen-space projection (mirrors A13's own exact/noisy-pick tests, which
  // are hand-specified 3D points too, not simulated clicks).
  bool alignManualForCli(quint32 sessionId,
                         const std::vector<std::array<float, 3>>& sourceLocal,
                         const std::vector<std::array<float, 3>>& targetLocal,
                         scanengine::merge::CorrespondenceSolution* out, QString* err);
  bool yawSearchForCli(quint32 sessionId, quint32 reference,
                       scanengine::merge::YawSearchResult* out, QString* err);
  bool refineForCli(QString* err);
  bool buildAndPublishForCli(bool colorBySession, QString* err);
  // Selects a Pairs-table row so its residual chart renders — for
  // --merge-dock-shot, which grabs this whole dock the way --plan-shot grabs
  // PlanDock.
  void selectPairForCli(int row);

  const scanengine::merge::MergeReport* reportOrNull() const;
  scanengine::PageStore* mergedStore() { return &merged_store_; }
  int sessionCount() const { return int(sessions_.size()); }

 Q_SIGNALS:
  void logLine(const QString& line);
  void exportMergedRequested();

 private:
  struct SessionRow {
    QString provenanceId;
    QString sourceDir;  // empty for the synthetic fixture
    std::unique_ptr<scanengine::Engine> engine;       // set for D6-replay sessions
    std::unique_ptr<scanengine::PageStore> ownStore;  // set for synthetic-fixture sessions
    scanengine::merge::SessionGeoref georef;
    bool hasGeoref = false;
    bool inProject = false;
    quint32 mergeId = 0;
    scanengine::PageStore* store();
  };

  void buildUi();
  void refreshSessionTable();
  void refreshPairTable();
  void refreshChartFor(int pairRow);
  void logAndStatus(const QString& s);

  // Returns the row index just added, or -1. `cloudPoints` is copied into a
  // freshly owned PageStore (synthetic sessions only — real sessions already
  // own their points via the loader's Engine).
  int addSession(const QString& provenanceId, const QString& sourceDir,
                std::unique_ptr<scanengine::Engine> engine,
                const std::vector<scanengine::PointVertex>* syntheticPoints,
                const scanengine::merge::SessionGeoref* georef);

  // --- UI handlers ---
  void onAddFromOpenProject();
  void onImportProject();
  void onBuildFixture();
  void onRemoveSelected();
  void onSetAnchor();
  void onAlignGeoref();
  void onStartPick();
  void onCancelPick();
  void onYawSearch();
  void onSurveyOverlap();
  void onRefine();
  void onBuild();
  void onExport();
  void onSessionTableSelectionChanged();
  void onPairTableSelectionChanged();

  // --- 3-point picking state machine, reusing the C3 measure tool ---------
  void beginPickPhase();
  void recordPick(const float sourceLocal[3], const float targetLocal[3]);
  void finalizePicks();
  void abortPicking(const QString& why);
  void onViewportMeasurementsChanged();

  ViewportWindow* viewport_ = nullptr;
  QString open_project_dir_;

  std::unique_ptr<scanengine::merge::MergeProject> project_;
  std::vector<SessionRow> sessions_;
  scanengine::PageStore merged_store_;
  scanengine::merge::MergeResult last_result_;
  bool has_result_ = false;

  // Per-pair init transform captured right before refine() runs, so the
  // residual chart can reproduce that pair's own trace on demand via
  // scanengine::merge::refine_pair() — see MergeDock.cpp.
  std::map<std::uint64_t, std::array<double, 16>> pre_refine_init_;

  // pick-mode state
  enum class PickSubPhase { kAwaitingSource, kAwaitingTarget };
  bool picking_ = false;
  PickSubPhase pick_sub_phase_ = PickSubPhase::kAwaitingSource;
  quint32 picking_session_ = 0;
  std::vector<scanengine::merge::PointCorrespondence> picks_;
  static constexpr std::size_t kPickCount = 3;

  // --- widgets ---
  QTableWidget* session_table_ = nullptr;
  QPushButton* set_anchor_btn_ = nullptr;
  QPushButton* remove_btn_ = nullptr;

  QComboBox* align_session_combo_ = nullptr;
  QComboBox* yaw_reference_combo_ = nullptr;
  QPushButton* georef_btn_ = nullptr;
  QPushButton* pick_btn_ = nullptr;
  QPushButton* pick_cancel_btn_ = nullptr;
  QPushButton* yaw_btn_ = nullptr;
  QLabel* align_status_ = nullptr;

  QPushButton* survey_btn_ = nullptr;
  QPushButton* refine_btn_ = nullptr;
  QLabel* refine_status_ = nullptr;

  QTableWidget* pair_table_ = nullptr;
  MergeIcpChart* chart_ = nullptr;

  QCheckBox* color_by_session_ = nullptr;
  QDoubleSpinBox* dedup_voxel_ = nullptr;
  QPushButton* build_btn_ = nullptr;
  QPushButton* export_btn_ = nullptr;
  QLabel* build_status_ = nullptr;
};

}  // namespace lidarscan
