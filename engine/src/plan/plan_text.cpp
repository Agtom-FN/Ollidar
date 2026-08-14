// plan_text.cpp — locale-independent, deterministic number formatting.
//
// Both output formats are text (DXF is ASCII group codes; a PDF content
// stream is ASCII operators), so every coordinate in both files goes through
// here. See plan_internal.h for why this is not std::snprintf.
#include <cmath>
#include <cstdint>
#include <string>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

const double kPow10[] = {1.0,      1e1,  1e2,  1e3,  1e4,  1e5,
                         1e6,      1e7,  1e8,  1e9,  1e10, 1e11};
constexpr int kMaxDecimals = 11;

}  // namespace

std::string fmt_int(long long v) {
  if (v == 0) return "0";
  const bool neg = v < 0;
  unsigned long long u = neg ? (0ull - static_cast<unsigned long long>(v))
                             : static_cast<unsigned long long>(v);
  char buf[24];
  int n = 0;
  while (u > 0) {
    buf[n++] = static_cast<char>('0' + (u % 10));
    u /= 10;
  }
  std::string s;
  s.reserve(static_cast<std::size_t>(n) + (neg ? 1u : 0u));
  if (neg) s.push_back('-');
  while (n > 0) s.push_back(buf[--n]);
  return s;
}

std::string fmt_fixed(double v, int decimals) {
  if (decimals < 0) decimals = 0;
  if (decimals > kMaxDecimals) decimals = kMaxDecimals;
  // A non-finite coordinate must never reach a file: it would produce a DXF
  // no reader accepts and a PDF that renders as nothing. Zero is wrong but
  // it is bounded, visible, and cannot corrupt the surrounding syntax.
  if (!std::isfinite(v)) v = 0.0;

  const bool neg = std::signbit(v) && v != 0.0;
  double a = std::fabs(v);
  const double scale = kPow10[decimals];
  // Round-half-away-from-zero, done once, on a value the caller can predict.
  double scaled = std::floor(a * scale + 0.5);
  if (scaled >= 9.2e18) scaled = 0.0;  // absurd input; see above
  std::uint64_t units = static_cast<std::uint64_t>(scaled);

  const std::uint64_t iscale = static_cast<std::uint64_t>(scale);
  const std::uint64_t whole = units / iscale;
  const std::uint64_t frac = units % iscale;

  std::string s;
  // -0.000 is legal in both formats but is noise in a diff; a rounded-to-zero
  // value loses its sign here.
  if (neg && (whole != 0 || frac != 0)) s.push_back('-');
  s += fmt_int(static_cast<long long>(whole));
  if (decimals > 0) {
    s.push_back('.');
    std::string f = fmt_int(static_cast<long long>(frac));
    for (int k = static_cast<int>(f.size()); k < decimals; ++k) s.push_back('0');
    s += f;
  }
  return s;
}

std::string fmt_trim(double v, int decimals) {
  std::string s = fmt_fixed(v, decimals);
  if (s.find('.') == std::string::npos) return s;
  std::size_t end = s.size();
  while (end > 0 && s[end - 1] == '0') --end;
  if (end > 0 && s[end - 1] == '.') --end;
  s.resize(end);
  if (s.empty() || s == "-") s = "0";
  return s;
}

}  // namespace plan
}  // namespace scanengine
