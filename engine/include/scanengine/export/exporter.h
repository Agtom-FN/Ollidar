// exporter.h — point-cloud and document export writers.
//
// SEAM ONLY for kDxf/kPdf. Owner: A9 (PLY binary+RGB, LAS 1.4 georeferenced,
// PCD, plus reference-reader tests — IMPLEMENTED here) / A12 (DXF + PDF,
// written against the same interface so the Export centre needs one code
// path).
//
// Contract notes fixed here so A9 and the app UIs can be built in parallel:
//   • Export always reads from the cloud/PageStore (pages are single-stream,
//     so per-session provenance survives into a merged export — A13).
//   • Progress is reported through the callback threaded through
//     export_points()/Exporter — jobs/ (A15) is the thing that turns that
//     into an EventType::kJobProgress event; A9 does not touch the event bus
//     directly (export/ has no engine back-pointer, per DESIGN.md §6.3).
//   • A cancelled export deletes its partial output file.
//   • Georeferenced formats (LAS) take the CRS as a caller-supplied WKT
//     string (`ExportOptions::crs_wkt`), NOT from the session manifest —
//     export/ only sees a PageStore, which carries no georeferencing state.
//     That means the "is this session georeferenced" decision is the
//     caller's (job layer / A10's CRS picker), not this module's: pass a
//     real WKT for a georeferenced session, or leave it empty and the LAS
//     writer embeds a clearly-labelled LOCAL/UNGEOREFERENCED placeholder
//     WKT (see export/las_constants.h's kLasLocalFramePlaceholderWkt)
//     rather than silently writing local coordinates under a real-looking
//     CRS tag or refusing to export local data at all. See
//     docs/A9-export.md §"CRS seam" for the full rationale and what A10
//     needs to change (nothing — just start passing crs_wkt).
//
// PointVertex (cloud/point_page.h) carries position + RGBA8 and NO separate
// intensity channel today. Until one exists, every A9 writer derives
// per-point intensity from RGB luminance (see export/point_source.h) when
// `include_intensity` is set. This is a documented bridge, not a permanent
// design: the D6 driver currently writes raw intensity into R/G/B (see
// src/drivers/d6/d6_driver.cpp), so the derived value is exact for
// unmodified D6 captures and becomes an approximation once real camera
// colorization (A11) overwrites RGB. A real per-point intensity channel is
// an A1/A14 decision (extend PointVertex or add a parallel buffer); A9's
// writers are structured so that swapping the source in export/point_source.h
// is the only change needed when it lands.
#ifndef SCANENGINE_EXPORT_EXPORTER_H
#define SCANENGINE_EXPORT_EXPORTER_H

#include <atomic>
#include <cstdint>
#include <string>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/core/types.h"

namespace scanengine {

enum class ExportFormat : std::uint8_t {
  kPlyBinary = 0,  // A9
  kLas14 = 1,      // A9 (georeferenced)
  kPcd = 2,        // A9
  kDxf = 3,        // A12 (floor plan)
  kPdf = 4,        // A12 (floor plan sheet)
};

// An axis-aligned filter applied in the session's local metric frame (the
// same frame PointVertex is in). Points strictly outside [min, max] on any
// axis are dropped. `enabled = false` (the default) exports everything.
struct ExportBoundsFilter {
  bool enabled = false;
  float min[3] = {0.f, 0.f, 0.f};
  float max[3] = {0.f, 0.f, 0.f};
};

// Called with a monotonically non-decreasing fraction in [0, 1]. May be
// invoked from whatever thread calls export_points()/Exporter::write() —
// there is no thread of export/'s own (DESIGN.md §2: "the engine owns no
// threads" until a task needs one, and A9 doesn't).
using ExportProgressCallback = void (*)(float fraction, void* user_data);

// A tiny cooperative cancellation flag. The caller (typically jobs/, A15)
// owns the instance and calls request_cancel() from any thread; the writer
// polls cancelled() between points/pages and, on seeing it set, stops
// writing, deletes the partial output file, and returns
// ScanError::kCancelled. Deliberately not a callback: a flag can be polled
// cheaply from a tight per-point loop, and it can be set from a thread that
// is not the one blocked inside write().
class ExportCancelToken {
 public:
  void request_cancel() noexcept { flag_.store(true, std::memory_order_release); }
  bool cancelled() const noexcept { return flag_.load(std::memory_order_acquire); }

