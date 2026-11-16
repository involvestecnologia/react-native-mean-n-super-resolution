#include "Burst.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>

namespace sr {

cv::Mat graySmall(const cv::Mat& bgr, double& s) {
  s = static_cast<double>(kSmallSide) / std::max(bgr.cols, bgr.rows);
  cv::Mat gray, small, out;
  cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
  cv::resize(gray, small, cv::Size(), s, s, cv::INTER_AREA);
  small.convertTo(out, CV_32F);
  return out;
}

double sharpness(const cv::Mat& graySmall32f) {
  cv::Mat lap;
  cv::Laplacian(graySmall32f, lap, CV_32F);
  cv::Scalar mean, sd;
  cv::meanStdDev(lap, mean, sd);
  return sd[0] * sd[0];
}

namespace {

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

}  // namespace

std::vector<int> applyBlurFilter(const std::vector<double>& sharp, double ratio,
                                 std::vector<Drop>& dropped) {
  const int n = static_cast<int>(sharp.size());
  std::vector<int> all(n);
  for (int i = 0; i < n; ++i) all[i] = i;

  const double med = median(sharp);
  std::vector<int> kept;
  std::vector<Drop> drops;
  for (int i = 0; i < n; ++i) {
    if (sharp[i] < ratio * med) {
      // why: same text as the lab log, f"sharpness {s / med:.2f} of median < {ratio:g}".
      char buf[96];
      std::snprintf(buf, sizeof buf, "sharpness %.2f of median < %g, dropped (blur)",
                    sharp[i] / med, ratio);
      drops.push_back({i, buf});
    } else {
      kept.push_back(i);
    }
  }
  if (static_cast<int>(kept.size()) < kMinKeep) return all;
  dropped.insert(dropped.end(), drops.begin(), drops.end());
  return kept;
}

}  // namespace sr
