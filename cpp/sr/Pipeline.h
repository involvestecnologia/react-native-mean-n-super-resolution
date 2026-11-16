#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sr {

// invariant: bounds shared with src/validate.ts (LP-API-02..04).
inline constexpr int kMinFrames = 3;
inline constexpr int kMaxFrames = 30;
inline constexpr int kMinN = 2;
inline constexpr int kMaxN = 30;

struct Request {
  std::vector<std::string> paths;
  // invariant: frames averaged, reference included (lab mean_N); fewer if fewer align.
  int n = 8;
  // invariant: blur filter ratio (lab filters.blur); 0 turns the filter off.
  double blur = 0.8;
  int scale = 2;
  std::optional<std::string> outputPath;
  std::string defaultOutputDir;
};

struct Result {
  std::string path;
  int width = 0;
  int height = 0;
  int framesUsed = 0;
};

// invariant: run() rejects with E_INTERNAL before allocating output-sized buffers
// when estimatePeakBytes() of the decoded reference exceeds this budget.
inline constexpr std::uint64_t kMemoryBudgetBytes = std::uint64_t{1} << 30;

// invariant: s^2*w*h*(3*4 + 3) (CV_32FC3 accumulator + one CV_8UC3 output-sized
// buffer) + 3*w*h (decoded frame) + 2*w*h (reference and current gray).
std::uint64_t estimatePeakBytes(int width, int height, int scale);

// invariant: working set of the ECC on a graySmall image (jacobian of 8 a-sized float planes
// plus ~15 a-sized float temporaries, a <= 1024 x 1024), rounded up.
inline constexpr std::uint64_t kEccWorkBytes = std::uint64_t{96} << 20;

// invariant: max of the two phases of run(), with p = w*h and a = graySmall area:
//   accumulate: frames*4a (graySmall per frame) + 12p (CV_32FC3 accumulator)
//               + 6p (decoded + warped frame) + kEccWorkBytes
//   finalize:   3p (mean) + 3*s^2*p (upscaled output)
std::uint64_t estimatePeakBytes(int width, int height, int scale, int frames);

// invariant: lab methods.mean, np.mean(dtype=float32).round().astype(uint8): each element of
// the CV_32FC3 sum of `count` uint8 frames becomes nearbyint(float(sum) / float(count)) (half
// to even), saturated to [0, 255], in a CV_8UC3 image of the same size.
void meanRoundHalfEven(const cv::Mat& acc, int count, cv::Mat& out);

// invariant: throws SrError(E_INVALID_ARGUMENT) iff the request breaks the JS API rules.
void validate(const Request& request);

// invariant: reports every failure as SrError; codes live in Errors.h.
Result run(const Request& request);

}  // namespace sr
