#pragma once

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace sr {

// invariant: long side of the gray image used for sharpness and alignment (lab align.SMALL).
inline constexpr int kSmallSide = 1024;

// invariant: lab align.gray_small. Gray of an 8-bit BGR frame resized by s = 1024 / long side
// with INTER_AREA (also when s > 1), as CV_32F; s is returned through `s`.
cv::Mat graySmall(const cv::Mat& bgr, double& s);

// invariant: lab align.sharpness. Variance of Laplacian(CV_32F) of a graySmall image.
double sharpness(const cv::Mat& graySmall32f);

// invariant: a frame left out of the mean; index is its position in paths, reason is the
// lab log text without the "frame NNN: " prefix.
struct Drop {
  int index;
  std::string reason;
};

// invariant: lab filters.MIN_KEEP; a filter that would keep fewer frames is not applied.
inline constexpr int kMinKeep = 3;

// invariant: lab filters.blur + apply_filters for one "blur=ratio" step. Drops frames whose
// sharpness is below ratio x median (np.median) unless fewer than kMinKeep would remain;
// appends the drops to `dropped` and returns the kept indices in ascending order.
std::vector<int> applyBlurFilter(const std::vector<double>& sharp, double ratio,
                                 std::vector<Drop>& dropped);

// invariant: lab align.pick_ref(first=8). Index (into sharp) of the sharpest frame among the
// first 8 entries of `kept` (kept non-empty); the earliest wins on ties, like np.argmax.
int pickReference(const std::vector<int>& kept, const std::vector<double>& sharp);

// invariant: lab run.py ordering. `kept` sorted by |index - ref|, earlier index first on ties
// (stable sort of an ascending list); ref comes first.
std::vector<int> temporalOrder(const std::vector<int>& kept, int ref);

}  // namespace sr
