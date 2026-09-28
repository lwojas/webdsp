#include "../src/resampler.h"
#include "test_main.h"

using namespace webdsp;

static void testLinearExactSamples() {
  float data[] = {0.0f, 1.0f, 2.0f, 3.0f};
  CHECK_NEAR(linear(data, 4, 0.0), 0.0, 1e-6);
  CHECK_NEAR(linear(data, 4, 1.0), 1.0, 1e-6);
  CHECK_NEAR(linear(data, 4, 2.5), 2.5, 1e-6);
}

static void testLinearClampsAtEdges() {
  float data[] = {5.0f, 6.0f, 7.0f};
  CHECK_NEAR(linear(data, 3, -1.0), 5.0, 1e-6);
  CHECK_NEAR(linear(data, 3, 10.0), 7.0, 1e-6);
}

static void testCubicHermiteExactAtIntegerPositions() {
  // At integer sample positions, cubic Hermite must reproduce the original sample exactly
  // regardless of neighbours (that's the defining property of an interpolating spline).
  float data[] = {0.2f, -0.5f, 1.0f, 0.3f, -0.1f};
  for (int i = 0; i < 5; i++) {
    CHECK_NEAR(cubicHermite(data, 5, static_cast<double>(i)), data[i], 1e-5);
  }
}

static void testCubicHermiteSmoothOnLine() {
  // A perfectly linear ramp should interpolate linearly too (Catmull-Rom reproduces lines).
  float data[] = {0.0f, 2.0f, 4.0f, 6.0f, 8.0f};
  CHECK_NEAR(cubicHermite(data, 5, 1.5), 3.0, 1e-4);
  CHECK_NEAR(cubicHermite(data, 5, 2.25), 4.5, 1e-4);
}

static void testCubicHermiteClampsAtEdges() {
  float data[] = {1.0f, 2.0f, 3.0f};
  // Should not crash or read out of bounds reading near/at the edges.
  CHECK_NEAR(cubicHermite(data, 3, -0.5), 1.0, 0.6);
  CHECK_NEAR(cubicHermite(data, 3, 2.5), 3.0, 0.6);
}

int main() {
  testLinearExactSamples();
  testLinearClampsAtEdges();
  testCubicHermiteExactAtIntegerPositions();
  testCubicHermiteSmoothOnLine();
  testCubicHermiteClampsAtEdges();

  std::printf("resampler_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
