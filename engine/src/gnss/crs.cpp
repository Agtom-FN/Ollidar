#include "scanengine/gnss/crs.h"

#include <cstdio>

namespace scanengine {
namespace crs {
namespace {

double sq(double x) { return x * x; }

}  // namespace

// --- geodetic ↔ ECEF -------------------------------------------------------

Ecef geodetic_to_ecef(const Geodetic& g, const Ellipsoid& el) noexcept {
  const double lat = g.lat_deg * kDeg;
  const double lon = g.lon_deg * kDeg;
  const double sl = std::sin(lat), cl = std::cos(lat);
  const double e2 = el.e2();
  const double N = el.a / std::sqrt(1.0 - e2 * sl * sl);  // prime vertical radius
  Ecef p;
  p.x = (N + g.height_m) * cl * std::cos(lon);
  p.y = (N + g.height_m) * cl * std::sin(lon);
  p.z = (N * (1.0 - e2) + g.height_m) * sl;
  return p;
}

Geodetic ecef_to_geodetic(const Ecef& p, const Ellipsoid& el) noexcept {
  Geodetic g;
  const double a = el.a, b = el.b(), e2 = el.e2(), ep2 = el.ep2();
  const double r = std::sqrt(p.x * p.x + p.y * p.y);
  g.lon_deg = std::atan2(p.y, p.x) * kRad;

  if (r < 1e-9) {
    // On the polar axis: Bowring's tangent form is singular here.
    g.lat_deg = p.z >= 0.0 ? 90.0 : -90.0;
    g.height_m = std::fabs(p.z) - b;
    return g;
  }

  // Bowring (1985): one pass of the parametric-latitude fixed point. The
  // published bound is < 1e-4 arcsec (≈ 3 µm) for heights in −10 km … +30 km,
  // and test_gnss.cpp measures 1e-9 m over the range this product sees.
  double lat;
  const double theta = std::atan2(p.z * a, r * b);
  const double st = std::sin(theta), ct = std::cos(theta);
  lat = std::atan2(p.z + ep2 * b * st * st * st, r - e2 * a * ct * ct * ct);

  // One Newton refinement. Cheap, and it removes the residual entirely for
  // the extreme heights (aircraft, and the −6371 km "centre of the earth"
  // degenerate input a fuzzer will eventually hand us).
  for (int i = 0; i < 2; ++i) {
    const double sl = std::sin(lat);
    const double N = a / std::sqrt(1.0 - e2 * sl * sl);
    const double h = r / std::cos(lat) - N;
    const double next = std::atan2(p.z, r * (1.0 - e2 * N / (N + h)));
    if (std::fabs(next - lat) < 1e-14) {
      lat = next;
      break;
    }
    lat = next;
  }

  const double sl = std::sin(lat);
  const double N = a / std::sqrt(1.0 - e2 * sl * sl);
  // Away from the poles use r/cos(lat); near them use z/sin(lat), which is
  // the well-conditioned branch there.
  if (std::fabs(lat) < 1.0) {
    g.height_m = r / std::cos(lat) - N;
  } else {
    g.height_m = p.z / sl - N * (1.0 - e2);
  }
  g.lat_deg = lat * kRad;
  return g;
}

// --- ENU -------------------------------------------------------------------

EnuFrame make_enu_frame(const Geodetic& origin, const Ellipsoid& el) noexcept {
  EnuFrame f;
  f.origin = origin;
  f.origin_ecef = geodetic_to_ecef(origin, el);
  const double lat = origin.lat_deg * kDeg, lon = origin.lon_deg * kDeg;
  const double sla = std::sin(lat), cla = std::cos(lat);
  const double slo = std::sin(lon), clo = std::cos(lon);
  // Columns are the ENU basis vectors expressed in ECEF; row-major storage.
  f.ecef_from_enu[0] = -slo;  f.ecef_from_enu[1] = -sla * clo;  f.ecef_from_enu[2] = cla * clo;
  f.ecef_from_enu[3] =  clo;  f.ecef_from_enu[4] = -sla * slo;  f.ecef_from_enu[5] = cla * slo;
  f.ecef_from_enu[6] =  0.0;  f.ecef_from_enu[7] =  cla;        f.ecef_from_enu[8] = sla;
  f.valid = true;
  return f;
}

Enu ecef_to_enu(const EnuFrame& f, const Ecef& p) noexcept {
  const double dx = p.x - f.origin_ecef.x;
  const double dy = p.y - f.origin_ecef.y;
  const double dz = p.z - f.origin_ecef.z;
  // Transpose of ecef_from_enu.
  Enu q;
  q.e = f.ecef_from_enu[0] * dx + f.ecef_from_enu[3] * dy + f.ecef_from_enu[6] * dz;
  q.n = f.ecef_from_enu[1] * dx + f.ecef_from_enu[4] * dy + f.ecef_from_enu[7] * dz;
  q.u = f.ecef_from_enu[2] * dx + f.ecef_from_enu[5] * dy + f.ecef_from_enu[8] * dz;
  return q;
}

Ecef enu_to_ecef(const EnuFrame& f, const Enu& p) noexcept {
  Ecef q;
  q.x = f.origin_ecef.x + f.ecef_from_enu[0] * p.e + f.ecef_from_enu[1] * p.n +
        f.ecef_from_enu[2] * p.u;
  q.y = f.origin_ecef.y + f.ecef_from_enu[3] * p.e + f.ecef_from_enu[4] * p.n +
        f.ecef_from_enu[5] * p.u;
  q.z = f.origin_ecef.z + f.ecef_from_enu[6] * p.e + f.ecef_from_enu[7] * p.n +
        f.ecef_from_enu[8] * p.u;
  return q;
}

Enu geodetic_to_enu(const EnuFrame& f, const Geodetic& g, const Ellipsoid& el) noexcept {
  return ecef_to_enu(f, geodetic_to_ecef(g, el));
}

Geodetic enu_to_geodetic(const EnuFrame& f, const Enu& p, const Ellipsoid& el) noexcept {
  return ecef_to_geodetic(enu_to_ecef(f, p), el);
}

double meters_per_deg_lat(double lat_deg, const Ellipsoid& el) noexcept {
  const double lat = lat_deg * kDeg;
  const double e2 = el.e2();
  const double w = 1.0 - e2 * sq(std::sin(lat));
  const double M = el.a * (1.0 - e2) / (w * std::sqrt(w));  // meridian radius
  return M * kDeg;
}

double meters_per_deg_lon(double lat_deg, const Ellipsoid& el) noexcept {
  const double lat = lat_deg * kDeg;
  const double e2 = el.e2();
  const double N = el.a / std::sqrt(1.0 - e2 * sq(std::sin(lat)));
  return N * std::cos(lat) * kDeg;
}

// --- Transverse Mercator ---------------------------------------------------
//
// Krüger's series in the third flattening n, 6th order. Coefficients are the
// standard ones (Karney 2011 eq. 35, also in the OSGB and JHS 154 guidance
// notes). At UTM's ±3° half-zone the truncation error is far below a
// micrometre; docs/A10-gnss.md §4 states the measured bound out to ±10°.

namespace {

struct KrugerCoeffs {
  double alpha[7];  // 1-based, [0] unused: forward (geodetic → projected)
  double beta[7];   // 1-based, [0] unused: inverse
  double A;         // rectifying radius × a
};

KrugerCoeffs kruger(const Ellipsoid& el) {
  const double n = el.n();
  const double n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
  KrugerCoeffs c{};
  c.A = el.a / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0 + n6 / 256.0);
  c.alpha[1] = n / 2.0 - 2.0 / 3.0 * n2 + 5.0 / 16.0 * n3 + 41.0 / 180.0 * n4 -
               127.0 / 288.0 * n5 + 7891.0 / 37800.0 * n6;
  c.alpha[2] = 13.0 / 48.0 * n2 - 3.0 / 5.0 * n3 + 557.0 / 1440.0 * n4 +
               281.0 / 630.0 * n5 - 1983433.0 / 1935360.0 * n6;
  c.alpha[3] = 61.0 / 240.0 * n3 - 103.0 / 140.0 * n4 + 15061.0 / 26880.0 * n5 +
               167603.0 / 181440.0 * n6;
  c.alpha[4] = 49561.0 / 161280.0 * n4 - 179.0 / 168.0 * n5 + 6601661.0 / 7257600.0 * n6;
  c.alpha[5] = 34729.0 / 80640.0 * n5 - 3418889.0 / 1995840.0 * n6;
  c.alpha[6] = 212378941.0 / 319334400.0 * n6;

