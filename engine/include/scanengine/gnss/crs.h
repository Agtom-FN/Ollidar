// crs.h — coordinate reference systems, hand-rolled (Tech Spec §3.4).
//
// "CRS support: EPSG picker (survey profile), WGS84/UTM defaults, coarse
// EGM96 geoid bundled." This file is the whole geodesy layer: geodetic ↔
// ECEF ↔ local ENU, UTM zone selection and projection, EPSG numbering, and
// the OGC WKT / PROJ strings that A9's `ExportOptions::crs_wkt` seam wants.
//
// **No PROJ, no GeographicLib, no new vcpkg port.** Same reasoning A8 wrote
// down for declining Ceres (docs/A8-pushbroom.md §2), applied here: every
// port has to build on five CI legs including the macOS universal overlay
// triplet and the Android NDK, PROJ drags SQLite plus a ~10 MB proj.db grid
// database that would have to be shipped inside the Android APK, and the
// mathematics we actually need is four closed-form transforms and one
// Krüger series. §4 of docs/A10-gnss.md states the measured accuracy of each
// one and the bound beyond which this decision has to be revisited. Short
// version:
//
//   * geodetic ↔ ECEF        exact to ~1e-9 m (closed form + Bowring)
//   * ECEF ↔ ENU             exact (a rotation and a translation)
//   * geodetic ↔ UTM         < 1 mm within a zone, < 1 cm to ±6° of the
//                            central meridian; NOT usable beyond ~±10°
//   * geoid                  NOT modelled here — see GeoidModel below
//
// Naming: `Geodetic::height_m` is ELLIPSOIDAL height throughout. GGA reports
// orthometric (MSL) height plus a geoid separation, and `GnssFix` carries
// both; mixing them is the classic 30-metre georeferencing bug, so the type
// names say which is which.
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_CRS_H
#define SCANENGINE_GNSS_CRS_H

#include <cmath>
#include <cstdint>
#include <string>

namespace scanengine {
namespace crs {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeg = kPi / 180.0;
inline constexpr double kRad = 180.0 / kPi;

struct Ellipsoid {
  double a = 6378137.0;                 // semi-major axis, m
  double inv_f = 298.257223563;         // inverse flattening

  double f() const { return 1.0 / inv_f; }
  double b() const { return a * (1.0 - f()); }
  double e2() const { return f() * (2.0 - f()); }          // first eccentricity²
  double ep2() const { return e2() / (1.0 - e2()); }       // second eccentricity²
  double n() const { return f() / (2.0 - f()); }           // third flattening
};

inline constexpr Ellipsoid kWgs84{6378137.0, 298.257223563};
// Clarke 1866 is here for exactly one reason: Snyder's published Transverse
// Mercator worked example (Map Projections — A Working Manual, USGS
// Professional Paper 1395, pp. 269–270) uses it, and that example is the
// external reference `test_gnss.cpp` validates the projection against.
inline constexpr Ellipsoid kClarke1866{6378206.4, 294.9786982};

struct Geodetic {
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  double height_m = 0.0;  // ELLIPSOIDAL
};

struct Ecef {
  double x = 0.0, y = 0.0, z = 0.0;
};

struct Enu {
  double e = 0.0, n = 0.0, u = 0.0;
};

// --- geodetic ↔ ECEF -------------------------------------------------------

Ecef geodetic_to_ecef(const Geodetic& g, const Ellipsoid& el = kWgs84) noexcept;

// Bowring's 1985 method: one iteration of the parametric-latitude fixed
// point, which is accurate to well under a micrometre for heights from
// −10 km to +30 km. Falls back to a bounded Newton iteration near the polar
// axis where Bowring's tangent form is ill-conditioned.
Geodetic ecef_to_geodetic(const Ecef& p, const Ellipsoid& el = kWgs84) noexcept;

// --- local ENU tangent frame ----------------------------------------------
//
// The session's local metric frame. `origin` is where the SLAM/local frame's
// (0,0,0) sits; every georeferenced coordinate the engine hands out is
// metres east/north/up from it. This is exact — no projection distortion —
// which is why the ENU frame, not UTM, is the engine's internal global
// frame. UTM is applied once, at export, by to_utm().
struct EnuFrame {
  Geodetic origin{};
  Ecef origin_ecef{};
  double ecef_from_enu[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major 3x3
  bool valid = false;
};

EnuFrame make_enu_frame(const Geodetic& origin, const Ellipsoid& el = kWgs84) noexcept;

Enu ecef_to_enu(const EnuFrame& f, const Ecef& p) noexcept;
Ecef enu_to_ecef(const EnuFrame& f, const Enu& p) noexcept;
Enu geodetic_to_enu(const EnuFrame& f, const Geodetic& g,
                    const Ellipsoid& el = kWgs84) noexcept;
Geodetic enu_to_geodetic(const EnuFrame& f, const Enu& p,
                         const Ellipsoid& el = kWgs84) noexcept;

// Metres per degree at a latitude — used for sanity checks and for the
// "how far is this fix from the origin" gate, not for coordinates.
double meters_per_deg_lat(double lat_deg, const Ellipsoid& el = kWgs84) noexcept;
double meters_per_deg_lon(double lat_deg, const Ellipsoid& el = kWgs84) noexcept;

// --- Transverse Mercator / UTM --------------------------------------------

struct TmParams {
  double lon0_deg = 0.0;      // central meridian
  double lat0_deg = 0.0;      // latitude of origin
  double k0 = 0.9996;
  double false_easting = 500000.0;
  double false_northing = 0.0;
};

// Meridian arc distance from the equator to `lat_rad`, from the standard
// series in the third flattening n. Exposed because the test validates it
// against a numerical integration of the meridian radius of curvature — an
// independent check of the projection's dominant term that needs no external
// data.
double meridian_arc(const Ellipsoid& el, double lat_rad) noexcept;

// Krüger series, 6th order in n. `k` (point scale factor) and `gamma`
// (grid convergence, degrees) are optional outputs; pass nullptr to skip.
void tm_forward(const Ellipsoid& el, const TmParams& p, double lat_deg, double lon_deg,
                double* x, double* y, double* k, double* gamma_deg) noexcept;
void tm_inverse(const Ellipsoid& el, const TmParams& p, double x, double y,
                double* lat_deg, double* lon_deg) noexcept;

struct UtmZone {
  int zone = 0;        // 1..60; 0 = invalid
  bool north = true;

