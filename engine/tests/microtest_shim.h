// microtest_shim.h — lets the S1 spike's test file run unmodified under
// doctest.
//
// tests/test_d6_parser.cpp is spikes/s1-d6-parser/tests/test_d6.cpp with only
// its #includes changed. Keeping the 33 cases byte-identical is the point:
// they are the S1 exit-criteria evidence, and a rewrite would quietly change
// what is being asserted. This shim maps microtest's three macros onto
// doctest with the SAME semantics microtest.h had (CHECK_EQ compares as
// long long; CHECK_NEAR is an absolute tolerance).
#ifndef SCANENGINE_TESTS_MICROTEST_SHIM_H
#define SCANENGINE_TESTS_MICROTEST_SHIM_H

#include <cmath>

#include "doctest.h"

#undef CHECK_EQ
#define CHECK_EQ(a, b) CHECK(static_cast<long long>(a) == static_cast<long long>(b))
#define CHECK_NEAR(a, b, eps) \
  CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= static_cast<double>(eps))
#define TEST(name) TEST_CASE("d6/" #name)

#endif  // SCANENGINE_TESTS_MICROTEST_SHIM_H
