// Parity harness, native side (lab-parity-mean T11): runs sr::run on the given frames and
// writes what scripts/parity/compare.py checks against the burst-sr-lab golden.
//
//   sr_parity_cli [--n N] [--blur B] --out DIR frame0 frame1 ...
//
// DIR/native.json: refIndex, used, dropped, warps (H full resolution, cc) of the scale-1 run.
// DIR/mean.png: scale 1 output. DIR/mean_x2.png: scale 2 output (native bicubic path).

#include "Errors.h"
#include "Pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int usage(const char* msg) {
  std::cerr << "sr_parity_cli: " << msg << "\n"
            << "usage: sr_parity_cli [--n N] [--blur B] --out DIR frame0 frame1 ...\n";
  return 2;
}

std::string jsonString(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string num(double v, const char* fmt) {
  char buf[40];
  std::snprintf(buf, sizeof buf, fmt, v);
  return buf;
}

std::string toJson(const sr::Result& r, const sr::Trace& trace) {
  std::string j = "{\n  \"refIndex\": " + std::to_string(r.refIndex) + ",\n  \"used\": [";
  for (size_t i = 0; i < r.used.size(); ++i) j += (i ? ", " : "") + std::to_string(r.used[i]);
  j += "],\n  \"dropped\": [";
  for (size_t i = 0; i < r.dropped.size(); ++i) {
    j += std::string(i ? "," : "") + "\n    {\"index\": " + std::to_string(r.dropped[i].index) +
         ", \"reason\": " + jsonString(r.dropped[i].reason) + "}";
  }
  j += r.dropped.empty() ? "],\n" : "\n  ],\n";
  j += "  \"warps\": [";
  for (size_t k = 0; k < trace.size(); ++k) {
    const auto& t = trace[k];
    j += std::string(k ? "," : "") + "\n    {\"index\": " + std::to_string(t.index) + ", \"H\": [";
    for (int row = 0; row < 3; ++row) {
      j += row ? ", [" : "[";
      for (int col = 0; col < 3; ++col) {
        // why: %.9g round-trips a float32 exactly.
        j += (col ? ", " : "") + num(t.H.at<float>(row, col), "%.9g");
      }
      j += "]";
    }
    j += "], \"cc\": " + num(t.cc, "%.17g") + "}";
  }
  j += trace.empty() ? "],\n" : "\n  ],\n";
  j += "  \"width\": " + std::to_string(r.width) + ",\n  \"height\": " + std::to_string(r.height) +
       "\n}\n";
  return j;
}

}  // namespace

int main(int argc, char** argv) {
  sr::Request request;
  std::filesystem::path out;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "--n" || a == "--blur" || a == "--out") && i + 1 >= argc) return usage("missing value");
    if (a == "--n") {
      request.n = std::atoi(argv[++i]);
    } else if (a == "--blur") {
      request.blur = std::atof(argv[++i]);
    } else if (a == "--out") {
      out = argv[++i];
    } else if (a.rfind("--", 0) == 0) {
      return usage(("unknown option " + a).c_str());
    } else {
      request.paths.push_back(a);
    }
  }
  if (out.empty()) return usage("--out is required");

  try {
    std::filesystem::create_directories(out);
    sr::Trace trace;
    request.scale = 1;
    request.outputPath = (out / "mean.png").string();
    const sr::Result r = sr::run(request, &trace);

    request.scale = 2;
    request.outputPath = (out / "mean_x2.png").string();
    sr::run(request);

    std::ofstream json(out / "native.json", std::ios::binary | std::ios::trunc);
    json << toJson(r, trace);
    if (!json) throw std::runtime_error("could not write native.json");
  } catch (const sr::SrError& e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "sr_parity_cli: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
