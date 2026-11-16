#include <gtest/gtest.h>

#include "../sr/Align.h"
#include "../sr/Burst.h"
#include "fixtures.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <regex>
#include <vector>

namespace {

using Status = sr::WarpEstimate::Status;

constexpr int kW = 2048;
constexpr int kH = 1536;

// invariant: rotation by deg about the image centre, uniform scale, then translation (tx, ty).
cv::Mat similarity(double deg, double scale, double tx, double ty) {
  const cv::Mat R = cv::getRotationMatrix2D(cv::Point2f(kW / 2.0f, kH / 2.0f), deg, scale);
  cv::Mat H = cv::Mat::eye(3, 3, CV_64F);
  R.copyTo(H.rowRange(0, 2));
  H.at<double>(0, 2) += tx;
  H.at<double>(1, 2) += ty;
  return H;
}

// invariant: frame(Htrue x) == scene(x), so the estimate mapping reference to frame is Htrue.
cv::Mat warpedFrame(const cv::Mat& scene, const cv::Mat& Htrue) {
  cv::Mat frame;
  cv::warpPerspective(scene, frame, Htrue, scene.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
  return frame;
}

double maxCornerError(const cv::Mat& H, const cv::Mat& Htrue) {
  const std::vector<cv::Point2d> corners = {{0, 0}, {kW - 1.0, 0}, {0, kH - 1.0}, {kW - 1.0, kH - 1.0}};
  cv::Mat H64;
  H.convertTo(H64, CV_64F);
  std::vector<cv::Point2d> a, b;
  cv::perspectiveTransform(corners, a, H64);
  cv::perspectiveTransform(corners, b, Htrue);
  double worst = 0;
  for (size_t i = 0; i < corners.size(); ++i) worst = std::max(worst, cv::norm(a[i] - b[i]));
  return worst;
}

struct Smalls {
  cv::Mat ref;
  cv::Mat cur;
  double s = 0;
};

Smalls smalls(const cv::Mat& refBgr, const cv::Mat& curBgr) {
  Smalls out;
  out.ref = sr::graySmall(refBgr, out.s);
  double s2 = 0;
  out.cur = sr::graySmall(curBgr, s2);
  return out;
}

}  // namespace

// LP-CORE-04: phaseCorrelate + ECC recovers a shift + rotation + scale at full resolution.
TEST(EstimateWarp, RecoversAHomographyWithinTheParityTolerance) {
  const cv::Mat scene = fixtures::texturedImage(kW, kH, 21);
  const cv::Mat Htrue = similarity(0.3, 1.002, 5.3, -3.7);
  const Smalls g = smalls(scene, warpedFrame(scene, Htrue));

  const sr::WarpEstimate e = sr::estimateWarp(g.ref, g.cur, g.s);

  ASSERT_EQ(e.status, Status::Ok);
  EXPECT_GE(e.cc, sr::kMinEccCorrelation);
  EXPECT_EQ(e.H.type(), CV_32FC1);
  EXPECT_LT(maxCornerError(e.H, Htrue), 0.05);
}

// LP-CORE-04: the reference against itself is the identity.
TEST(EstimateWarp, IdenticalFramesGiveTheIdentity) {
  const cv::Mat scene = fixtures::texturedImage(1600, 1200, 22);
  const Smalls g = smalls(scene, scene);
  const sr::WarpEstimate e = sr::estimateWarp(g.ref, g.cur, g.s);
  ASSERT_EQ(e.status, Status::Ok);
  EXPECT_LT(cv::norm(e.H, cv::Mat::eye(3, 3, CV_32F), cv::NORM_INF), 1e-4);
}

// LP-CORE-05: ECC converges but the correlation is below 0.9.
TEST(EstimateWarp, NoisyFrameIsLowCorrelation) {
  const cv::Mat scene = fixtures::texturedImage(1600, 1200, 23);
  cv::Mat noisy;
  scene.convertTo(noisy, CV_16SC3);
  cv::Mat noise(scene.size(), CV_16SC3);
  cv::RNG(9).fill(noise, cv::RNG::NORMAL, cv::Scalar::all(0), cv::Scalar::all(120));
  noisy += noise;
  noisy.convertTo(noisy, CV_8UC3);
  const Smalls g = smalls(scene, noisy);

  const sr::WarpEstimate e = sr::estimateWarp(g.ref, g.cur, g.s);

  ASSERT_EQ(e.status, Status::LowCorrelation);
  EXPECT_LT(e.cc, sr::kMinEccCorrelation);
  EXPECT_TRUE(std::regex_match(sr::dropReason(e), std::regex(R"(ECC cc 0\.\d{3} < 0\.9, dropped)")))
      << sr::dropReason(e);
}

// LP-CORE-05 / edge case: a flat frame does not abort and is dropped as non-converged.
TEST(EstimateWarp, FlatFrameDoesNotConverge) {
  const cv::Mat scene = fixtures::texturedImage(1600, 1200, 24);
  const cv::Mat flat(scene.size(), CV_8UC3, cv::Scalar::all(90));
  const Smalls g = smalls(scene, flat);

  const sr::WarpEstimate e = sr::estimateWarp(g.ref, g.cur, g.s);

  EXPECT_EQ(e.status, Status::NoConvergence);
  EXPECT_EQ(sr::dropReason(e), "ECC did not converge, dropped");
}

// LP-CORE-05: an unrelated scene never counts as aligned.
TEST(EstimateWarp, UnrelatedSceneIsDropped) {
  const Smalls g = smalls(fixtures::texturedImage(1600, 1200, 25), fixtures::texturedImage(1600, 1200, 26));
  const sr::WarpEstimate e = sr::estimateWarp(g.ref, g.cur, g.s);
  EXPECT_NE(e.status, Status::Ok);
}

// Edge case: inputs OpenCV would assert on never reach it.
TEST(EstimateWarp, DegenerateInputsDoNotReachOpenCv) {
  const cv::Mat row(1, 1024, CV_32F, cv::Scalar(10));
  EXPECT_EQ(sr::estimateWarp(row, row, 1.0).status, Status::NoConvergence);

  const cv::Mat a(768, 1024, CV_32F, cv::Scalar(10));
  const cv::Mat b(768, 1000, CV_32F, cv::Scalar(10));
  EXPECT_EQ(sr::estimateWarp(a, b, 1.0).status, Status::NoConvergence);

  cv::Mat a8;
  a.convertTo(a8, CV_8U);
  EXPECT_EQ(sr::estimateWarp(a8, a8, 1.0).status, Status::NoConvergence);
  EXPECT_EQ(sr::estimateWarp(cv::Mat(), cv::Mat(), 1.0).status, Status::NoConvergence);
}

TEST(EstimateWarp, LowCorrelationReasonUsesThreeDecimals) {
  sr::WarpEstimate e;
  e.status = Status::LowCorrelation;
  e.cc = 0.87349;
  EXPECT_EQ(sr::dropReason(e), "ECC cc 0.873 < 0.9, dropped");
}
