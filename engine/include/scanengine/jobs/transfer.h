// transfer.h — "Extract for transfer" (Tech Spec §3.8's third row): package
// a project as a `.lscan.zip` via A5's zip_export(), and validate a bundle
// that arrives on the receiving machine via zip_import().
//
// No server involved on either side — this is the bit that makes local /
// cloud / transfer "the same pipeline in three places" true for the third
// place: the receiving desktop app runs the SAME PostSlamPipeline / export
// path on the imported .lscan directory that a local capture would.
//
// Owner: A15.
#ifndef SCANENGINE_JOBS_TRANSFER_H
#define SCANENGINE_JOBS_TRANSFER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/jobs/job_types.h"
#include "scanengine/record/lscan.h"

namespace scanengine {
namespace jobs {

// Export side: runs a kTransferExport JobSpec. `progress_cb`/`cancelled`
// follow the same cooperative shape as the rest of jobs/.
//
// UPDATED BY INT-34 (docs/A15-jobs.md §7.4 is now closed). This function used
// to document a real gap: A5's zip_export() took neither a progress callback
// nor a cancel token, so a kTransferExport job reported only start/end and
// could be cancelled only either side of a blocking multi-minute call.
// record/zip.h now takes both (ZipProgressFn + ZipCancelToken), and this
// function passes them through, so:
//   * progress is continuous over the zip's payload bytes, mapped onto
//     0.0..1.0 (or 0.3..1.0 when `include_results` is false and a staging
//     copy had to happen first), and stays monotone across the whole job;
//   * cancellation is polled from INSIDE the copy loop, and the half-written
//     zip is removed rather than left on disk looking openable.
Status run_transfer_export(const TransferExportParams& params, std::function<void(float)> progress_cb,
                            std::function<bool()> cancelled);

// Import-side validation helper (goal 3): extracts `zip_path` into
// `dest_dir` with A5's zip_import(), then opens the result with
// FileRecordReader to produce a manifest sanity report — this is what the
// desktop app's drag-drop/file-association import (Tech Spec §3.13) should
// show the user before trusting a bundle that arrived from another machine.
struct ImportValidationReport {
  bool zip_import_ok = false;
  ScanError zip_import_error = ScanError::kOk;

  bool manifest_present = false;
  bool manifest_ok = false;

  std::uint32_t truncated_tail_chunks = 0;
  std::uint32_t crc_mismatch_chunks = 0;
  std::uint32_t unreadable_streams = 0;

  std::vector<lscan::StreamSummary> streams;

  // A conservative "safe to process" signal: the zip extracted cleanly, a
  // manifest was found and well-formed, and the validation pass found no
  // truncated/corrupt chunks or unreadable stream files. A bundle that
  // fails this can still often be processed (the .lscan format tolerates
  // a missing/corrupt manifest and truncated tails by design,
  // record/lscan.h) — `sane()` is a UI signal, not a hard gate.
  bool sane() const {
    return zip_import_ok && manifest_present && manifest_ok && truncated_tail_chunks == 0 &&
           crc_mismatch_chunks == 0 && unreadable_streams == 0;
  }
};

// Always returns a filled-in report (graded failure, DESIGN.md §3's "failure
// is graded, not binary" philosophy) rather than a bare Status — a UI wants
// to show what was found even when something was wrong, not just a yes/no.
ImportValidationReport import_and_validate(const std::string& zip_path, const std::string& dest_dir);

}  // namespace jobs
}  // namespace scanengine

#endif  // SCANENGINE_JOBS_TRANSFER_H
