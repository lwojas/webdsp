#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

// Tiny assert-based test harness. Not gtest: the native test surface here is small
// (resampler math, scheduler ordering) and doesn't need a test framework dependency.
namespace webdsp::test {

inline int g_failures = 0;
inline int g_count = 0;

inline void check(bool cond, const std::string& what, const char* file, int line) {
  g_count++;
  if (!cond) {
    g_failures++;
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", what.c_str(), file, line);
  }
}

inline void checkNear(double a, double b, double tol, const std::string& what, const char* file,
                       int line) {
  check(std::fabs(a - b) <= tol, what + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")",
        file, line);
}

}  // namespace webdsp::test

#define CHECK(cond) webdsp::test::check((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) webdsp::test::checkNear((a), (b), (tol), #a " ~= " #b, __FILE__, __LINE__)
