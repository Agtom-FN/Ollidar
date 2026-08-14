// las_constants.h — LAS 1.4 layout constants and the georeferencing
// placeholder, public so a UI/job layer can show or check them without
// pulling in the writer implementation.
//
// Owner: A9. See exporter.h's top comment and docs/A9-export.md ("CRS seam")
// for why an empty ExportOptions::crs_wkt gets kLasLocalFramePlaceholderWkt
// instead of a refusal to export.
#ifndef SCANENGINE_EXPORT_LAS_CONSTANTS_H
#define SCANENGINE_EXPORT_LAS_CONSTANTS_H

#include <cstddef>
#include <cstdint>

namespace scanengine {

// LAS 1.4 Public Header Block size, bytes. Fixed by the format.
inline constexpr std::size_t kLas14HeaderSize = 375;

// Point Data Record Formats A9 writes.
//   2 — RGB, no GPS time (26 bytes/point). Readable by any LAS 1.2+ tool.
//   7 — RGB + GPS time (36 bytes/point). LAS 1.4-only "extended" format;
//       this is the "RGB+time-capable" format from the A9 task brief.
// Selected via ExportOptions::las_gps_time.
inline constexpr std::uint8_t kLasPointFormat2 = 2;
inline constexpr std::uint8_t kLasPointFormat7 = 7;
inline constexpr std::uint16_t kLasPointFormat2RecordLen = 26;
inline constexpr std::uint16_t kLasPointFormat7RecordLen = 36;

// The VLR that carries the CRS as OGC WKT. Global encoding bit 4 set means
// "CRS is WKT, not legacy GeoTIFF keys" (LAS 1.4 spec; mandatory for point
// formats 6-10, and what A9 always uses).
inline constexpr const char* kLasWktVlrUserId = "LASF_Projection";
inline constexpr std::uint16_t kLasWktVlrRecordId = 2112;
inline constexpr std::uint16_t kLasGlobalEncodingWktBit = 1u << 4;

// Embedded when ExportOptions::crs_wkt is empty: the caller did not supply a
// real CRS (session isn't georeferenced yet, or A10 hasn't wired the CRS
// picker's output through to export options). A9 does not refuse to export
// in this case (an ungeoreferenced local-frame LAS is still useful — it is
// what a bench/lab capture with no GNSS produces). Deliberately an ENGCRS
// (Engineering CRS, OGC WKT2) so nothing downstream — CloudCompare, QGIS,
// PDAL — mistakes these coordinates for a real-world CRS: an ENGCRS has no
// datum/ellipsoid, so a tool that tries to reproject it fails loudly instead
// of producing silently-wrong geography.
inline constexpr const char* kLasLocalFramePlaceholderWkt =
    "ENGCRS[\"LidarScan Local/Ungeoreferenced Frame\","
    "EDATUM[\"LidarScan session origin\"],"
    "CS[Cartesian,3],"
    "AXIS[\"x\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
    "AXIS[\"y\",north,ORDER[2],LENGTHUNIT[\"metre\",1]],"
    "AXIS[\"z\",up,ORDER[3],LENGTHUNIT[\"metre\",1]],"
    "REMARK[\"No GNSS/RTK fix at export time -- coordinates are the "
    "engine's local metric capture frame, NOT a real-world CRS. Pass "
    "ExportOptions::crs_wkt for a georeferenced export (see A10).\"]]";

}  // namespace scanengine

#endif  // SCANENGINE_EXPORT_LAS_CONSTANTS_H
