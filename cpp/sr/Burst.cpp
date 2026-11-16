#include "Burst.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

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

}  // namespace sr
