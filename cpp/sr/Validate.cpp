#include "Errors.h"
#include "Paths.h"
#include "Pipeline.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace sr {

void validate(const Request& request) {
  const auto n = request.paths.size();
  if (n < kMinFrames || n > kMaxFrames) {
    throw SrError(code::InvalidArgument,
                  "paths must contain 3 to 30 entries, got " + std::to_string(n));
  }
  for (size_t i = 0; i < n; ++i) {
    if (request.paths[i].empty()) {
      throw SrError(code::InvalidArgument, "paths[" + std::to_string(i) + "] is empty");
    }
  }
  if (request.n < kMinN || request.n > kMaxN) {
    throw SrError(code::InvalidArgument,
                  "n must be an integer from 2 to 30, got " + std::to_string(request.n));
  }
  if (!std::isfinite(request.blur) || request.blur < 0 || request.blur > 1) {
    char value[32];
    std::snprintf(value, sizeof value, "%g", request.blur);
    throw SrError(code::InvalidArgument,
                  std::string("blur must be a number from 0 to 1, got ") + value);
  }
  if (request.scale < 1 || request.scale > 4) {
    throw SrError(code::InvalidArgument,
                  "scale must be 1, 2, 3 or 4, got " + std::to_string(request.scale));
  }
  if (request.outputPath) {
    const auto ext = lowerExt(*request.outputPath);
    if (request.outputPath->empty() || (ext != ".jpg" && ext != ".jpeg" && ext != ".png")) {
      throw SrError(code::InvalidArgument,
                    "outputPath must end in .jpg, .jpeg or .png: '" + *request.outputPath + "'");
    }
  }
}

}  // namespace sr
