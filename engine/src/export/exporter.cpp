// exporter.cpp — export_points() dispatch + the Exporter interface
// implementations, all delegating to the per-format writers so there is one
// code path whether a caller uses the free function or the interface (the
// interface exists for callers that want to poll progress()/call cancel()
// on an object rather than thread an ExportCancelToken through themselves).
#include "scanengine/export/exporter.h"

#include <atomic>
#include <vector>

#include "las_writer.h"
#include "pcd_writer.h"
#include "ply_writer.h"
#include "scanengine/core/error.h"

namespace scanengine {

Status export_points(const PageStore& store, Span<const StreamId> streams, ExportFormat format,
                      const std::string& path, const ExportOptions& options,
                      ExportProgressCallback progress_cb, void* progress_user_data,
                      ExportCancelToken* cancel_token) {
  if (path.empty()) {
    return Status(set_last_error(ScanError::kInvalidArgument, "export_points: empty output path"));
  }
  switch (format) {
    case ExportFormat::kPlyBinary:
      return exportimpl::write_ply(store, streams, path, options, progress_cb, progress_user_data,
                                    cancel_token);
    case ExportFormat::kLas14:
      return exportimpl::write_las14(store, streams, path, options, progress_cb,
                                      progress_user_data, cancel_token);
    case ExportFormat::kPcd:
      return exportimpl::write_pcd(store, streams, path, options, progress_cb, progress_user_data,
                                    cancel_token);
    case ExportFormat::kDxf:
    case ExportFormat::kPdf:
    default:
      return Status(set_last_error(ScanError::kUnimplemented,
                                    "export_points: format not implemented by A9 "
                                    "(A12 owns DXF/PDF)"));
  }
}

namespace {

// Adapts export_points()'s function-pointer progress callback + explicit
// cancel token onto the Exporter interface's progress()/cancel() polling
// model. One tiny base class so kPlyBinary/kLas14/kPcd only differ in which
// ExportFormat/writer they name.
class StreamingExporter : public Exporter {
 public:
  explicit StreamingExporter(ExportFormat fmt) : format_(fmt) {}

  ExportFormat format() const override { return format_; }

  Status write(const PageStore& points, const ExportOptions& opts) override {
    progress_.store(0.f, std::memory_order_relaxed);
    const std::vector<StreamId> all_streams;  // empty = every stream in `points`
    return export_points(points, Span<const StreamId>(all_streams.data(), all_streams.size()),
                          format_, opts.output_path, opts, &progress_trampoline, this, &cancel_);
  }

  float progress() const override { return progress_.load(std::memory_order_relaxed); }
  void cancel() override { cancel_.request_cancel(); }

 private:
  static void progress_trampoline(float fraction, void* user_data) {
    static_cast<StreamingExporter*>(user_data)->progress_.store(fraction,
                                                                  std::memory_order_relaxed);
  }

  ExportFormat format_;
  std::atomic<float> progress_{0.f};
  ExportCancelToken cancel_;
};

}  // namespace

Result<Exporter*> make_exporter(ExportFormat format) {
  switch (format) {
    case ExportFormat::kPlyBinary:
    case ExportFormat::kLas14:
    case ExportFormat::kPcd:
      return new StreamingExporter(format);
    case ExportFormat::kDxf:
    case ExportFormat::kPdf:
    default:
      return set_last_error(ScanError::kUnimplemented,
                             "make_exporter: format not implemented by A9 (A12 owns DXF/PDF)");
  }
}

}  // namespace scanengine
