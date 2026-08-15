// zip.h — the `.lscan.zip` transfer/cloud-upload bundle (Tech Spec §3.8,
// §3.11: "the zip of this directory is the cloud-upload and transfer unit").
//
// Design decision (see docs/A5-lscan.md for the full rationale): entries are
// written STORED (ZIP method 0, uncompressed) rather than vendoring a
// DEFLATE codec (e.g. miniz) into this module. That keeps A5's dependency
// footprint at zero — no new vcpkg port, no vendored third-party source to
// license-track — while still producing a real, standards-conformant ZIP
// any unzip tool (or jobs/'s future upload client) can open. Compression is
// a strictly additive upgrade later: swap the method field and add an
// encoder without changing either function's signature or the bundle
// layout. `.lscan` payload (D6/Mid-360 raw bytes, JPEG keyframes) is already
// high-entropy, so stored-vs-deflated is a smaller win here than it would be
// for, say, a text corpus.
//
// Owner: A5.
#ifndef SCANENGINE_RECORD_ZIP_H
#define SCANENGINE_RECORD_ZIP_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "scanengine/core/error.h"

namespace scanengine {
namespace lscan {

// --- cancel + progress (INT-34, closing docs/A15-jobs.md §7.4) -------------
//
// A kTransferExport job used to report only start/end progress and could
// honour cancellation only either side of the (blocking) zip call, because
// neither function below took a hook. A 2 GiB bundle is minutes of work, so
// "cancel" meant "wait, then discard". These two parameters close that.
//
// Deliberately the SAME SHAPE as A9's ExportCancelToken (export/exporter.h):
// a poll-based flag rather than a callback, because it can be polled cheaply
// from a tight copy loop and set from a thread that is not the one blocked
// inside the call. It is a separate type from post::CancelToken only so that
// record/ keeps depending on nothing but core/ — the module boundary
// DESIGN.md §6 item 3 asks for.
class ZipCancelToken {
 public:
  void request_cancel() noexcept { flag_.store(true, std::memory_order_release); }
  bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }
  void reset() noexcept { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

// Called with monotone non-decreasing `bytes_done` out of `bytes_total`
// (payload bytes; the ZIP's own headers are not counted, so the two agree
// with what a UI would call "the data"), plus the entry being worked on —
// never null, "" between entries. Called on the thread inside the call:
// quick, and must not re-enter — the same rule every other callback in the
// engine follows (DESIGN.md §2).
using ZipProgressFn =
    std::function<void(std::uint64_t bytes_done, std::uint64_t bytes_total, const char* entry)>;

// Recursively zips every regular file under `lscan_dir` into `zip_path`
// (created fresh; an existing file at zip_path is overwritten), using paths
// relative to `lscan_dir` with '/' separators. Streams each file through a
// bounded buffer (two passes: one to compute size+CRC32, one to copy bytes)
// rather than loading it into memory, so this is safe to call on
// multi-gigabyte captures.
//
// `progress` and `cancel` are optional and ADDITIVE — every existing call
// site keeps compiling and behaving identically. Cancellation is polled
// between entries and every 64 KiB inside one, and returns
// ScanError::kCancelled after REMOVING the half-written zip: a partial
// archive on disk is worse than none, because it looks openable and its
// central directory is missing.
Status zip_export(const std::string& lscan_dir, const std::string& zip_path,
                  ZipProgressFn progress = nullptr, ZipCancelToken* cancel = nullptr);

// Extracts a bundle produced by zip_export() (or another tool using the
// stored/method-0 convention) into `dest_dir`. `dest_dir` is created if
// missing; the standard .lscan skeleton (streams/, streams/frames/,
// processed/, merged/, exports/) is always (re)created regardless of which
// directories had files in the archive, so the result is a well-formed
// .lscan directory even when some of those were empty at export time.
//
// Rejects, with `kNotSupported`: any entry using DEFLATE (method 8) or
// another compression method, since no inflate codec is vendored (see the
// header comment). Rejects, with `kInvalidArgument`: any entry whose name
// would resolve outside dest_dir (absolute paths, ".." components — the
// classic "zip-slip" path-traversal attack, relevant here because a
// transfer bundle may arrive from another machine, Tech Spec §3.8 "Extract
// for transfer"). Rejects, with `kCorruptData`: a missing/unreadable
// end-of-central-directory record, or an entry whose extracted bytes do not
// match its recorded CRC32.
//
// `progress`/`cancel` behave as for zip_export(). On cancellation the files
// already extracted are LEFT in `dest_dir` (unlike the export, which removes
// its partial output): the destination is a directory the caller chose and
// may already have had contents, so deleting it is not this function's call
// to make. `kCancelled` plus a directory the caller is free to remove is the
// honest outcome.
Status zip_import(const std::string& zip_path, const std::string& dest_dir,
                  ZipProgressFn progress = nullptr, ZipCancelToken* cancel = nullptr);

}  // namespace lscan
}  // namespace scanengine

#endif  // SCANENGINE_RECORD_ZIP_H
