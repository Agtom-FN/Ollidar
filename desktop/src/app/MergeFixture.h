// MergeFixture.h — C6 evidence fixture: the same synthetic multi-session
// building engine/tests/test_merge.cpp measures A13 against. Ported rather
// than shared: the generator (Rng/sample_plane/building_surfaces/session_cloud
// /Fixture) is file-local to that test binary and engine/ is read-only for
// this task (NOTES.md's OWNERSHIP note) — the same posture SyntheticMid360.h
// (C4) and SyntheticBuilding.h (C5) already established for their own docks'
// evidence fixtures.
//
// THE BUILDING: 30 x 12 x 3 m, two interior partitions splitting a corridor at
// y > 8. Three sessions cover x in [0,13], [9,22] and [18,30] — 0-1 and 1-2
// overlap by 4 m each, 0-2 share nothing (the merge workbench's "these two do
// not see the same place" case comes for free from the same fixture). Session
// 0 is the anchor. Each session's cloud is independently re-sampled (its own
// RNG stream) with 5 mm Gaussian range noise, so two sessions of the same wall
// are two different samplings of it, not the same points twice.
//
// GEOREFERENCING: each session additionally gets a SessionGeoref anchored at
// its OWN ENU origin (three different days' first-fix locations, 200-400 m
// apart, one shared CRS) — this is A13-merge.md §3's "shared CRS is not
// shared frame" trap, reproduced on purpose so the desktop's georeferenced
// auto-align path has something real to compose through ECEF.
//
// Owner: C6.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <QString>

#include "scanengine/cloud/point_page.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/merge/session.h"

namespace lidarscan {

struct MergeFixtureSession {
  QString provenanceId;
  // In the session's OWN local frame (what a real capture's PageStore would
  // hold before any merge alignment is applied).
  std::vector<scanengine::PointVertex> cloud;
  bool has_georef = false;
  scanengine::merge::SessionGeoref georef;
};

class MergeFixture {
 public:
  MergeFixture();

  // withGeoref selects whether each session's SessionGeoref is populated —
  // false exercises the manual/ICP paths without a shortcut through
  // align_georeferenced().
  std::vector<MergeFixtureSession> sessions(bool withGeoref) const;

  // The 4 hand-picked physical features test_merge.cpp calls kPickWorld,
  // indexed the same way, transformed into `session`'s own local frame
  // (source) and the anchor's (session 0's) own local frame (target — the
  // merged frame, since the anchor's world_from_session is identity by
  // definition). noiseM == 0 reproduces the "exact picks" case; > 0 adds the
  // same per-click Gaussian jitter the noisy-pick case in
  // engine/tests/test_merge.cpp uses, with the SAME xorshift64+Box-Muller Rng
  // (deterministic across every platform this app builds on, unlike
  // <random>).
  void pick(int session, std::size_t index, double noiseM, std::uint64_t seed,
            float sourceLocalOut[3], float targetLocalOut[3]) const;

  // world_from_s[0]^-1 * world_from_s[session] — anchor-relative ground
  // truth, what every alignment error in NOTES.md's C6 section is measured
  // against (se3::transform_error).
  void truthWorldFromSession(int session, double out[16]) const;

  const scanengine::crs::EnuFrame& sessionEnu(int session) const { return session_enu_[session]; }
  const scanengine::crs::EnuFrame& worldEnu() const { return world_enu_; }

 private:
  double world_from_s_[3][16];
  std::vector<scanengine::PointVertex> cloud_[3];
  scanengine::crs::EnuFrame world_enu_;
  scanengine::crs::EnuFrame session_enu_[3];
};

}  // namespace lidarscan
