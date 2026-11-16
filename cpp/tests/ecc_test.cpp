#include <gtest/gtest.h>

#include "../sr/vendor/Ecc.h"
#include "fixtures.h"

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

namespace {

constexpr int kW = 320;
constexpr int kH = 240;
constexpr int kIter = 100;
constexpr double kEps = 1e-5;
constexpr int kGauss = 5;

cv::Mat toFloatGray(const cv::Mat& bgr) {
  cv::Mat g, f;
  cv::cvtColor(bgr, g, cv::COLOR_BGR2GRAY);
  g.convertTo(f, CV_32F);
  return f;
}

// invariant: smooth texture, so ECC has gradients everywhere (raw RNG noise would not converge).
cv::Mat smoothScene() {
  cv::Mat img = fixtures::texturedImage(kW, kH, 11);
  cv::GaussianBlur(img, img, cv::Size(0, 0), 2.0);
  return toFloatGray(img);
}

cv::Mat knownHomography() {
  return (cv::Mat_<double>(3, 3) << 1.002, 0.003, 2.4, -0.002, 0.999, -1.6, 1e-6, -2e-6, 1);
}

struct Pair {
  cv::Mat templ;
  cv::Mat input;
};

Pair warpedPair() {
  const cv::Mat templ = smoothScene();
  cv::Mat input;
  cv::warpPerspective(templ, input, knownHomography(), templ.size(), cv::INTER_LINEAR,
                      cv::BORDER_REPLICATE);
  return {templ, input};
}

}  // namespace

// LP-CORE-05 / T1: the vendored copy computes exactly what the library function computes.
TEST(Ecc, MatchesOpenCvFindTransformEccExactly) {
  const Pair p = warpedPair();
  cv::Mat expectedH = cv::Mat::eye(3, 3, CV_32F);
  const double expectedRho = cv::findTransformECC(
      p.templ, p.input, expectedH, cv::MOTION_HOMOGRAPHY,
      cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, kIter, kEps), cv::noArray(),
      kGauss);

  cv::Mat H = cv::Mat::eye(3, 3, CV_32F);
  const auto rho = sr::vendor::eccHomography(p.templ, p.input, H, kIter, kEps, kGauss);

  ASSERT_TRUE(rho.has_value());
  EXPECT_EQ(*rho, expectedRho);
  EXPECT_EQ(cv::norm(H, expectedH, cv::NORM_INF), 0.0);
  EXPECT_GT(*rho, 0.9);
}

TEST(Ecc, FlatInputDoesNotConverge) {
  const cv::Mat templ = smoothScene();
  const cv::Mat flat(kH, kW, CV_32F, cv::Scalar(100));
  cv::Mat H = cv::Mat::eye(3, 3, CV_32F);
  EXPECT_FALSE(sr::vendor::eccHomography(templ, flat, H, kIter, kEps, kGauss).has_value());
}

TEST(Ecc, InitialGuessOutsideTheImageDoesNotConverge) {
  const Pair p = warpedPair();
  cv::Mat H = (cv::Mat_<float>(3, 3) << 1, 0, 900, 0, 1, 700, 0, 0, 1);
  EXPECT_FALSE(sr::vendor::eccHomography(p.templ, p.input, H, kIter, kEps, kGauss).has_value());
}

TEST(Ecc, UncorrelatedInputScoresBelowThreshold) {
  const cv::Mat templ = smoothScene();
  cv::Mat noise(kH, kW, CV_32F);
  cv::RNG(5).fill(noise, cv::RNG::UNIFORM, 0, 255);
  cv::Mat H = cv::Mat::eye(3, 3, CV_32F);
  const auto rho = sr::vendor::eccHomography(templ, noise, H, kIter, kEps, kGauss);
  // why: either outcome drops the frame; what matters is that it returns instead of aborting.
  if (rho.has_value()) EXPECT_LT(*rho, 0.9);
}

TEST(Ecc, RejectsArgumentsUpstreamWouldAssertOn) {
  const Pair p = warpedPair();
  cv::Mat H = cv::Mat::eye(3, 3, CV_32F);
  cv::Mat templ8u;
  p.templ.convertTo(templ8u, CV_8U);
  EXPECT_FALSE(sr::vendor::eccHomography(templ8u, p.input, H, kIter, kEps, kGauss).has_value());
  EXPECT_FALSE(sr::vendor::eccHomography(cv::Mat(), p.input, H, kIter, kEps, kGauss).has_value());

  cv::Mat H2x3 = cv::Mat::eye(2, 3, CV_32F);
  EXPECT_FALSE(sr::vendor::eccHomography(p.templ, p.input, H2x3, kIter, kEps, kGauss).has_value());
  cv::Mat H64 = cv::Mat::eye(3, 3, CV_64F);
  EXPECT_FALSE(sr::vendor::eccHomography(p.templ, p.input, H64, kIter, kEps, kGauss).has_value());

  EXPECT_FALSE(sr::vendor::eccHomography(p.templ, p.input, H, 0, kEps, kGauss).has_value());
  EXPECT_FALSE(sr::vendor::eccHomography(p.templ, p.input, H, kIter, kEps, 4).has_value());
}
