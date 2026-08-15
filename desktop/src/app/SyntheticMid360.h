// SyntheticMid360.h — builds a REAL .lscan directory containing
// kMid360Points/kMid360Imu chunks, entirely from the desktop side, so C4's
// "run a real post-process end-to-end from the UI" goal has something to run
// on without any hardware.
//
// This is a port of engine/tests/test_post.cpp's own `write_synthetic_lscan()`
// helper (a ray-cast loop through a synthetic hall, written through A5's real
// `lscan::FileRecordWriter` with real Mid-360 wire-format datagrams) — not a
// re-derivation. engine/ is read-only for this task, and that helper is
// file-local to a test binary, so the desktop needs its own copy to drive the
// exact same recipe from the UI/CLI. See engine/docs/A7-post.md §6.2 for what
// the pipeline measures against this shape of input.
//
// Deliberately NOT the same code as A7's `transcode_livoxdump()` path (§6.3):
// that needs a real fixture file this task cannot rely on being present.
// This path needs nothing but the engine's own public headers.
//
// Owner: C4.
#pragma once

#include <QString>

namespace lidarscan {

struct SyntheticMid360Result {
  bool ok = false;
  QString error;
  quint64 point_packets = 0;
  quint64 imu_packets = 0;
  quint64 points = 0;
  double duration_s = 0.0;
};

// Writes a fresh .lscan directory at `dir` (must not already exist as a
// non-empty directory with a manifest — FileRecordWriter::open() creates the
// skeleton) containing a ray-cast loop through a 24x18x3.5 m synthetic hall:
// real kMid360Points/kMid360Imu chunks, real CRCs, a sealed manifest.
// `duration_s` trades runtime for content — 8 s (the default, matching A7's
// own test) produces a few hundred keyframes' worth of motion and a real
// loop closure in well under a second of post-processing.
SyntheticMid360Result buildSyntheticMid360Project(const QString& dir, double duration_s = 8.0);

}  // namespace lidarscan
