// merge.h — multi-session merge (§3.10).
//
// SEAM ONLY. Owner: A13 (depends on A7 and A10).
//
// Coarse alignment is automatic when sessions are georeferenced in a shared
// CRS, manual 3-point/drag otherwise; refinement is voxel-downsampled
// point-to-plane ICP per pair with optional global relaxation beyond two
// sessions; the merged cloud keeps PER-SESSION PROVENANCE — which is why
// cloud/PageStore pages are single-stream and page ids are never reused.
#ifndef SCANENGINE_MERGE_MERGE_H
#define SCANENGINE_MERGE_MERGE_H

#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"

namespace scanengine {

struct MergePair {
  std::uint32_t session_a = 0;
  std::uint32_t session_b = 0;
  double b_from_a[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  float rms_residual_m = 0.f;
  float overlap_fraction = 0.f;
  bool converged = false;
};

struct MergeReport {
  std::vector<MergePair> pairs;
  float worst_rms_m = 0.f;
};

class SessionMerger {
 public:
  virtual ~SessionMerger() = default;
  virtual Status add_session(const std::string& lscan_dir, std::uint32_t* session_id) = 0;
  virtual Status coarse_align_georeferenced() = 0;
  virtual Status set_manual_alignment(std::uint32_t session, const double transform[16]) = 0;
  virtual Status refine_icp(float voxel_size_m) = 0;
  virtual Status build_merged(PageStore* out, MergeReport* report) = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_MERGE_MERGE_H
