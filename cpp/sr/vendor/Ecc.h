#pragma once

#include <opencv2/core.hpp>

#include <optional>

namespace sr::vendor {

// invariant: same result as cv::findTransformECC(templ, input, warp, MOTION_HOMOGRAPHY,
// {COUNT|EPS, maxIter, eps}, noArray(), gaussFiltSize), except that every failure returns
// nullopt instead of raising CV_Error (opencv-mobile is built without exceptions, so the
// upstream error aborts the process, AD-004/AD-006). On failure `warp` is unspecified.
// templ and input are single-channel CV_32F; warp is a 3x3 CV_32F initial guess, refined in place.
std::optional<double> eccHomography(const cv::Mat& templ, const cv::Mat& input, cv::Mat& warp,
                                    int maxIter, double eps, int gaussFiltSize);

}  // namespace sr::vendor
