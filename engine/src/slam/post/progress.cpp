// progress.cpp — stage labels (A7). Stable strings: tests and CI logs match
// on them, and the UI shows them verbatim.
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace post {

const char* to_string(PostStage s) noexcept {
  switch (s) {
    case PostStage::kIdle: return "idle";
    case PostStage::kOpening: return "opening recording";
    case PostStage::kOdometry: return "full-density odometry";
    case PostStage::kLoopDetection: return "loop detection";
    case PostStage::kOptimization: return "pose-graph optimization";
    case PostStage::kReintegration: return "re-integration";
    case PostStage::kFiltering: return "filtering";
    case PostStage::kPublishing: return "publishing";
    case PostStage::kDone: return "done";
    case PostStage::kCancelled: return "cancelled";
    case PostStage::kFailed: return "failed";
  }
  return "unknown";
}

}  // namespace post
}  // namespace scanengine
