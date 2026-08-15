// MergeSessionLoader.h — turns a real `.lscan` directory into a merge
// session's cloud, for C6's "add from open project" / "import other .lscan
// projects" (NOTES.md §4's C6 row: "New window over A13 ... per-session
// provenance and colour-by-session need no renderer redesign").
//
// engine/docs/A13-merge.md §2 is explicit that MergeProject's supported input
// is memory (SessionInput), not a `.lscan` directory: nothing writes a
// processed cloud into a `.lscan` yet, so `SessionMerger::add_session(dir)`
// is declared but not implemented. The one real, already-decoded stream this
// desktop can point a merge session at is exactly what ReplayController
// already proves end to end (C1 §"replay == capture"): a project's raw D6
// bytes, replayed UNPACED through a live scanengine::Engine, landing decoded
// points on StreamId::kLidarD6.
//
// So this loader is ReplayController's own shape (a private Engine + a live
// preview session + one D6 device + lscan::ReplaySource at speed <= 0),
// except: (a) each loaded session gets its OWN Engine instance rather than
// sharing the app's one EngineHost, because a merge project holds several
// sessions' clouds alive at once and `merge/session.h`'s SessionCloud is a
// list of NON-OWNING spans into whatever produced them (collect_pages()) —
// so the Engine (and the PageStore inside it) must outlive the MergeProject;
// and (b) it runs synchronously to completion (speed <= 0, no pacing) rather
// than on a polled worker thread, because a merge workbench needs the whole
// cloud before alignment can start, not a live-growing preview.
//
// A project with no D6 raw chunks (Mid-360-only, per record/replay.h's
// documented "only ChunkType::kD6Raw replays today") is refused with a clear
// message rather than silently producing an empty session — the same
// `ProjectInfo::has_d6_raw` gate MainWindow::startReplay() already uses.
//
// Owner: C6.
#pragma once

#include <QString>

#include <memory>

#include "scanengine/core/engine.h"

namespace lidarscan {

class MergeSessionLoader {
 public:
  // Replays every kD6Raw chunk in `lscanDir` unpaced into a fresh, owned
  // Engine and reports the outcome. On success, engine() and its PageStore
  // stay alive for as long as this object does — the caller (MergeDock) keeps
  // one of these per loaded session for exactly that reason.
  bool load(const QString& lscanDir, QString* err);

  scanengine::Engine* engine() { return engine_.get(); }
  quint64 pointsDecoded() const { return points_decoded_; }
  quint64 chunksReplayed() const { return chunks_replayed_; }

  // Transfers ownership out (the caller keeps the Engine — and its
  // PageStore — alive for as long as a merge session's SessionCloud spans
  // point into it). Only meaningful after a successful load().
  std::unique_ptr<scanengine::Engine> takeEngine() { return std::move(engine_); }

 private:
  std::unique_ptr<scanengine::Engine> engine_;
  quint64 points_decoded_ = 0;
  quint64 chunks_replayed_ = 0;
};

}  // namespace lidarscan
