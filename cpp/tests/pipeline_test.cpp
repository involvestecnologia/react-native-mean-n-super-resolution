#include <gtest/gtest.h>

#include "../platform/TempDir.h"
#include "../sr/Align.h"
#include "../sr/Errors.h"
#include "../sr/Paths.h"
#include "../sr/Pipeline.h"
#include "fixtures.h"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kW = 64;
constexpr int kH = 48;

sr::Request requestFor(const std::vector<std::string>& paths, const fs::path& outDir) {
  sr::Request r;
  r.paths = paths;
  r.defaultOutputDir = outDir.string();
  return r;
}

std::vector<std::string> identicalFrames(const fs::path& dir, int n = 5) {
  const cv::Mat img = fixtures::texturedImage(kW, kH);
  std::vector<std::string> paths;
  for (int i = 0; i < n; ++i) {
    paths.push_back(fixtures::writePng(dir, "f" + std::to_string(i) + ".png", img));
  }
  return paths;
}

// invariant: texture squeezed into [30, 221], so the offsets and gains below never clip.
cv::Mat baseImage(int w, int h, std::uint64_t seed = 42) {
  cv::Mat out;
  fixtures::texturedImage(w, h, seed).convertTo(out, -1, 0.75, 30);
  return out;
}

// why: ECC is invariant to brightness offsets, so offset frames align with H = I exactly
// while still making the mean depend on which frames were used (L-001).
cv::Mat plus(const cv::Mat& img, int c) { return img + cv::Scalar::all(c); }

// invariant: same texture rotated by 180 degrees, contrast 0.95 (sharpness ~0.9 of the
// original: passes the 0.8 blur filter, never the sharpest, never aligns).
cv::Mat unrelated(const cv::Mat& img) {
  cv::Mat r, out;
  cv::rotate(img, r, cv::ROTATE_180);
  r.convertTo(out, -1, 0.95, 6);
  return out;
}

// invariant: per element nearbyint(sum / count) of frames[idx], computed in double.
cv::Mat expectedMean(const std::vector<cv::Mat>& frames, const std::vector<int>& idx) {
  cv::Mat sum = cv::Mat::zeros(frames[0].size(), CV_32SC3);
  for (int i : idx) cv::add(sum, frames[i], sum, cv::noArray(), CV_32SC3);
  cv::Mat out(sum.size(), CV_8UC3);
  for (int r = 0; r < sum.rows; ++r) {
    for (int c = 0; c < sum.cols * 3; ++c) {
      out.ptr<uchar>(r)[c] = static_cast<uchar>(
          std::nearbyint(static_cast<double>(sum.ptr<int>(r)[c]) / static_cast<double>(idx.size())));
    }
  }
  return out;
}

double maxAbsDiff(const cv::Mat& a, const cv::Mat& b) {
  if (a.size() != b.size() || a.type() != b.type()) return -1;
  return cv::norm(a, b, cv::NORM_INF);
}

cv::Mat readOut(const sr::Result& res) { return cv::imread(sr::toFsPath(res.path), cv::IMREAD_COLOR); }

std::vector<int> dropIndices(const sr::Result& res) {
  std::vector<int> out;
  for (const auto& d : res.dropped) out.push_back(d.index);
  return out;
}

const char* const kEccReason = R"(ECC did not converge, dropped|ECC cc -?\d\.\d{3} < 0\.9, dropped)";

sr::SrError runExpectingError(const sr::Request& r) {
  try {
    sr::run(r);
  } catch (const sr::SrError& e) {
    return e;
  }
  return sr::SrError("<none>", "run() did not throw");
}

bool startsWith(const std::vector<unsigned char>& bytes, const std::vector<unsigned char>& magic) {
  return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

const std::vector<unsigned char> kPngMagic = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
const std::vector<unsigned char> kJpegMagic = {0xFF, 0xD8, 0xFF};

}  // namespace

