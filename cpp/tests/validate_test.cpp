#include <gtest/gtest.h>

#include "../sr/Errors.h"
#include "../sr/Pipeline.h"

#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kAccepted = "<accepted>";

sr::Request validRequest(size_t frames = 5) {
  sr::Request r;
  r.paths.assign(frames, "/frames/a.jpg");
  r.defaultOutputDir = "/tmp";
  return r;
}

std::string outcome(const sr::Request& r) {
  try {
    sr::validate(r);
  } catch (const sr::SrError& e) {
    return e.code;
  }
  return kAccepted;
}

std::string message(const sr::Request& r) {
  try {
    sr::validate(r);
  } catch (const sr::SrError& e) {
    return e.message;
  }
  return kAccepted;
}

}  // namespace

// LP-API-02: any count from 3 to 30.
TEST(Validate, AcceptsFrameCounts3To30) {
  for (size_t n : {3u, 4u, 5u, 8u, 15u, 29u, 30u}) EXPECT_EQ(outcome(validRequest(n)), kAccepted) << n;
}

TEST(Validate, RejectsFrameCountsOutside3To30) {
  for (size_t n : {0u, 1u, 2u, 31u, 60u}) {
    EXPECT_EQ(outcome(validRequest(n)), "E_INVALID_ARGUMENT") << n;
  }
  EXPECT_EQ(message(validRequest(2)), "paths must contain 3 to 30 entries, got 2");
}

// LP-API-03
TEST(Validate, AcceptsN2To30) {
  for (int n : {2, 8, 30}) {
    auto r = validRequest();
    r.n = n;
    EXPECT_EQ(outcome(r), kAccepted) << n;
  }
}

TEST(Validate, RejectsNOutside2To30) {
  for (int n : {0, 1, 31, -8}) {
    auto r = validRequest();
    r.n = n;
    EXPECT_EQ(outcome(r), "E_INVALID_ARGUMENT") << n;
  }
  auto r = validRequest();
  r.n = 31;
  EXPECT_EQ(message(r), "n must be an integer from 2 to 30, got 31");
}

// LP-API-03: n larger than the burst is valid; the mean uses what is available.
TEST(Validate, AcceptsNLargerThanTheBurst) {
  auto r = validRequest(3);
  r.n = 30;
  EXPECT_EQ(outcome(r), kAccepted);
}

// LP-API-04
TEST(Validate, AcceptsBlur0To1) {
  for (double b : {0.0, 0.5, 0.8, 1.0}) {
    auto r = validRequest();
    r.blur = b;
    EXPECT_EQ(outcome(r), kAccepted) << b;
  }
}

TEST(Validate, RejectsBlurOutside0To1OrNotFinite) {
  for (double b : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::infinity()}) {
    auto r = validRequest();
    r.blur = b;
    EXPECT_EQ(outcome(r), "E_INVALID_ARGUMENT") << b;
  }
  auto r = validRequest();
  r.blur = 1.5;
  EXPECT_EQ(message(r), "blur must be a number from 0 to 1, got 1.5");
}

TEST(Validate, DefaultsAreN8AndBlur08) {
  const sr::Request r;
  EXPECT_EQ(r.n, 8);
  EXPECT_EQ(r.blur, 0.8);
}

TEST(Validate, RejectsEmptyPathString) {
  auto r = validRequest();
  r.paths[3] = "";
  EXPECT_EQ(outcome(r), "E_INVALID_ARGUMENT");
}

TEST(Validate, AcceptsScale1To4) {
  for (int s : {1, 2, 3, 4}) {
    auto r = validRequest();
    r.scale = s;
    EXPECT_EQ(outcome(r), kAccepted) << s;
  }
}

TEST(Validate, RejectsScaleOutOfRange) {
  for (int s : {0, 5, -1}) {
    auto r = validRequest();
    r.scale = s;
    EXPECT_EQ(outcome(r), "E_INVALID_ARGUMENT") << s;
  }
}

TEST(Validate, AcceptsJpegAndPngOutputExtensions) {
  for (const char* p : {"/out/a.jpg", "/out/a.JPG", "/out/a.jpeg", "/out/a.png"}) {
    auto r = validRequest();
    r.outputPath = p;
    EXPECT_EQ(outcome(r), kAccepted) << p;
  }
}

TEST(Validate, RejectsOtherOutputExtensions) {
  for (const char* p : {"/out/a.bmp", "/out/a", ""}) {
    auto r = validRequest();
    r.outputPath = std::string(p);
    EXPECT_EQ(outcome(r), "E_INVALID_ARGUMENT") << '"' << p << '"';
  }
}
