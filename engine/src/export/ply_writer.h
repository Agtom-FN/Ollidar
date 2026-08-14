// ply_writer.h — binary_little_endian PLY writer (A9).
#ifndef SCANENGINE_SRC_EXPORT_PLY_WRITER_H
#define SCANENGINE_SRC_EXPORT_PLY_WRITER_H

#include <string>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/export/exporter.h"

namespace scanengine::exportimpl {

Status write_ply(const PageStore& store, Span<const StreamId> streams, const std::string& path,
                  const ExportOptions& opts, ExportProgressCallback progress_cb,
                  void* progress_user_data, ExportCancelToken* cancel_token);

}  // namespace scanengine::exportimpl

#endif  // SCANENGINE_SRC_EXPORT_PLY_WRITER_H
