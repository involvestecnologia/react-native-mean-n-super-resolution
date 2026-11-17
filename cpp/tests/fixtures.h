#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fixtures {

// invariant: same (w, h, seed) always yields the same pixels (seeded cv::RNG).
cv::Mat texturedImage(int width, int height, std::uint64_t seed = 42);

// invariant: returns a fresh, empty directory unique to the running test.
std::filesystem::path freshDir();

std::string writePng(const std::filesystem::path& dir, const std::string& name, const cv::Mat& img);

// invariant: writes frames[i] as "f<i>.png" into dir, returns the paths in order.
std::vector<std::string> writeFrames(const std::filesystem::path& dir, const std::vector<cv::Mat>& frames);

std::vector<unsigned char> readBytes(const std::filesystem::path& file);

}  // namespace fixtures
