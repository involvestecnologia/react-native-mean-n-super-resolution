#include "Pipeline.h"

#include "Align.h"
#include "Burst.h"
#include "Errors.h"
#include "Paths.h"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace sr {
namespace {

std::string sizeText(const cv::Size& s) {
  return std::to_string(s.width) + "x" + std::to_string(s.height);
}

cv::Mat decodeFrame(const std::string& pathOrUri, size_t index) {
  const std::string fsPath = toFsPath(pathOrUri);
  cv::Mat img = cv::imread(fsPath, cv::IMREAD_COLOR);
  if (img.empty() || img.cols <= 0 || img.rows <= 0) {
    throw SrError(code::DecodeFailed,
                  "index=" + std::to_string(index) + " could not decode image: " + fsPath);
  }
  // invariant: every later OpenCV call assumes 8-bit BGR (AD-004: asserts abort).
  if (img.type() != CV_8UC3) {
    throw SrError(code::Internal, "index=" + std::to_string(index) +
                                      " decoded to unexpected type " + std::to_string(img.type()));
  }
  return img;
}

void encodeTo(const std::string& fsPath, const cv::Mat& img) {
  // why: opencv-mobile's imwrite matches extensions case-sensitively (.Jpg fails),
  // so encode with the lower-cased extension and write the bytes ourselves.
  const std::string ext = lowerExt(fsPath);
  std::vector<int> params;
  if (ext == ".jpg" || ext == ".jpeg") params = {cv::IMWRITE_JPEG_QUALITY, 95};

  std::vector<uchar> bytes;
  if (!cv::imencode(ext, img, bytes, params)) {
    throw SrError(code::WriteFailed, "could not encode " + ext);
  }

  std::ofstream out(fsPath, std::ios::binary | std::ios::trunc);
  if (!out) throw SrError(code::WriteFailed, "could not open for writing: " + fsPath);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw SrError(code::WriteFailed, "could not write: " + fsPath);
}

void checkSize(const cv::Mat& frame, const cv::Size& expected, size_t index) {
  if (frame.size() != expected) {
    throw SrError(code::SizeMismatch, "index=" + std::to_string(index) + " is " +
                                          sizeText(frame.size()) + ", expected " +
                                          sizeText(expected));
  }
}

}  // namespace

std::uint64_t estimatePeakBytes(int width, int height, int scale, int frames) {
  const double s = static_cast<double>(kSmallSide) / std::max(width, height);
  const std::uint64_t a = static_cast<std::uint64_t>(std::lround(width * s)) *
                          static_cast<std::uint64_t>(std::lround(height * s));
  const std::uint64_t p = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  const std::uint64_t s2 = static_cast<std::uint64_t>(scale) * static_cast<std::uint64_t>(scale);
  const std::uint64_t accumulate =
      static_cast<std::uint64_t>(frames) * 4 * a + 12 * p + 6 * p + kEccWorkBytes;
  const std::uint64_t finalize = 3 * p + 3 * s2 * p;
  return std::max(accumulate, finalize);
}

void meanRoundHalfEven(const cv::Mat& acc, int count, cv::Mat& out) {
  out.create(acc.size(), CV_8UC3);
  const float n = static_cast<float>(count);
  const int width = acc.cols * 3;
  for (int r = 0; r < acc.rows; ++r) {
    const float* src = acc.ptr<float>(r);
    uchar* dst = out.ptr<uchar>(r);
    for (int c = 0; c < width; ++c) {
      // why: std::nearbyint rounds half to even in the default rounding mode, like np.round.
      const float v = std::nearbyint(src[c] / n);
      dst[c] = static_cast<uchar>(std::clamp(v, 0.0f, 255.0f));
    }
  }
}

Result run(const Request& request, Trace* trace) {
  validate(request);

  const std::string outFs =
      request.outputPath ? toFsPath(*request.outputPath) : uniqueOutputPath(request.defaultOutputDir);
  const size_t frames = request.paths.size();

  // Pass 1: one decoded frame alive at a time; only its graySmall and sharpness are kept.
  cv::Size size;
  double s = 0;
  std::vector<cv::Mat> small(frames);
  std::vector<double> sharp(frames);
  for (size_t i = 0; i < frames; ++i) {
    const cv::Mat frame = decodeFrame(request.paths[i], i);
    if (i == 0) {
      size = frame.size();
      const std::uint64_t peak = estimatePeakBytes(size.width, size.height, request.scale,
                                                   static_cast<int>(frames));
      if (peak > kMemoryBudgetBytes) {
        throw SrError(code::Internal, "estimated memory " + std::to_string(peak >> 20) +
                                          " MiB exceeds budget of " +
                                          std::to_string(kMemoryBudgetBytes >> 20) + " MiB");
      }
      // invariant: cv::resize asserts on an empty target size, and phaseCorrelate/ECC need
      // sides >= 2 (AD-004: asserts abort), so such thin frames never reach graySmall.
      const double r = static_cast<double>(kSmallSide) / std::max(size.width, size.height);
      if (std::lround(size.width * r) < 2 || std::lround(size.height * r) < 2) {
        throw SrError(code::AlignmentFailed, "frames of " + sizeText(size) + " are too thin to align");
      }
    } else {
      checkSize(frame, size, i);
    }
    small[i] = graySmall(frame, s);
    sharp[i] = sharpness(small[i]);
  }

  Result result;
  // invariant: blur 0 keeps every frame (no sharpness is below 0 x median).
  const std::vector<int> kept = applyBlurFilter(sharp, request.blur, result.dropped);
  const int ref = pickReference(kept, sharp);
  const std::vector<int> order = temporalOrder(kept, ref);

  // Pass 2: frames in temporal order until n are accepted; only accepted frames are decoded again.
  cv::Mat acc = cv::Mat::zeros(size, CV_32FC3);
  for (const int i : order) {
    if (static_cast<int>(result.used.size()) == request.n) break;
    cv::Mat H;
    if (i != ref) {
      const WarpEstimate e = estimateWarp(small[ref], small[i], s);
      if (e.status != WarpEstimate::Status::Ok) {
        result.dropped.push_back({i, dropReason(e)});
        continue;
      }
      H = e.H;
      if (trace) trace->push_back({i, e.H, e.cc});
    }

    const cv::Mat frame = decodeFrame(request.paths[i], i);
    // why: the file may have changed since pass 1.
    checkSize(frame, size, i);
    if (H.empty()) {
      cv::accumulate(frame, acc);
    } else {
      cv::Mat warped;
      cv::warpPerspective(frame, warped, H, size, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
                          cv::BORDER_REPLICATE);
      cv::accumulate(warped, acc);
    }
    result.used.push_back(i);
  }
  small.clear();

  if (result.used.size() < 2) {
    std::string msg = "only " + std::to_string(result.used.size()) +
                      " frame(s) usable, need at least 2; dropped:";
    for (const Drop& d : result.dropped) msg += " index=" + std::to_string(d.index) + " " + d.reason + ";";
    throw SrError(code::AlignmentFailed, msg);
  }

  cv::Mat out;
  meanRoundHalfEven(acc, static_cast<int>(result.used.size()), out);
  acc.release();
  if (request.scale > 1) {
    cv::Mat up;
    cv::resize(out, up, cv::Size(), request.scale, request.scale, cv::INTER_CUBIC);
    out = up;
  }

  encodeTo(outFs, out);
  result.path = toFileUri(outFs);
  result.width = out.cols;
  result.height = out.rows;
  result.framesUsed = static_cast<int>(result.used.size());
  result.refIndex = ref;
  return result;
}

}  // namespace sr