// LP-CORE-09 / LP-API-01: identical frames at the default scale give the input back.
TEST(Pipeline, IdenticalFramesAtDefaultScaleEqualTheInput) {
  const auto dir = fixtures::freshDir();
  const auto paths = identicalFrames(dir, 10);
  const cv::Mat input = cv::imread(paths[0], cv::IMREAD_COLOR);
  for (const int n : {8, 30}) {
    auto r = requestFor(paths, dir);
    r.n = n;
    r.outputPath = (dir / ("out" + std::to_string(n) + ".png")).string();

    const sr::Result res = sr::run(r);

    EXPECT_EQ(res.path.rfind("file://", 0), 0u) << res.path;
    EXPECT_EQ(res.width, kW);
    EXPECT_EQ(res.height, kH);
    const size_t expectedUsed = std::min<size_t>(n, paths.size());
    EXPECT_EQ(res.used.size(), expectedUsed) << n;
    EXPECT_EQ(res.framesUsed, static_cast<int>(expectedUsed)) << n;
    ASSERT_FALSE(res.used.empty());
    EXPECT_EQ(res.used[0], res.refIndex);
    EXPECT_EQ(res.refIndex, 0);  // sharpness ties: first frame wins
    EXPECT_TRUE(res.dropped.empty());
    EXPECT_EQ(maxAbsDiff(readOut(res), input), 0.0) << n;
  }
}

// LP-CORE-01 / L-001: a blurred frame is dropped with the lab text and left out of the mean.
TEST(Pipeline, BlurredFrameIsDroppedAndLeftOutOfTheMean) {
  const auto dir = fixtures::freshDir();
  const cv::Mat base = baseImage(128, 96);
  std::vector<cv::Mat> frames;
  for (int i = 0; i < 10; ++i) frames.push_back(plus(base, i));
  cv::GaussianBlur(frames[3], frames[3], cv::Size(0, 0), 1.5);
  auto r = requestFor(fixtures::writeFrames(dir, frames), dir);
  r.outputPath = (dir / "out.png").string();

  const sr::Result res = sr::run(r);

  ASSERT_EQ(res.dropped.size(), 1u);
  EXPECT_EQ(res.dropped[0].index, 3);
  EXPECT_TRUE(std::regex_match(res.dropped[0].reason,
                               std::regex(R"(sharpness 0\.\d\d of median < 0\.8, dropped \(blur\))")))
      << res.dropped[0].reason;
  EXPECT_EQ(res.refIndex, 0);
  const std::vector<int> used = {0, 1, 2, 4, 5, 6, 7, 8};
  EXPECT_EQ(res.used, used);
  EXPECT_EQ(maxAbsDiff(readOut(res), expectedMean(frames, used)), 0.0);
}

// LP-CORE-05 / L-001: an unrelated frame is dropped by ECC and left out of the mean.
TEST(Pipeline, UnrelatedFrameIsDroppedByEccAndLeftOutOfTheMean) {
  const auto dir = fixtures::freshDir();
  const cv::Mat base = baseImage(128, 96);
  std::vector<cv::Mat> frames;
  for (int i = 0; i < 6; ++i) frames.push_back(plus(base, i));
  frames[1] = unrelated(frames[1]);
  auto r = requestFor(fixtures::writeFrames(dir, frames), dir);
  r.outputPath = (dir / "out.png").string();

  const sr::Result res = sr::run(r);

  EXPECT_EQ(res.refIndex, 0);
  ASSERT_EQ(res.dropped.size(), 1u);
  EXPECT_EQ(res.dropped[0].index, 1);
  EXPECT_TRUE(std::regex_match(res.dropped[0].reason, std::regex(kEccReason))) << res.dropped[0].reason;
  const std::vector<int> used = {0, 2, 3, 4, 5};
  EXPECT_EQ(res.used, used);
  EXPECT_EQ(maxAbsDiff(readOut(res), expectedMean(frames, used)), 0.0);
}

