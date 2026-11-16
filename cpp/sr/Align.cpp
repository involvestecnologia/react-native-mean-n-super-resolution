#include "Align.h"

#include "vendor/Ecc.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

// hazard: SPEC_DEVIATION from design step 2 (phaseCorrelate init + ECC Euclidean refine).
// opencv-mobile is built without C++ exceptions, so a non-converging findTransformECC
// (CV_Error StsNoConv) aborts the whole process instead of throwing. Alignment is
// therefore translation-only via phaseCorrelate, the fallback the design names for a
// missing video module, and a frame is accepted when the normalized cross-correlation
// at the estimated shift is >= 0.85. Rotation between frames is not compensated.

namespace sr {
namespace {

constexpr int kMaxAlignSide = 1024;
constexpr double kMinCorrelation = 0.85;
constexpr double kMinOverlapFraction = 0.25;

cv::Mat toAlignLevel(const cv::Mat& gray8, double d) {
  cv::Mat f;
  if (d < 1.0) {
    cv::Mat small;
    cv::resize(gray8, small, cv::Size(), d, d, cv::INTER_AREA);
    small.convertTo(f, CV_32F);
  } else {
    gray8.convertTo(f, CV_32F);
  }
  return f;
}

// invariant: returns NCC in [-1, 1] of ref(x) vs cur(x + shift) over their overlap,
// or -1 when the overlap is too small or either side is flat.
double correlationAtShift(const cv::Mat& ref, const cv::Mat& cur, cv::Point2d shift) {
  const cv::Mat W = (cv::Mat_<double>(2, 3) << 1, 0, shift.x, 0, 1, shift.y);
  cv::Mat warped;
  cv::warpAffine(cur, warped, W, ref.size(), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
                 cv::BORDER_REPLICATE);

  const int x0 = std::max(0, static_cast<int>(std::ceil(-shift.x)) + 1);
  const int y0 = std::max(0, static_cast<int>(std::ceil(-shift.y)) + 1);
  const int x1 = std::min(ref.cols, ref.cols - static_cast<int>(std::ceil(shift.x)) - 1);
  const int y1 = std::min(ref.rows, ref.rows - static_cast<int>(std::ceil(shift.y)) - 1);
  if (x1 - x0 <= 0 || y1 - y0 <= 0) return -1.0;
  const cv::Rect overlap(x0, y0, x1 - x0, y1 - y0);
  if (overlap.area() < kMinOverlapFraction * ref.size().area()) return -1.0;

  cv::Scalar mr, sdr, mc, sdc;
  cv::meanStdDev(ref(overlap), mr, sdr);
  cv::meanStdDev(warped(overlap), mc, sdc);
  if (sdr[0] < 1e-6 || sdc[0] < 1e-6) return -1.0;

  cv::Mat zr, zc;
  cv::subtract(ref(overlap), mr, zr);
  cv::subtract(warped(overlap), mc, zc);
  return zr.dot(zc) / (static_cast<double>(overlap.area()) * sdr[0] * sdc[0]);
}

}  // namespace

cv::Mat estimateTransform(const cv::Mat& refGray, const cv::Mat& gray, bool& ok) {
  ok = false;
  const cv::Mat identity = cv::Mat::eye(2, 3, CV_64F);

  // why: identical frames (including flat ones, where NCC is undefined) align trivially.
  if (cv::norm(refGray, gray, cv::NORM_INF) == 0) {
    ok = true;
    return identity;
  }

  const int longSide = std::max(refGray.cols, refGray.rows);
  const double d = longSide > kMaxAlignSide ? static_cast<double>(kMaxAlignSide) / longSide : 1.0;
  // invariant: createHanningWindow/phaseCorrelate assert on sides < 2 (AD-004: abort).
  if (std::lround(refGray.cols * d) < 2 || std::lround(refGray.rows * d) < 2) return identity;
  const cv::Mat ref = toAlignLevel(refGray, d);
  const cv::Mat cur = toAlignLevel(gray, d);

  cv::Mat window;
  cv::createHanningWindow(window, ref.size(), CV_32F);
  // hazard: phaseCorrelate multiplies its inputs by the window in place when they are
  // already DFT-optimal size, so it must get copies.
  const cv::Point2d shift = cv::phaseCorrelate(ref.clone(), cur.clone(), window);
  if (!std::isfinite(shift.x) || !std::isfinite(shift.y)) return identity;

  if (!(correlationAtShift(ref, cur, shift) >= kMinCorrelation)) return identity;

  // invariant: cur(x + shift) ~ ref(x), so frame->reference is a translation by -shift.
  ok = true;
  return (cv::Mat_<double>(2, 3) << 1, 0, -shift.x / d, 0, 1, -shift.y / d);
}

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
