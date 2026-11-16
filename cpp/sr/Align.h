#pragma once

#include <opencv2/core.hpp>

#include <string>

namespace sr {

// invariant: lab align.MIN_CC; an ECC correlation below this means the frame did not align.
inline constexpr double kMinEccCorrelation = 0.9;

struct WarpEstimate {
  enum class Status { Ok, NoConvergence, LowCorrelation };
  Status status = Status::NoConvergence;
  // invariant: when status is Ok, a 3x3 CV_32F homography in full-resolution pixels mapping
  // reference coordinates to frame coordinates (use with WARP_INVERSE_MAP).
  cv::Mat H;
  double cc = 0;
};

// invariant: lab align.estimate_warp on two graySmall images (same size, CV_32F) whose
// frames were shrunk by s: phaseCorrelate (Hanning CV_64F, float64 inputs) as the initial
// translation, vendored ECC MOTION_HOMOGRAPHY (100 iterations / 1e-5, gaussFiltSize 5), then
// H = inv(S) * H * S in float32. Never aborts: bad inputs and ECC failures are NoConvergence.
WarpEstimate estimateWarp(const cv::Mat& refSmall, const cv::Mat& graySmall, double s);

// invariant: lab align.align log text without the "frame NNN: " prefix (status != Ok).
std::string dropReason(const WarpEstimate& estimate);

}  // namespace sr