// LP-CORE-02/03/04: the sharpest frame is the reference and fixes the output geometry; frames
// are visited by temporal distance and the run stops after n accepted (index 9, unrelated,
// is never visited, so it is not dropped).
TEST(Pipeline, StopsAfterNFramesInTemporalOrderFromTheSharpest) {
  constexpr int w = 128, h = 96, m = 8;
  const std::vector<cv::Point> shifts = {{0, 0},  {2, 1},  {-3, 2}, {1, -2}, {-2, -1},
                                         {3, 3},  {-1, 2}, {2, -3}, {0, 1},  {0, 0}};
  const cv::Mat scene = baseImage(w + 2 * m, h + 2 * m, 7);
  std::vector<cv::Mat> frames;
  for (const auto& d : shifts) frames.push_back(scene(cv::Rect(m - d.x, m - d.y, w, h)).clone());
  frames[5].convertTo(frames[5], -1, 1.1, -12.8);  // sharpest (higher contrast)
  frames[9] = unrelated(frames[9]);
  const auto dir = fixtures::freshDir();
  auto r = requestFor(fixtures::writeFrames(dir, frames), dir);
  r.n = 3;
  r.outputPath = (dir / "out.png").string();

  sr::Trace trace;
  const sr::Result res = sr::run(r, &trace);

  EXPECT_EQ(res.refIndex, 5);
  EXPECT_EQ(res.used, (std::vector<int>{5, 4, 6}));
  EXPECT_EQ(res.framesUsed, 3);
  EXPECT_TRUE(res.dropped.empty());
  ASSERT_EQ(trace.size(), 2u);
  for (size_t k = 0; k < trace.size(); ++k) {
    EXPECT_EQ(trace[k].index, res.used[k + 1]);
    EXPECT_EQ(trace[k].H.type(), CV_32FC1);
    EXPECT_EQ(trace[k].H.size(), cv::Size(3, 3));
    EXPECT_GE(trace[k].cc, sr::kMinEccCorrelation);
  }

  // invariant: the output is registered to frame 5, not to paths[0].
  const auto gray64 = [](const cv::Mat& bgr) {
    cv::Mat g, f;
    cv::cvtColor(bgr, g, cv::COLOR_BGR2GRAY);
    g.convertTo(f, CV_64F);
    return f;
  };
  const cv::Mat out = gray64(readOut(res));
  ASSERT_EQ(out.size(), cv::Size(w, h));
  cv::Mat window;
  cv::createHanningWindow(window, out.size(), CV_64F);
  const cv::Point2d toRef = cv::phaseCorrelate(out.clone(), gray64(frames[5]), window);
  const cv::Point2d toFirst = cv::phaseCorrelate(out.clone(), gray64(frames[0]), window);
  EXPECT_LT(cv::norm(toRef), 0.25) << toRef;
  EXPECT_NEAR(cv::norm(toFirst), cv::norm(shifts[5] - shifts[0]), 0.25) << toFirst;
}

// LP-CORE-08: scale > 1 is INTER_CUBIC of the mean, after averaging.
TEST(Pipeline, ScaleTwoIsBicubicOfTheMean) {
  const auto dir = fixtures::freshDir();
  auto r = requestFor(identicalFrames(dir), dir);
  r.scale = 2;
  r.outputPath = (dir / "out.png").string();

  const sr::Result res = sr::run(r);

  EXPECT_EQ(res.width, 2 * kW);
  EXPECT_EQ(res.height, 2 * kH);
  cv::Mat expected;
  cv::resize(cv::imread(r.paths[0], cv::IMREAD_COLOR), expected, cv::Size(), 2, 2, cv::INTER_CUBIC);
  EXPECT_EQ(maxAbsDiff(readOut(res), expected), 0.0);
}

