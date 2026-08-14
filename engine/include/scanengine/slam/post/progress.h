// progress.h — the cancel token and the progress vocabulary every A7 post
// stage speaks.
//
// Tech Spec §3.3 requires the Mid-360 post pipeline to be "cancellable,
// progress-reported", and §3.8 puts it behind a foreground service (Android)
// or a background task (desktop). A15 owns the job layer; A7 owns the
// primitives it drives. So this header deliberately contains NO threading
// policy and NO job identity — just:
//
//   * CancelToken   an atomic flag the caller sets from any thread. Every
//                   long-running loop in slam/post/ polls it at a bounded
//                   granularity and unwinds with ScanError::kCancelled,
//                   leaving no partial output in the caller's PageStore
//                   beyond what had already been appended.
//   * PostStage     what the pipeline is doing right now, as a stable enum
//                   plus a stable English label (the UI shows the label; a
//                   test matches on the enum).
//   * PostProgress  0..1 overall, plus the same within the current stage and
//                   the raw item counts, because "47%" is useless in a log
//                   and "keyframe 812 / 1,730" is not.
//
// WHY A PLAIN C++ CALLBACK AND NOT AN EVENT. jobs/job.h says progress is
// reported through EventType::kJobProgress on the engine event bus. That is
// right for a *job*; it is wrong for the pipeline itself, which must be
// runnable in a unit test and by the cloud worker's CLI with no Engine
// instance in sight. A15 subscribes a two-line lambda that republishes onto
// the bus. Same reason LioOdometry takes IMU as primitives (A6).
//
// Owner: A7.
#ifndef SCANENGINE_SLAM_POST_PROGRESS_H
#define SCANENGINE_SLAM_POST_PROGRESS_H

#include <atomic>
#include <cstdint>
#include <functional>

namespace scanengine {
namespace post {

// A cooperative cancellation flag. Copyable it is NOT — callers share it by
// pointer (A15 owns one per job and hands the pipeline a pointer), which
// keeps the ownership question out of the pipeline entirely.
//
// Threading: cancel() is safe from any thread at any time, including before
// run() starts and after it returns. The relaxed ordering is deliberate:
// the only thing published is the flag itself, there is no data handed over
// with it, and a cancellation that is observed one loop iteration late is
// indistinguishable from one that arrived one loop iteration later.
class CancelToken {
 public:
  CancelToken() = default;
  CancelToken(const CancelToken&) = delete;
  CancelToken& operator=(const CancelToken&) = delete;

  void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
  bool cancelled() const noexcept { return flag_.load(std::memory_order_relaxed); }
  void reset() noexcept { flag_.store(false, std::memory_order_relaxed); }

 private:
  std::atomic<bool> flag_{false};
};

// Null-safe helper: `cancelled(nullptr)` is false, so every call site reads
// as one condition rather than two.
inline bool cancelled(const CancelToken* t) noexcept {
  return t != nullptr && t->cancelled();
}

// Stable and append-only, like every other enum that crosses a UI boundary.
// The numeric order is also the execution order, which is what lets a UI
// draw a stage strip without a lookup table.
enum class PostStage : std::uint8_t {
  kIdle = 0,
  kOpening = 1,        // open the .lscan, validate, size the work
  kOdometry = 2,       // full-density LIO re-run + keyframing
  kLoopDetection = 3,  // Scan Context candidates + ICP verification
  kOptimization = 4,   // pose-graph Gauss-Newton
  kReintegration = 5,  // second pass: points through the optimized trajectory
  kFiltering = 6,      // voxel dedup + statistical outlier filter
  kPublishing = 7,     // into the PageStore
  kDone = 8,
  kCancelled = 9,
  kFailed = 10,
};

// Stable English, safe to log, matched on by tests. Never localized.
const char* to_string(PostStage s) noexcept;

struct PostProgress {
  PostStage stage = PostStage::kIdle;
  const char* label = "idle";  // to_string(stage); never null
  // Overall completion, 0..1, monotone non-decreasing across a whole run.
  // The per-stage weights are fixed (see post_pipeline.cpp) rather than
  // measured, because a progress bar that jumps backwards when the second
  // pass turns out cheaper than the first is worse than one that is
  // slightly wrong.
  float fraction = 0.f;
  // Completion within the current stage, 0..1.
  float stage_fraction = 0.f;
  // The honest numbers. Units depend on the stage (chunks, keyframes,
  // candidate pairs, iterations, points) — `label` says which.
  std::uint64_t done = 0;
  std::uint64_t total = 0;
};

// Invoked on the thread inside run(). Must be quick and must not re-enter
// the pipeline — the same rule EventBus callbacks and PageStore subscribers
// follow (DESIGN.md §2).
using PostProgressFn = std::function<void(const PostProgress&)>;

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_PROGRESS_H