 private:
  std::atomic<bool> flag_{false};
};

struct ExportOptions {
  // --- original A1 seam fields (kept for source/ABI stability — see
  // tests/test_headers.cpp, which is not A9's file to edit) -----------------
  ExportFormat format = ExportFormat::kPlyBinary;
  std::string output_path;
  bool include_color = true;
  bool include_intensity = true;
  std::string crs_epsg;      // e.g. "EPSG:32633"; empty = local frame.
                              // Superseded by crs_wkt below when crs_wkt is
                              // non-empty; kept so a caller that only has an
                              // EPSG code can still express one (A9 does not
                              // resolve EPSG -> WKT itself — that is A10's
                              // CRS/EPSG machinery — so a bare crs_epsg with
                              // no crs_wkt still produces the placeholder
                              // WKT, with crs_epsg carried through into the
                              // VLR description field so the file at least
                              // records intent for a human/tool to fix up).
  double voxel_size_m = 0.0; // 0 = no downsampling (voxel dedup is A13/A14's
                              // job; A9 does not implement it — see
                              // docs/A9-export.md).

  // --- A9 additions -----------------------------------------------------
  std::string crs_wkt;               // OGC WKT (1 or 2). Seam for A10's real
                                      // CRS string. Empty => LAS embeds the
                                      // documented local/ungeoreferenced
                                      // placeholder (export/las_writer.h).
  ExportBoundsFilter bounds_filter;  // default: disabled (export everything)
  std::uint32_t decimate = 1;        // keep 1 of every N points; 0 and 1 both
                                      // mean "no decimation".

  // LAS only: false -> point format 2 (RGB, 26 bytes/point, LAS 1.2+
  // compatible). true -> point format 7 (RGB + GPS time, 36 bytes/point,
  // LAS 1.4-only). PageStore has no true per-point GPS time (append() takes
  // one timestamp per batch, not per point — see cloud/page_store.h), so
  // format 7's GPS Time field is linearly interpolated across each page
  // between its t_first_ns/t_last_ns, in engine-monotonic seconds, NOT a
  // real GPS time base. That is enough to prove the format's structure is
  // correct and to carry relative timing; A10/A4 are the seam for a real
  // GPS time source. See docs/A9-export.md.
  bool las_gps_time = false;
};

class Exporter {
 public:
  virtual ~Exporter() = default;
  virtual ExportFormat format() const = 0;
  virtual Status write(const PageStore& points, const ExportOptions& opts) = 0;
  virtual float progress() const = 0;
  virtual void cancel() = 0;
};

// A9 implements this factory for kPlyBinary/kLas14/kPcd; kDxf/kPdf (A12) and
// any other value return kUnimplemented via Result's ScanError. The caller
// owns the returned Exporter* and must `delete` it (no smart-pointer return
// type, to match the rest of the engine's raw-pointer factory conventions).
Result<Exporter*> make_exporter(ExportFormat format);

// The one entry point jobs/ (A15) calls. Streams a PageStore straight to
// disk in `format` without ever materializing the whole cloud in memory:
// each format's writer makes two passes over the *pages already resident in
// `store`* (no extra copy) — one to total the selected point count (and, for
// LAS, the exact post-filter/decimate bounds the header's scale/offset are
// chosen from), one to write — so header fields that must precede the point
// data (element/point counts, LAS bounds) are exact, not estimated.
//
//   store    — points already decoded into the cloud (drivers, SLAM, color).
//   streams  — which StreamId's pages to include; empty = every page in
//              `store`, regardless of stream (this is what a "single merged
//              export" of a multi-sensor session wants).
//   format   — kPlyBinary / kLas14 / kPcd (kDxf/kPdf go through A12's own
//              writer, not this function).
//   path     — output file path. On success the file at `path` is complete;
//              on failure or cancellation, nothing is left at `path` (a
//              partial file is removed).
//   options  — see ExportOptions above.
//   progress_cb/progress_user_data — optional; called during the write pass
//              only (the count/bounds pass is fast and not reported).
//   cancel_token — optional; poll-based cancellation, see ExportCancelToken.
//
// Returns kInvalidArgument for an empty path or a store/format mismatch that
// makes no sense (there is none today, but the seam is here), kUnimplemented
// for kDxf/kPdf, kCancelled if cancel_token->cancelled() was observed, and
// kIoError for a filesystem failure. An empty selection (0 points after
// streams/bounds/decimate filtering) is NOT an error: a valid empty-cloud
// file is written.
Status export_points(const PageStore& store, Span<const StreamId> streams, ExportFormat format,
                      const std::string& path, const ExportOptions& options,
                      ExportProgressCallback progress_cb = nullptr,
                      void* progress_user_data = nullptr,
                      ExportCancelToken* cancel_token = nullptr);

}  // namespace scanengine

#endif  // SCANENGINE_EXPORT_EXPORTER_H