// invariant: SR-CFG-03 / L-002 — `.jpg` output is JPEG at quality 95; identical frames make
// the mean equal the reference, so the encoded bytes are fully determined.
TEST(Pipeline, JpegOutputIsEncodedAtQuality95) {
  const auto dir = fixtures::freshDir();
  auto r = requestFor(identicalFrames(dir), dir);
  r.outputPath = (dir / "out.jpg").string();

  sr::run(r);

  const cv::Mat expected = cv::imread(r.paths[0], cv::IMREAD_COLOR);
  std::vector<uchar> expectedBytes;
  ASSERT_TRUE(cv::imencode(".jpg", expected, expectedBytes, {cv::IMWRITE_JPEG_QUALITY, 95}));

  const auto got = fixtures::readBytes(*r.outputPath);
  EXPECT_EQ(got.size(), expectedBytes.size());
  EXPECT_TRUE(std::equal(got.begin(), got.end(), expectedBytes.begin(), expectedBytes.end()));
}

TEST(Pipeline, ScaleControlsOutputDimensions) {
  const auto dir = fixtures::freshDir();
  const auto paths = identicalFrames(dir);
  for (int s : {2, 3, 4}) {
    auto r = requestFor(paths, dir);
    r.scale = s;
    r.outputPath = (dir / ("out" + std::to_string(s) + ".png")).string();
    const sr::Result res = sr::run(r);
    EXPECT_EQ(res.width, s * kW) << s;
    EXPECT_EQ(res.height, s * kH) << s;
    const cv::Mat out = cv::imread(sr::toFsPath(res.path), cv::IMREAD_COLOR);
    EXPECT_EQ(out.cols, s * kW) << s;
    EXPECT_EQ(out.rows, s * kH) << s;
  }
}

TEST(Pipeline, OutputFormatFollowsExtension) {
  const auto dir = fixtures::freshDir();
  const auto paths = identicalFrames(dir);
  const struct {
    const char* name;
    const std::vector<unsigned char>* magic;
  } cases[] = {{"o.png", &kPngMagic}, {"o.jpg", &kJpegMagic}, {"o.jpeg", &kJpegMagic},
               {"o.Jpg", &kJpegMagic}, {"o.PNG", &kPngMagic}};
  for (const auto& c : cases) {
    auto r = requestFor(paths, dir);
    r.outputPath = (dir / c.name).string();
    const sr::Result res = sr::run(r);
    EXPECT_EQ(res.path, sr::toFileUri(*r.outputPath)) << c.name;
    EXPECT_TRUE(startsWith(fixtures::readBytes(*r.outputPath), *c.magic)) << c.name;
  }
}

TEST(Pipeline, OutputPathAcceptsFileUri) {
  const auto dir = fixtures::freshDir();
  auto r = requestFor(identicalFrames(dir), dir);
  const std::string target = (dir / "uri out.png").string();
  r.outputPath = sr::toFileUri(target);
  const sr::Result res = sr::run(r);
  EXPECT_EQ(res.path, sr::toFileUri(target));
  EXPECT_TRUE(fs::exists(target));
}

TEST(Pipeline, OverwritesExistingOutput) {
  const auto dir = fixtures::freshDir();
  auto r = requestFor(identicalFrames(dir), dir);
  r.outputPath = (dir / "out.png").string();
  { std::ofstream(*r.outputPath) << "stale contents"; }

  sr::run(r);

  EXPECT_TRUE(startsWith(fixtures::readBytes(*r.outputPath), kPngMagic));
  const cv::Mat out = cv::imread(*r.outputPath, cv::IMREAD_COLOR);
  EXPECT_EQ(out.cols, kW);
  EXPECT_EQ(out.rows, kH);
}

TEST(Pipeline, MissingFileFailsDecodeWithIndex) {
  const auto dir = fixtures::freshDir();
  auto paths = identicalFrames(dir);
  paths[2] = (dir / "does-not-exist.png").string();
  const auto e = runExpectingError(requestFor(paths, dir));
  EXPECT_EQ(e.code, "E_DECODE_FAILED");
  EXPECT_NE(e.message.find("index=2"), std::string::npos) << e.message;
}

