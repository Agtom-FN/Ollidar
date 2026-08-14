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

#include <string>

#include "scanengine/core/error.h"

namespace scanengine {
namespace lscan {

// Recursively zips every regular file under `lscan_dir` into `zip_path`
// (created fresh; an existing file at zip_path is overwritten), using paths
// relative to `lscan_dir` with '/' separators. Streams each file through a
// bounded buffer (two passes: one to compute size+CRC32, one to copy bytes)
// rather than loading it into memory, so this is safe to call on
// multi-gigabyte captures.
Status zip_export(const std::string& lscan_dir, const std::string& zip_path);

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
Status zip_import(const std::string& zip_path, const std::string& dest_dir);

}  // namespace lscan
}  // namespace scanengine

#endif  // SCANENGINE_RECORD_ZIP_H