  bool valid() const { return zone >= 1 && zone <= 60; }
};

// Standard zone from longitude, with the two ISO 19111 irregularities that
// matter to anybody working in Europe: zone 32 is widened over south-west
// Norway, and Svalbard uses 31/33/35/37. Getting these wrong puts a Norwegian
// survey in the wrong zone, which is a silently-30-km-off failure.
UtmZone utm_zone_for(double lat_deg, double lon_deg) noexcept;

TmParams utm_params(UtmZone z) noexcept;

struct UtmCoord {
  double easting = 0.0;
  double northing = 0.0;
  int zone = 0;
  bool north = true;
  double convergence_deg = 0.0;  // grid north minus true north
  double scale = 1.0;            // point scale factor
};

UtmCoord geodetic_to_utm(const Geodetic& g, const Ellipsoid& el = kWgs84) noexcept;
UtmCoord geodetic_to_utm_zone(const Geodetic& g, UtmZone z,
                              const Ellipsoid& el = kWgs84) noexcept;
Geodetic utm_to_geodetic(const UtmCoord& c, double height_m = 0.0,
                         const Ellipsoid& el = kWgs84) noexcept;

// --- EPSG / WKT / PROJ ----------------------------------------------------

inline constexpr int kEpsgWgs84Geographic2D = 4326;
inline constexpr int kEpsgWgs84Geographic3D = 4979;
inline constexpr int kEpsgWgs84Geocentric = 4978;
inline constexpr int kEpsgEgm96Height = 5773;

// WGS 84 / UTM zone NN N|S: 32600 + zone (north), 32700 + zone (south).
int utm_epsg(UtmZone z) noexcept;
UtmZone utm_zone_from_epsg(int epsg) noexcept;

// "EPSG:32633"
std::string epsg_string(int epsg);
// "EPSG:32633" -> 32633; 0 when it is not an EPSG string.
int parse_epsg_string(const std::string& s) noexcept;

// OGC WKT1, hand-written to match the EPSG registry's text for these CRSs.
// WKT1 rather than WKT2 on purpose: the LAS 1.4 spec's OGC-WKT VLR predates
// WKT2, and CloudCompare / QGIS / LAStools / PDAL all read WKT1 without
// argument, whereas WKT2 support in the older readers is patchy. A9's local
// placeholder is WKT2 ENGCRS precisely so it FAILS to reproject; a real CRS
// must succeed everywhere, so it gets the conservative dialect.
std::string utm_wkt1(UtmZone z);
std::string wgs84_geographic_wkt1();
// Dispatch on an EPSG code: 4326 / 4979 and the two UTM ranges. Empty string
// for anything else — the caller then has to supply its own WKT, which is the
// survey-profile "EPSG picker" escape hatch.
std::string wkt1_for_epsg(int epsg);

// "+proj=utm +zone=33 +datum=WGS84 +units=m +no_defs +type=crs"
std::string utm_proj_string(UtmZone z);
std::string proj_string_for_epsg(int epsg);

// Human label, e.g. "WGS 84 / UTM zone 33N".
std::string crs_name_for_epsg(int epsg);

// --- geoid ----------------------------------------------------------------
//
// §3.4 says "coarse EGM96 geoid bundled". A10 ships the SEAM and no grid,
// and that is a considered decision, not an omission:
//
//   * The receiver already carries one. GGA field 11 is the geoid separation
//     the rover's own (usually EGM96) model computed for this exact position,
//     and `GnssFix` keeps it. When the rover reports it — every F9P and
//     Emlid does — an engine-side model adds nothing but a second opinion.
//   * A grid coarse enough to embed in source is worse than useless. EGM96
//     undulation spans −105 … +85 m; a 15° lattice interpolates to several
//     metres of error, which would silently corrupt the vertical of a survey
//     whose horizontal is good to 2 cm.
//   * A real 15'×15' EGM96 grid is a ~2 MB data asset with an install rule
//     in a CMakeLists this task does not own.
//
// So: `ConstantGeoidModel` for a site with a known local undulation (the
// honest configuration for a single-site survey), the rover's own value when
// present, and a documented seam for the Phase-2 grid.
class GeoidModel {
 public:
  virtual ~GeoidModel() = default;
  virtual const char* name() const = 0;
  // Geoid–ellipsoid separation N in metres, such that
  //   h_ellipsoidal = H_orthometric + N.
  // False when the model cannot answer for this position.
  virtual bool undulation(double lat_deg, double lon_deg, double* n_m) const = 0;
};

class ConstantGeoidModel final : public GeoidModel {
 public:
  explicit ConstantGeoidModel(double n_m) : n_(n_m) {}
  const char* name() const override { return "constant"; }
  bool undulation(double, double, double* n_m) const override {
    if (n_m) *n_m = n_;
    return true;
  }

 private:
  double n_;
};

}  // namespace crs
}  // namespace scanengine

#endif  // SCANENGINE_GNSS_CRS_H