  c.beta[1] = n / 2.0 - 2.0 / 3.0 * n2 + 37.0 / 96.0 * n3 - 1.0 / 360.0 * n4 -
              81.0 / 512.0 * n5 + 96199.0 / 604800.0 * n6;
  c.beta[2] = 1.0 / 48.0 * n2 + 1.0 / 15.0 * n3 - 437.0 / 1440.0 * n4 +
              46.0 / 105.0 * n5 - 1118711.0 / 3870720.0 * n6;
  c.beta[3] = 17.0 / 480.0 * n3 - 37.0 / 840.0 * n4 - 209.0 / 4480.0 * n5 +
              5569.0 / 90720.0 * n6;
  c.beta[4] = 4397.0 / 161280.0 * n4 - 11.0 / 504.0 * n5 - 830251.0 / 7257600.0 * n6;
  c.beta[5] = 4583.0 / 161280.0 * n5 - 108847.0 / 3991680.0 * n6;
  c.beta[6] = 20648693.0 / 638668800.0 * n6;
  return c;
}

}  // namespace

double meridian_arc(const Ellipsoid& el, double lat_rad) noexcept {
  const KrugerCoeffs c = kruger(el);
  // Conformal-to-rectifying: the same series the projection uses, evaluated
  // on the central meridian, which is exactly the meridian arc × k0=1.
  const double e = std::sqrt(el.e2());
  const double sl = std::sin(lat_rad);
  const double t = std::sinh(std::atanh(sl) - e * std::atanh(e * sl));
  const double xi = std::atan(t / 1.0);  // ξ' with η' = 0 on the meridian
  double s = xi;
  for (int j = 1; j <= 6; ++j) s += c.alpha[j] * std::sin(2.0 * j * xi);
  return c.A * s;
}

void tm_forward(const Ellipsoid& el, const TmParams& p, double lat_deg, double lon_deg,
                double* x, double* y, double* k, double* gamma_deg) noexcept {
  const KrugerCoeffs c = kruger(el);
  const double e = std::sqrt(el.e2());
  const double phi = lat_deg * kDeg;
  double dlon = (lon_deg - p.lon0_deg);
  while (dlon > 180.0) dlon -= 360.0;
  while (dlon < -180.0) dlon += 360.0;
  const double lam = dlon * kDeg;

  const double sphi = std::sin(phi), cphi = std::cos(phi);
  // tau' = tan of the CONFORMAL latitude, via the isometric latitude
  // (Karney 2011 eq. 7/9). This is the one place the ellipsoid enters before
  // the series.
  const double taup = std::sinh(std::atanh(sphi) - e * std::atanh(e * sphi));
  const double clam = std::cos(lam), slam = std::sin(lam);
  const double hyp = std::sqrt(taup * taup + clam * clam);
  const double xi_p = std::atan2(taup, clam);
  const double eta_p = std::asinh(slam / hyp);

  double xi = xi_p, eta = eta_p;
  double dxi = 1.0, deta = 0.0;  // ∂ξ/∂ξ' and ∂ξ/∂η' — the scale derivative
  for (int j = 1; j <= 6; ++j) {
    const double a = c.alpha[j];
    const double tj = 2.0 * j;
    xi += a * std::sin(tj * xi_p) * std::cosh(tj * eta_p);
    eta += a * std::cos(tj * xi_p) * std::sinh(tj * eta_p);
    dxi += tj * a * std::cos(tj * xi_p) * std::cosh(tj * eta_p);
    deta += tj * a * std::sin(tj * xi_p) * std::sinh(tj * eta_p);
  }

  if (x) *x = p.false_easting + p.k0 * c.A * eta;
  if (y) {
    const double y0 = meridian_arc(el, p.lat0_deg * kDeg);
    *y = p.false_northing + p.k0 * (c.A * xi - y0);
  }

  // Karney 2011 eqs. 25–30. Scale and convergence fall out of the same series
  // derivatives, so they are almost free — and a survey report that states
  // the grid convergence is what lets a user reconcile a grid bearing with a
  // true one.
  if (gamma_deg) {
    const double gamma_p = std::atan2(taup * std::tan(lam), std::sqrt(1.0 + taup * taup));
    const double gamma_pp = std::atan2(deta, dxi);
    *gamma_deg = (gamma_p + gamma_pp) * kRad;
  }
  if (k) {
    const double k1 = std::sqrt(1.0 - el.e2() * sphi * sphi) / (cphi * hyp);
    const double k2 = std::sqrt(dxi * dxi + deta * deta);
    *k = p.k0 * (c.A / el.a) * k1 * k2;
  }
}

void tm_inverse(const Ellipsoid& el, const TmParams& p, double x, double y,
                double* lat_deg, double* lon_deg) noexcept {
  const KrugerCoeffs c = kruger(el);
  const double e = std::sqrt(el.e2());
  const double e2 = el.e2();
  const double y0 = meridian_arc(el, p.lat0_deg * kDeg);
  const double xi = (y - p.false_northing + y0) / (p.k0 * c.A);
  const double eta = (x - p.false_easting) / (p.k0 * c.A);

  double xi_p = xi, eta_p = eta;
  for (int j = 1; j <= 6; ++j) {
    const double b = c.beta[j];
    const double tj = 2.0 * j;
    xi_p -= b * std::sin(tj * xi) * std::cosh(tj * eta);
    eta_p -= b * std::cos(tj * xi) * std::sinh(tj * eta);
  }

  const double sh = std::sinh(eta_p), cxi = std::cos(xi_p);
  // tau' of the conformal latitude (Karney eq. 21).
  const double taup = std::sin(xi_p) / std::sqrt(sh * sh + cxi * cxi);

  // tau' -> tau by Newton (Karney eqs. 19–20). Converges to double precision
  // in 2–3 steps anywhere on the ellipsoid; the loop bound is a guard, not a
  // budget. Note tau' may exceed 1 — it is a TANGENT — so any formulation
  // that takes sqrt(1 - tau'^2) is wrong.
  double tau = taup;
  for (int i = 0; i < 8; ++i) {
    const double sig = std::sinh(e * std::atanh(e * tau / std::sqrt(1.0 + tau * tau)));
    const double tau1 = tau * std::sqrt(1.0 + sig * sig) - sig * std::sqrt(1.0 + tau * tau);
    const double dtau = (taup - tau1) * (1.0 + (1.0 - e2) * tau * tau) /
                        ((1.0 - e2) * std::sqrt(1.0 + tau * tau) * std::sqrt(1.0 + tau1 * tau1));
    tau += dtau;
    if (std::fabs(dtau) < 1e-15 * (1.0 + std::fabs(tau))) break;
  }
  if (lat_deg) *lat_deg = std::atan(tau) * kRad;
  if (lon_deg) {
    const double lam = std::atan2(sh, cxi);
    double lon = p.lon0_deg + lam * kRad;
    while (lon > 180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    *lon_deg = lon;
  }
}

// --- UTM -------------------------------------------------------------------

UtmZone utm_zone_for(double lat_deg, double lon_deg) noexcept {
  UtmZone z;
  double lon = lon_deg;
  while (lon >= 180.0) lon -= 360.0;
  while (lon < -180.0) lon += 360.0;
  int zone = static_cast<int>(std::floor((lon + 180.0) / 6.0)) + 1;
  if (zone < 1) zone = 1;
  if (zone > 60) zone = 60;

  // ISO 19111 / EPSG irregularities. Without these a survey in Bergen or on
  // Svalbard lands in the wrong zone — a 200–400 km easting error that looks
  // entirely plausible until it is overlaid on a basemap.
  if (lat_deg >= 56.0 && lat_deg < 64.0 && lon >= 3.0 && lon < 12.0) {
    zone = 32;  // south-west Norway
  }
  if (lat_deg >= 72.0 && lat_deg < 84.0) {
    if (lon >= 0.0 && lon < 9.0) zone = 31;
    else if (lon >= 9.0 && lon < 21.0) zone = 33;
    else if (lon >= 21.0 && lon < 33.0) zone = 35;
    else if (lon >= 33.0 && lon < 42.0) zone = 37;
  }
  z.zone = zone;
  z.north = lat_deg >= 0.0;
  return z;
}

TmParams utm_params(UtmZone z) noexcept {
  TmParams p;
  p.lon0_deg = static_cast<double>(z.zone) * 6.0 - 183.0;
  p.lat0_deg = 0.0;
  p.k0 = 0.9996;
  p.false_easting = 500000.0;
  p.false_northing = z.north ? 0.0 : 10000000.0;
  return p;
}

UtmCoord geodetic_to_utm_zone(const Geodetic& g, UtmZone z, const Ellipsoid& el) noexcept {
  UtmCoord c;
  if (!z.valid()) return c;
  const TmParams p = utm_params(z);
  double k = 1.0, gamma = 0.0;
  tm_forward(el, p, g.lat_deg, g.lon_deg, &c.easting, &c.northing, &k, &gamma);
  c.zone = z.zone;
  c.north = z.north;
  c.scale = k;
  c.convergence_deg = gamma;
  return c;
}

UtmCoord geodetic_to_utm(const Geodetic& g, const Ellipsoid& el) noexcept {
  return geodetic_to_utm_zone(g, utm_zone_for(g.lat_deg, g.lon_deg), el);
}

Geodetic utm_to_geodetic(const UtmCoord& c, double height_m, const Ellipsoid& el) noexcept {
  Geodetic g;
  UtmZone z{c.zone, c.north};
  if (!z.valid()) return g;
  const TmParams p = utm_params(z);
  tm_inverse(el, p, c.easting, c.northing, &g.lat_deg, &g.lon_deg);
  g.height_m = height_m;
  return g;
}

// --- EPSG / WKT / PROJ -----------------------------------------------------

int utm_epsg(UtmZone z) noexcept {
  if (!z.valid()) return 0;
  return (z.north ? 32600 : 32700) + z.zone;
}

UtmZone utm_zone_from_epsg(int epsg) noexcept {
  UtmZone z;
  if (epsg > 32600 && epsg <= 32660) {
    z.zone = epsg - 32600;
    z.north = true;
  } else if (epsg > 32700 && epsg <= 32760) {
    z.zone = epsg - 32700;
    z.north = false;
  }
  return z;
}

std::string epsg_string(int epsg) {
  if (epsg <= 0) return std::string();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "EPSG:%d", epsg);
  return std::string(buf);
}

int parse_epsg_string(const std::string& s) noexcept {
  const char* p = s.c_str();
  if (s.size() > 5 && (s.compare(0, 5, "EPSG:") == 0 || s.compare(0, 5, "epsg:") == 0)) {
    p = s.c_str() + 5;
  }
  int v = 0;
  bool any = false;
  for (; *p; ++p) {
    if (*p < '0' || *p > '9') return 0;
    v = v * 10 + (*p - '0');
    any = true;
    if (v > 1000000) return 0;
  }
  return any ? v : 0;
}

namespace {

// The WGS 84 geographic node, verbatim from the EPSG registry's WKT1 for
// 4326. Shared by the geographic CRS and by every UTM PROJCS's base.
const char* kWgs84Geogcs =
    "GEOGCS[\"WGS 84\","
    "DATUM[\"WGS_1984\","
    "SPHEROID[\"WGS 84\",6378137,298.257223563,AUTHORITY[\"EPSG\",\"7030\"]],"
    "AUTHORITY[\"EPSG\",\"6326\"]],"
    "PRIMEM[\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],"
    "UNIT[\"degree\",0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],"
    "AUTHORITY[\"EPSG\",\"4326\"]]";

}  // namespace

std::string wgs84_geographic_wkt1() { return std::string(kWgs84Geogcs); }

std::string utm_wkt1(UtmZone z) {
  if (!z.valid()) return std::string();
  const double lon0 = static_cast<double>(z.zone) * 6.0 - 183.0;
  const double fn = z.north ? 0.0 : 10000000.0;
  char buf[1400];
  std::snprintf(
      buf, sizeof(buf),
      "PROJCS[\"WGS 84 / UTM zone %d%c\",%s,"
      "PROJECTION[\"Transverse_Mercator\"],"
      "PARAMETER[\"latitude_of_origin\",0],"
      "PARAMETER[\"central_meridian\",%g],"
      "PARAMETER[\"scale_factor\",0.9996],"
      "PARAMETER[\"false_easting\",500000],"
      "PARAMETER[\"false_northing\",%g],"
      "UNIT[\"metre\",1,AUTHORITY[\"EPSG\",\"9001\"]],"
      "AXIS[\"Easting\",EAST],"
      "AXIS[\"Northing\",NORTH],"
      "AUTHORITY[\"EPSG\",\"%d\"]]",
      z.zone, z.north ? 'N' : 'S', kWgs84Geogcs, lon0, fn, utm_epsg(z));
  return std::string(buf);
}

std::string wkt1_for_epsg(int epsg) {
  if (epsg == kEpsgWgs84Geographic2D || epsg == kEpsgWgs84Geographic3D) {
    return wgs84_geographic_wkt1();
  }
  const UtmZone z = utm_zone_from_epsg(epsg);
  if (z.valid()) return utm_wkt1(z);
  return std::string();
}

std::string utm_proj_string(UtmZone z) {
  if (!z.valid()) return std::string();
  char buf[128];
  std::snprintf(buf, sizeof(buf), "+proj=utm +zone=%d%s +datum=WGS84 +units=m +no_defs +type=crs",
                z.zone, z.north ? "" : " +south");
  return std::string(buf);
}

std::string proj_string_for_epsg(int epsg) {
  if (epsg == kEpsgWgs84Geographic2D || epsg == kEpsgWgs84Geographic3D) {
    return "+proj=longlat +datum=WGS84 +no_defs +type=crs";
  }
  const UtmZone z = utm_zone_from_epsg(epsg);
  if (z.valid()) return utm_proj_string(z);
  return std::string();
}

std::string crs_name_for_epsg(int epsg) {
  if (epsg == kEpsgWgs84Geographic2D) return "WGS 84";
  if (epsg == kEpsgWgs84Geographic3D) return "WGS 84 (3D)";
  if (epsg == kEpsgWgs84Geocentric) return "WGS 84 (geocentric)";
  const UtmZone z = utm_zone_from_epsg(epsg);
  if (z.valid()) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "WGS 84 / UTM zone %d%c", z.zone, z.north ? 'N' : 'S');
    return std::string(buf);
  }
  return epsg_string(epsg);
}

}  // namespace crs
}  // namespace scanengine
