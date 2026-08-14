// Tiny single-header test framework (plain asserts + a registry).
// Deliberately dependency-free: the S1 spike must build with nothing but a
// C++17 toolchain.
#ifndef MICROTEST_H
#define MICROTEST_H

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace microtest {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& fail_count() { static int f = 0; return f; }
inline int& check_count() { static int c = 0; return c; }
inline const char*& current() { static const char* n = ""; return n; }

struct Registrar {
  Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

inline void report_fail(const char* file, int line, const std::string& msg) {
  ++fail_count();
  std::printf("  FAIL %s:%d  [%s]\n        %s\n", file, line, current(),
              msg.c_str());
}

inline int run_all() {
  std::printf("running %zu test cases\n", cases().size());
  int failed_cases = 0;
  for (const auto& c : cases()) {
    current() = c.name;
    const int before = fail_count();
    c.fn();
    const bool ok = fail_count() == before;
    if (!ok) ++failed_cases;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", c.name);
  }
  std::printf("\n%d checks, %zu cases, %d failed cases, %d failed checks\n",
              check_count(), cases().size(), failed_cases, fail_count());
  std::printf("%s\n", fail_count() == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
  return fail_count() == 0 ? 0 : 1;
}

}  // namespace microtest

#define TEST(name)                                            \
  static void name();                                         \
  static ::microtest::Registrar microtest_reg_##name(#name, name); \
  static void name()

#define CHECK(expr)                                                        \
  do {                                                                     \
    ++::microtest::check_count();                                          \
    if (!(expr))                                                           \
      ::microtest::report_fail(__FILE__, __LINE__,                         \
                               std::string("CHECK(") + #expr + ") failed"); \
  } while (0)

#define CHECK_EQ(a, b)                                                      \
  do {                                                                      \
    ++::microtest::check_count();                                           \
    const auto microtest_a = (a);                                           \
    const auto microtest_b = (b);                                           \
    if (!(microtest_a == microtest_b)) {                                    \
      char microtest_buf[256];                                              \
      std::snprintf(microtest_buf, sizeof(microtest_buf),                   \
                    "CHECK_EQ(%s, %s): %lld != %lld", #a, #b,               \
                    (long long)(microtest_a), (long long)(microtest_b));    \
      ::microtest::report_fail(__FILE__, __LINE__, microtest_buf);          \
    }                                                                       \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                               \
  do {                                                                      \
    ++::microtest::check_count();                                           \
    const double microtest_a = (double)(a);                                 \
    const double microtest_b = (double)(b);                                 \
    if (!(std::fabs(microtest_a - microtest_b) <= (double)(eps))) {          \
      char microtest_buf[256];                                              \
      std::snprintf(microtest_buf, sizeof(microtest_buf),                   \
                    "CHECK_NEAR(%s, %s, %s): %.6f vs %.6f", #a, #b, #eps,   \
                    microtest_a, microtest_b);                              \
      ::microtest::report_fail(__FILE__, __LINE__, microtest_buf);          \
    }                                                                       \
  } while (0)

#endif  // MICROTEST_H