TEST(Pipeline, TextFileFailsDecodeWithIndex) {
  const auto dir = fixtures::freshDir();
  auto paths = identicalFrames(dir);
  const auto txt = dir / "notes.txt";
  { std::ofstream(txt) << "this is not an image\n"; }
  paths[1] = txt.string();
  const auto e = runExpectingError(requestFor(paths, dir));
  EXPECT_EQ(e.code, "E_DECODE_FAILED");
  EXPECT_NE(e.message.find("index=1"), std::string::npos) << e.message;
}

// LP-API-07: sizes are checked in pass 1, so a mismatch is reported even for a frame
// that pass 2 would never visit (n = 2 is reached at frame 1).
TEST(Pipeline, DifferentSizeFrameFailsWithIndexBeforeAnyWarp) {
  const auto dir = fixtures::freshDir();
  auto paths = identicalFrames(dir);
  paths[4] = fixtures::writePng(dir, "small.png", fixtures::texturedImage(kW - 8, kH));
  auto r = requestFor(paths, dir);
  r.n = 2;
  r.outputPath = (dir / "out.png").string();
  const auto e = runExpectingError(r);
  EXPECT_EQ(e.code, "E_SIZE_MISMATCH");
  EXPECT_NE(e.message.find("index=4"), std::string::npos) << e.message;
  EXPECT_FALSE(fs::exists(*r.outputPath));
}

// LP-API-07: when every other frame is dropped the call rejects and lists the drops.
TEST(Pipeline, OnlyTheReferenceAlignedFails) {
  const auto dir = fixtures::freshDir();
  const cv::Mat base = baseImage(128, 96);
  const auto paths = fixtures::writeFrames(dir, {base, unrelated(base), unrelated(plus(base, 2))});
  auto r = requestFor(paths, dir);
  r.outputPath = (dir / "out.png").string();

  const auto e = runExpectingError(r);

  EXPECT_EQ(e.code, "E_ALIGNMENT_FAILED");
  EXPECT_NE(e.message.find("only 1 frame(s) usable"), std::string::npos) << e.message;
  EXPECT_NE(e.message.find("index=1 ECC"), std::string::npos) << e.message;
  EXPECT_NE(e.message.find("index=2 ECC"), std::string::npos) << e.message;
  EXPECT_FALSE(fs::exists(*r.outputPath));
}

TEST(Pipeline, MissingOutputDirectoryFailsWrite) {
  const auto dir = fixtures::freshDir();
  auto r = requestFor(identicalFrames(dir), dir);
  r.outputPath = (dir / "no-such-dir" / "out.jpg").string();
  const auto e = runExpectingError(r);
  EXPECT_EQ(e.code, "E_WRITE_FAILED");
}

TEST(Pipeline, GrayAndAlphaInputsYieldThreeChannelOutput) {
  const auto dir = fixtures::freshDir();
  const cv::Mat color = fixtures::texturedImage(kW, kH);
  cv::Mat gray, rgba;
  cv::cvtColor(color, gray, cv::COLOR_BGR2GRAY);
  cv::cvtColor(color, rgba, cv::COLOR_BGR2BGRA);

  for (const auto& [name, img] : {std::pair<std::string, cv::Mat>{"gray", gray}, {"rgba", rgba}}) {
    std::vector<std::string> paths;
    for (int i = 0; i < 5; ++i) {
      paths.push_back(fixtures::writePng(dir, name + std::to_string(i) + ".png", img));
    }
    auto r = requestFor(paths, dir);
    r.outputPath = (dir / (name + "-out.png")).string();
    sr::run(r);
    const cv::Mat out = cv::imread(*r.outputPath, cv::IMREAD_UNCHANGED);
    ASSERT_FALSE(out.empty()) << name;
    EXPECT_EQ(out.channels(), 3) << name;
    EXPECT_EQ(out.depth(), CV_8U) << name;
  }
}

