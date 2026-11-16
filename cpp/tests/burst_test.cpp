#include <gtest/gtest.h>

#include "../sr/Burst.h"
#include "fixtures.h"

#include <opencv2/imgproc.hpp>

#include <cmath>

namespace {

cv::Mat expectedGraySmall(const cv::Mat& bgr, double s) {
  cv::Mat gray, small, out;
  cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
  cv::resize(gray, small, cv::Size(), s, s, cv::INTER_AREA);
  small.convertTo(out, CV_32F);
  return out;
}

// invariant: variance computed by hand (two-pass, double), independent of meanStdDev.
double laplacianVariance(const cv::Mat& g32f) {
  cv::Mat lap;
  cv::Laplacian(g32f, lap, CV_32F);
  double sum = 0;
  for (int r = 0; r < lap.rows; ++r)
    for (int c = 0; c < lap.cols; ++c) sum += lap.at<float>(r, c);
  const double mean = sum / lap.total();
  double sq = 0;
  for (int r = 0; r < lap.rows; ++r)
    for (int c = 0; c < lap.cols; ++c) sq += std::pow(lap.at<float>(r, c) - mean, 2);
  return sq / lap.total();
}

}  // namespace

// LP-CORE-01 (sharpness input): lab gray_small, landscape.
TEST(Burst, GraySmallShrinksLongSideTo1024) {
  const cv::Mat bgr = fixtures::texturedImage(2000, 1500, 3);
  double s = 0;
  const cv::Mat g = sr::graySmall(bgr, s);
  EXPECT_EQ(s, 1024.0 / 2000.0);
  EXPECT_EQ(g.size(), cv::Size(1024, 768));
  EXPECT_EQ(g.type(), CV_32FC1);
  EXPECT_EQ(cv::norm(g, expectedGraySmall(bgr, s), cv::NORM_INF), 0.0);
}

// Edge case: frames smaller than 1024 px are upscaled, as lab gray_small does.
TEST(Burst, GraySmallUpscalesSmallFrames) {
  const cv::Mat bgr = fixtures::texturedImage(640, 480, 4);
  double s = 0;
  const cv::Mat g = sr::graySmall(bgr, s);
  EXPECT_EQ(s, 1.6);
  EXPECT_EQ(g.size(), cv::Size(1024, 768));
  EXPECT_EQ(cv::norm(g, expectedGraySmall(bgr, s), cv::NORM_INF), 0.0);
}

TEST(Burst, GraySmallUsesTheLongSideOfPortraitFrames) {
  const cv::Mat bgr = fixtures::texturedImage(1500, 2000, 5);
  double s = 0;
  const cv::Mat g = sr::graySmall(bgr, s);
  EXPECT_EQ(s, 1024.0 / 2000.0);
  EXPECT_EQ(g.size(), cv::Size(768, 1024));
}

TEST(Burst, SharpnessIsTheLaplacianVariance) {
  double s = 0;
  const cv::Mat g = sr::graySmall(fixtures::texturedImage(1200, 900, 6), s);
  const double expected = laplacianVariance(g);
  EXPECT_GT(expected, 0.0);
  EXPECT_NEAR(sr::sharpness(g), expected, expected * 1e-9);
}

TEST(Burst, SharpnessOfAFlatImageIsZero) {
  const cv::Mat flat(768, 1024, CV_32F, cv::Scalar(80));
  EXPECT_EQ(sr::sharpness(flat), 0.0);
}

TEST(Burst, BlurLowersSharpness) {
  const cv::Mat bgr = fixtures::texturedImage(1200, 900, 8);
  cv::Mat blurred;
  cv::GaussianBlur(bgr, blurred, cv::Size(0, 0), 2.0);
  double s = 0;
  EXPECT_LT(sr::sharpness(sr::graySmall(blurred, s)), sr::sharpness(sr::graySmall(bgr, s)));
}
