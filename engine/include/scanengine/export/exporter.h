// exporter.h — point-cloud and document export writers.
//
// SEAM ONLY. Owner: A9 (PLY binary+RGB, LAS 1.4 georeferenced, PCD, plus
// reference-reader tests) / A12 (DXF + PDF, written against the same
// interface so the Export centre needs one code path).
//
// Contract notes fixed here so A9 and the app UIs can be built in parallel:
//   • Export always reads from the cloud/PageStore (pages are single-stream,
//     so per-session provenance survives into a merged export — A13).
//   • Progress is reported through EventType::kJobProgress, never by
//     blocking a UI thread; a cancelled export deletes its partial file.
//   • Georeferenced formats take the CRS from the session manifest; if the
//     session is not georeferenced, LAS export must fail with
//     kInvalidState rather than silently writing local coordinates.
#ifndef SCANENGINE_EXPORT_EXPORTER_H
#define SCANENGINE_EXPORT_EXPORTER_H

#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"

namespace scanengine {

enum class ExportFormat : std::uint8_t {
  kPlyBinary = 0,  // A9
  kLas14 = 1,      // A9 (georeferenced)
  kPcd = 2,        // A9
  kDxf = 3,        // A12 (floor plan)
  kPdf = 4,        // A12 (floor plan sheet)
};

struct ExportOptions {
  ExportFormat format = ExportFormat::kPlyBinary;
  std::string output_path;
  bool include_color = true;
  bool include_intensity = true;
  std::string crs_epsg;      // e.g. "EPSG:32633"; empty = local frame
  double voxel_size_m = 0.0; // 0 = no downsampling
};

class Exporter {
 public:
  virtual ~Exporter() = default;
  virtual ExportFormat format() const = 0;
  virtual Status write(const PageStore& points, const ExportOptions& opts) = 0;
  virtual float progress() const = 0;
  virtual void cancel() = 0;
};

// A9 implements this factory; unknown formats return kUnimplemented.
Result<Exporter*> make_exporter(ExportFormat format);

}  // namespace scanengine

#endif  // SCANENGINE_EXPORT_EXPORTER_H