TEST(Pipeline, SamePathRepeatedIsValid) {
  const auto dir = fixtures::freshDir();
  const auto one = fixtures::writePng(dir, "only.png", fixtures::texturedImage(kW, kH));
  auto r = requestFor(std::vector<std::string>(5, one), dir);
  r.outputPath = (dir / "out.png").string();
  EXPECT_EQ(sr::run(r).framesUsed, 5);
}

TEST(Pipeline, RepeatedRunsAreByteIdentical) {
  const auto dir = fixtures::freshDir();
  const auto paths = identicalFrames(dir);
  auto a = requestFor(paths, dir);
  a.outputPath = (dir / "a.jpg").string();
  auto b = requestFor(paths, dir);
  b.outputPath = (dir / "b.jpg").string();
  sr::run(a);
  sr::run(b);
  const auto ba = fixtures::readBytes(*a.outputPath);
  ASSERT_FALSE(ba.empty());
  EXPECT_EQ(ba, fixtures::readBytes(*b.outputPath));
}

TEST(Pipeline, ConcurrentDefaultOutputsAreDistinctJpegsInTempDir) {
  const auto dir = fixtures::freshDir();
  const auto paths = identicalFrames(dir);
  const std::string tempDir = sr::platform::defaultOutputDir();

  sr::Result r1, r2;
  std::thread t1([&] { r1 = sr::run(requestFor(paths, tempDir)); });
  std::thread t2([&] { r2 = sr::run(requestFor(paths, tempDir)); });
  t1.join();
  t2.join();

  EXPECT_NE(r1.path, r2.path);
  for (const auto& res : {r1, r2}) {
    const fs::path p = sr::toFsPath(res.path);
    EXPECT_TRUE(fs::exists(p)) << p;
    EXPECT_TRUE(fs::equivalent(p.parent_path(), tempDir)) << p;
    EXPECT_TRUE(startsWith(fixtures::readBytes(p), kJpegMagic)) << p;
    fs::remove(p);
  }
}

// Edge case (memory): design formula, max of accumulate and finalize phases.
TEST(PipelinePreflight, LabEstimateMatchesFormula) {
  // 4000x3000 -> graySmall 1024x768 (a = 786432), p = 12e6
  // scale 4, 30 frames: accumulate 30*4a + 18p + 96 MiB = 411035136; finalize 3p + 48p = 612000000
  EXPECT_EQ(sr::estimatePeakBytes(4000, 3000, 4, 30), 612000000u);
  // scale 1, 3 frames: accumulate 3*4a + 18p + 96 MiB = 326100480; finalize 6p = 72000000
  EXPECT_EQ(sr::estimatePeakBytes(4000, 3000, 1, 3), 326100480u);
}

TEST(PipelinePreflight, LabEstimateBudgetBoundaries) {
  EXPECT_LT(sr::estimatePeakBytes(4000, 3000, 4, 30), sr::kMemoryBudgetBytes);  // 12 MP
  EXPECT_GT(sr::estimatePeakBytes(8160, 6120, 4, 30), sr::kMemoryBudgetBytes);  // 50 MP
  EXPECT_LT(sr::estimatePeakBytes(6000, 4000, 1, 30), sr::kMemoryBudgetBytes);  // 24 MP, 1x
}

// LP-CORE-07: np.mean(dtype=float32).round() rounds half to even.
TEST(MeanRoundHalfEven, RoundsHalfToEven) {
  cv::Mat acc(1, 4, CV_32FC3);
  const float sums[4] = {5, 7, 9, 11};
  for (int i = 0; i < 4; ++i) acc.at<cv::Vec3f>(0, i) = cv::Vec3f(sums[i], sums[i], sums[i]);
  cv::Mat out;
  sr::meanRoundHalfEven(acc, 2, out);
  ASSERT_EQ(out.type(), CV_8UC3);
  const uchar expected[4] = {2, 4, 4, 6};
  for (int i = 0; i < 4; ++i) EXPECT_EQ(out.at<cv::Vec3b>(0, i)[1], expected[i]) << i;
}

