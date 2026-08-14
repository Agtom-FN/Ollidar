#include "scanengine/jobs/transfer.h"

#include <filesystem>
#include <system_error>

#include "scanengine/record/zip.h"

namespace scanengine {
namespace jobs {

namespace fs = std::filesystem;

namespace {

// Copies manifest.json + streams/ (raw capture only, no processed/exports/
// merged/) into a fresh temp dir so zip_export() — which has no
// include/exclude filter, record/zip.h is not this task's file — can be
// pointed at a subset. Errors are folded into the returned Status; nothing
// here assumes the source tree is well-formed (a manifest may legitimately
// be missing, record/lscan.h's crash-safety contract).
Status stage_raw_only(const std::string& project_dir, const std::string& staging_dir) {
  std::error_code ec;
  fs::remove_all(staging_dir, ec);
  fs::create_directories(staging_dir, ec);
  if (ec) {
    return set_last_error(ScanError::kFileError, "jobs/transfer: cannot create staging dir '%s'",
                           staging_dir.c_str());
  }

  const fs::path src(project_dir);
  const fs::path manifest = src / lscan::kManifestFile;
  if (fs::exists(manifest, ec)) {
    fs::copy_file(manifest, fs::path(staging_dir) / lscan::kManifestFile,
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return set_last_error(ScanError::kFileError, "jobs/transfer: cannot stage manifest from '%s'",
                             project_dir.c_str());
    }
  }

  const fs::path streams_src = src / lscan::kStreamsDir;
  if (fs::exists(streams_src, ec)) {
    fs::copy(streams_src, fs::path(staging_dir) / lscan::kStreamsDir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return set_last_error(ScanError::kFileError, "jobs/transfer: cannot stage streams/ from '%s'",
                             project_dir.c_str());
    }
  }
  return kOkStatus;
}

}  // namespace

Status run_transfer_export(const TransferExportParams& params, std::function<void(float)> progress_cb,
                            std::function<bool()> cancelled) {
  if (params.project_dir.empty() || params.zip_path.empty()) {
    return set_last_error(ScanError::kInvalidArgument,
                           "jobs/transfer: project_dir and zip_path are both required");
  }
  if (progress_cb) progress_cb(0.f);
  if (cancelled && cancelled()) {
    return set_last_error(ScanError::kCancelled, "jobs/transfer: cancelled before staging");
  }

  std::string source_dir = params.project_dir;
  std::string staging_dir;
  if (!params.include_results) {
    staging_dir = params.zip_path + ".staging";
    SCAN_TRY(stage_raw_only(params.project_dir, staging_dir));
    source_dir = staging_dir;
    if (progress_cb) progress_cb(0.3f);
  }

  // zip_export() (record/zip.h, A5) has neither a progress callback nor a
  // cancel token — see this file's header comment. Cancellation is only
  // honoured either side of this call.
  if (cancelled && cancelled()) {
    std::error_code ec;
    if (!staging_dir.empty()) fs::remove_all(staging_dir, ec);
    return set_last_error(ScanError::kCancelled, "jobs/transfer: cancelled before zip_export");
  }
  const Status zip_status = lscan::zip_export(source_dir, params.zip_path);

  std::error_code ec;
  if (!staging_dir.empty()) fs::remove_all(staging_dir, ec);

  if (!zip_status.ok()) return zip_status;

  if (cancelled && cancelled()) {
    fs::remove(params.zip_path, ec);
    return set_last_error(ScanError::kCancelled, "jobs/transfer: cancelled; produced zip discarded");
  }

  if (progress_cb) progress_cb(1.f);
  return kOkStatus;
}

ImportValidationReport import_and_validate(const std::string& zip_path, const std::string& dest_dir) {
  ImportValidationReport report;

  const Status import_status = lscan::zip_import(zip_path, dest_dir);
  report.zip_import_ok = import_status.ok();
  report.zip_import_error = import_status.error();
  if (!report.zip_import_ok) return report;

  lscan::FileRecordReader reader;
  const Status open_status = reader.open(dest_dir);
  if (!open_status.ok()) {
    // A structurally-broken directory after a successful zip_import() would
    // be surprising, but report it rather than crash: manifest fields stay
    // at their defaults (both false) and streams stays empty, which already
    // reads as "not sane" to a caller checking sane().
    return report;
  }

  report.manifest_present = reader.manifest_present();
  report.manifest_ok = reader.manifest_ok();
  report.truncated_tail_chunks = reader.warnings().truncated_tail_chunks;
  report.crc_mismatch_chunks = reader.warnings().crc_mismatch_chunks;
  report.unreadable_streams = reader.warnings().unreadable_streams;
  report.streams = reader.stream_summaries();
  (void)reader.close();
  return report;
}

}  // namespace jobs
}  // namespace scanengine
