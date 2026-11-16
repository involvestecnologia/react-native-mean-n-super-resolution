#pragma once

#include <opencv2/core.hpp>

namespace sr {

// invariant: long side of the gray image used for sharpness and alignment (lab align.SMALL).
inline constexpr int kSmallSide = 1024;

// invariant: lab align.gray_small. Gray of an 8-bit BGR frame resized by s = 1024 / long side
// with INTER_AREA (also when s > 1), as CV_32F; s is returned through `s`.
cv::Mat graySmall(const cv::Mat& bgr, double& s);

// invariant: lab align.sharpness. Variance of Laplacian(CV_32F) of a graySmall image.
double sharpness(const cv::Mat& graySmall32f);

}  // namespace sr