TEST(MeanRoundHalfEven, DividesByAnyCount) {
  cv::Mat acc(1, 3, CV_32FC3);
  acc.at<cv::Vec3f>(0, 0) = cv::Vec3f(15, 21, 0);        // /6 -> 2.5, 3.5, 0
  acc.at<cv::Vec3f>(0, 1) = cv::Vec3f(16, 20, 6 * 255);  // /6 -> 2.67, 3.33, 255
  acc.at<cv::Vec3f>(0, 2) = cv::Vec3f(1, 5, 3);          // /6 -> 0.17, 0.83, 0.5
  cv::Mat out;
  sr::meanRoundHalfEven(acc, 6, out);
  EXPECT_EQ(out.at<cv::Vec3b>(0, 0), cv::Vec3b(2, 4, 0));
  EXPECT_EQ(out.at<cv::Vec3b>(0, 1), cv::Vec3b(3, 3, 255));
  EXPECT_EQ(out.at<cv::Vec3b>(0, 2), cv::Vec3b(0, 1, 0));
}

TEST(MeanRoundHalfEven, ThirtyFullFramesStayAt255) {
  const cv::Mat acc(2, 2, CV_32FC3, cv::Scalar::all(30 * 255));
  cv::Mat out;
  sr::meanRoundHalfEven(acc, 30, out);
  EXPECT_EQ(cv::countNonZero(out.reshape(1) != 255), 0);
}

namespace {

// invariant: 22 MP (51p > 1 GiB at scale 4, < 1 GiB at scale 1) with a textured centre so
// ECC converges; the flat rest keeps the PNG small.
std::string largeFrame(const fs::path& dir) {
  cv::Mat img(4400, 5000, CV_8UC3, cv::Scalar(90, 120, 150));
  fixtures::texturedImage(1000, 1000).copyTo(img(cv::Rect(2000, 1700, 1000, 1000)));
  return fixtures::writePng(dir, "big.png", img);
}

}  // namespace

TEST(PipelinePreflight, OverBudgetBurstRejectsInternalWithoutOutput) {
  ASSERT_GT(sr::estimatePeakBytes(5000, 4400, 4, 3), sr::kMemoryBudgetBytes);
  const auto dir = fixtures::freshDir();
  auto r = requestFor(std::vector<std::string>(3, largeFrame(dir)), dir);
  r.scale = 4;
  r.outputPath = (dir / "out.jpg").string();

  const auto e = runExpectingError(r);

  EXPECT_EQ(e.code, "E_INTERNAL");
  EXPECT_NE(e.message.find("exceeds budget"), std::string::npos) << e.message;
  EXPECT_FALSE(fs::exists(*r.outputPath));
}

TEST(PipelinePreflight, SameLargeFramesAtScaleOneResolve) {
  ASSERT_LT(sr::estimatePeakBytes(5000, 4400, 1, 3), sr::kMemoryBudgetBytes);
  const auto dir = fixtures::freshDir();
  auto r = requestFor(std::vector<std::string>(3, largeFrame(dir)), dir);
  r.outputPath = (dir / "out.jpg").string();

  const sr::Result res = sr::run(r);

  EXPECT_EQ(res.width, 5000);
  EXPECT_EQ(res.height, 4400);
  EXPECT_EQ(res.framesUsed, 3);
  EXPECT_TRUE(fs::exists(*r.outputPath));
}

// Edge case: frames so thin that graySmall would have a side < 2 px reject instead of reaching
// cv::resize's size assert (AD-004: abort).
TEST(PipelinePreflight, TooThinFramesRejectInsteadOfAborting) {
  const auto dir = fixtures::freshDir();
  const auto row = fixtures::writePng(dir, "row.png", fixtures::texturedImage(4096, 1));
  const auto e = runExpectingError(requestFor(std::vector<std::string>(3, row), dir));
  EXPECT_EQ(e.code, "E_ALIGNMENT_FAILED");
  EXPECT_NE(e.message.find("too thin"), std::string::npos) << e.message;
}
