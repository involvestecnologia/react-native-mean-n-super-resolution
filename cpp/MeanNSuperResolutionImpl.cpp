#include "MeanNSuperResolutionImpl.h"

#include "platform/TempDir.h"
#include "sr/ErrorMapping.h"
#include "sr/Pipeline.h"

#include <cmath>
#include <exception>
#include <thread>
#include <utility>

namespace facebook::react {

namespace {

// invariant: a missing option maps to its default; a non-integral or non-finite
// value maps to 0, which sr::validate rejects with E_INVALID_ARGUMENT like any
// other out-of-range value.
int toInt(const std::optional<double>& value, int fallback) {
  if (!value) return fallback;
  const double v = *value;
  if (!std::isfinite(v) || v != std::trunc(v) || v < -1e6 || v > 1e6) return 0;
  return static_cast<int>(v);
}

// invariant: a non-finite blur maps to -1 so sr::validate rejects it with a
// readable value.
double toBlur(const std::optional<double>& value, double fallback) {
  if (!value) return fallback;
  return std::isfinite(*value) ? *value : -1.0;
}

SuperResolveResult toJs(const sr::Result& r) {
  std::vector<double> used(r.used.begin(), r.used.end());
  std::vector<SuperResolveDropped> dropped;
  dropped.reserve(r.dropped.size());
  for (const sr::Drop& d : r.dropped) {
    dropped.push_back(SuperResolveDropped{static_cast<double>(d.index), d.reason});
  }
  return SuperResolveResult{
    r.path,
    static_cast<double>(r.width),
    static_cast<double>(r.height),
    static_cast<double>(r.framesUsed),
    static_cast<double>(r.refIndex),
    std::move(used),
    std::move(dropped)};
}

}

MeanNSuperResolutionImpl::MeanNSuperResolutionImpl(
  std::shared_ptr<CallInvoker> jsInvoker
)
  : NativeMeanNSuperResolutionCxxSpec(std::move(jsInvoker)) {}

AsyncPromise<SuperResolveResult> MeanNSuperResolutionImpl::superResolve(
  jsi::Runtime& rt,
  std::vector<std::string> paths,
  SuperResolveOptions options
) {
  AsyncPromise<SuperResolveResult> promise(rt, jsInvoker_);

  sr::Request request;
  request.paths = std::move(paths);
  const sr::Request defaults;
  request.n = toInt(options.n, defaults.n);
  request.blur = toBlur(options.blur, defaults.blur);
  request.scale = toInt(options.scale, defaults.scale);
  request.outputPath = std::move(options.outputPath);

  // invariant: the worker owns copies of the request and promise and never
  // captures `this`; the module may be destroyed before the worker finishes.
  auto work = [promise, request = std::move(request)]() mutable {
    try {
      // why: resolved on the worker so a missing temp dir rejects instead of
      // throwing into JS; Android may need JNI here (see android/src/TempDir.cpp).
      if (!request.outputPath) request.defaultOutputDir = sr::platform::defaultOutputDir();
      promise.resolve(toJs(sr::run(request)));
    } catch (...) {
      promise.reject(Error(sr::toRejectMessage(std::current_exception())));
    }
  };

  try {
    std::thread(std::move(work)).detach();
  } catch (...) {
    promise.reject(Error(sr::toRejectMessage(std::current_exception())));
  }
  return promise;
}

}
