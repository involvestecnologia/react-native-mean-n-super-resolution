#include "Align.h"

#include "vendor/Ecc.h"

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>

namespace sr {

WarpEstimate estimateWarp(const cv::Mat& refSmall, const cv::Mat& graySmall, double s) {
  WarpEstimate out;
  // invariant: createHanningWindow/phaseCorrelate and ECC assert on these (AD-004: abort).
  if (refSmall.empty() || refSmall.type() != CV_32FC1 || graySmall.type() != CV_32FC1 ||
      refSmall.size() != graySmall.size() || refSmall.cols < 2 || refSmall.rows < 2) {
    return out;
  }

  cv::Mat ref64, cur64, window;
  refSmall.convertTo(ref64, CV_64F);
  graySmall.convertTo(cur64, CV_64F);
  cv::createHanningWindow(window, ref64.size(), CV_64F);
  // hazard: phaseCorrelate can overwrite its inputs; ref64/cur64 are per-call copies.
  const cv::Point2d shift = cv::phaseCorrelate(ref64, cur64, window);
  if (!std::isfinite(shift.x) || !std::isfinite(shift.y)) return out;

  cv::Mat H = (cv::Mat_<float>(3, 3) << 1, 0, static_cast<float>(shift.x), 0, 1,
               static_cast<float>(shift.y), 0, 0, 1);
  const auto cc = vendor::eccHomography(refSmall, graySmall, H, 100, 1e-5, 5);
  if (!cc || !std::isfinite(*cc)) return out;
  out.cc = *cc;
  if (*cc < kMinEccCorrelation) {
    out.status = WarpEstimate::Status::LowCorrelation;
    return out;
  }

  // why: lab computes np.linalg.inv(S) @ H @ S on float32 arrays; with a diagonal S that is
  // H[i][j] * (1/s or 1) * (s or 1), each product rounded to float32.
  const float sf = static_cast<float>(s);
  const float rowScale[3] = {1.0f / sf, 1.0f / sf, 1.0f};
  const float colScale[3] = {sf, sf, 1.0f};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      float& h = H.at<float>(i, j);
      h = (rowScale[i] * h) * colScale[j];
    }
  }
  out.status = WarpEstimate::Status::Ok;
  out.H = H;
  return out;
}

std::string dropReason(const WarpEstimate& estimate) {
  if (estimate.status == WarpEstimate::Status::LowCorrelation) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "ECC cc %.3f < %g, dropped", estimate.cc, kMinEccCorrelation);
    return buf;
  }
  return "ECC did not converge, dropped";
}

}  // namespace sr
