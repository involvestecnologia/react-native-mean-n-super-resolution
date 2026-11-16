# STATE

## Decisions

### AD-001
- **Decision**: All native logic (including future OpenCV super-resolution code) lives in shared C++ under `cpp/`, exposed as a pure C++ TurboModule; no Kotlin/Swift/Obj-C module code.
- **Reason**: Project scaffolded as `turbo-module` + `cpp` (create-react-native-library 0.64.0); one implementation compiles for both iOS (podspec + `ios/OnLoad.mm`) and Android (`android/CMakeLists.txt` via `react-native.config.js`).
- **Trade-off**: New Architecture only; platform-specific APIs (e.g. image decoding from Photos/content URIs) need C++ bridging or must be handled in JS before calling the module.
- **Scope**: All features; `cpp/`, `ios/`, `android/`, `src/Native*.ts`.
- **Date**: 2026-11-11
- **Status**: active

### AD-002
- **Decision**: OpenCV comes from opencv-mobile prebuilt packages, version + per-platform URL + SHA-256 pinned in root `opencv.json`, downloaded at build time (Android CMake, podspec evaluation, host tests) into gitignored `.opencv/`; never committed or shipped in npm tarball.
- **Reason**: Smallest binaries, one OpenCV version across Android, iOS and host tests; consumer adds no Gradle/Pod deps.
- **Trade-off**: Build needs network (or `OPENCV_MOBILE_DIR` override); module set limited to what opencv-mobile ships.
- **Scope**: All native features using OpenCV.
- **Date**: 2026-11-11
- **Status**: active

### AD-003
- **Decision**: Image-processing logic lives in JSI-free C++ under `cpp/sr/` and is covered by host GoogleTest suite in `cpp/tests/` (`yarn test:native`); TurboModule impl is a thin async adapter. Platform specifics go behind `cpp/platform/` interfaces.
- **Reason**: Only way to get automated, CI-runnable evidence for native behavior; RN layer stays thin.
- **Trade-off**: Extra CMake project and CI job; platform glue (temp dir, threading) still needs device verification.
- **Scope**: All native features.
- **Date**: 2026-11-11
- **Status**: active

### AD-004
- **Decision**: Native code must never rely on catching `cv::Exception`: opencv-mobile is built with `-fno-exceptions`, so OpenCV assertion failures abort the process. Every OpenCV call is preceded by input guards (non-empty, expected type, positive sizes, finite matrices) and allocation-heavy steps by a memory pre-flight budget.
- **Reason**: Owner chose to keep opencv-mobile (AD-002) rather than build/ship OpenCV with exceptions (2026-11-11).
- **Trade-off**: No ECC (`findTransformECC` asserts internally) — alignment is translation-only; residual crash risk for unforeseen OpenCV asserts.
- **Scope**: All native code calling OpenCV.
- **Date**: 2026-11-11
- **Status**: active

### AD-005
- **Decision**: Consumer Android apps must build with NDK >= r30 (example uses 30.0.14904198); library CMake fails fast below r30. *(Amended 2026-11-13: owner chose "require recent NDK" assuming r29; T9 found r29 also fails to link, r30 works.)*
- **Reason**: opencv-mobile v38 Android static libs are built with NDK r29+ (newer libc++ `__hash_memory`, OpenMP `__kmpc_dispatch_deinit`) and fail to link under RN 0.88 default NDK 27.1. Owner chose this over pinning an older opencv-mobile (2026-11-12).
- **Trade-off**: Extra install step (`ndkVersion` in app `android/build.gradle`); NDK bumps must track opencv-mobile releases.
- **Scope**: Android build, README install docs, CI build-android.
- **Date**: 2026-11-12
- **Status**: active

### AD-006
- **Decision**: Frame alignment uses a vendored copy of OpenCV 4.14.0's ECC homography (`cpp/sr/vendor/Ecc.cpp`, homography path only) whose non-convergence returns a failure instead of raising `CV_Error`. Amends AD-004's trade-off "No ECC": ECC is allowed through this copy only; calling `cv::findTransformECC` directly stays forbidden.
- **Reason**: Owner chose lab parity with burst-sr-lab `mean_N` (2026-11-16), which aligns with ECC `MOTION_HOMOGRAPHY`. Spike (lab-parity-mean design.md): vendored copy is bit-identical to the library function; flat input and off-image guesses return failure without abort; Python cv2 4.12 vs C++ 4.14 differ by ~1e-6 px.
- **Trade-off**: ~300 lines of upstream code to re-sync when `opencv.json` bumps OpenCV; AD-004 guards still apply to everything else.
- **Scope**: `cpp/sr/Align.*`, `cpp/sr/vendor/**`.
- **Date**: 2026-11-16
- **Status**: active

## Handoff

- **Feature**: `.specs/features/lab-parity-mean` (port burst-sr-lab `mean_N` + bicubic; breaking API)
- **Phase / Task**: Execute — T1–T7 done; next T8 (tasks approved 2026-11-16)
- **Completed**: spec.md (owner-confirmed 2026-11-16), context.md, design.md (approved 2026-11-16), tasks.md, ECC spike (results in design.md), macOS host link fix in `cmake/OpenCVMobile.cmake`
- **In-progress** (file:line): none
- **Next step**: T8 two-pass run (also removes estimateTransform, old estimatePeakBytes, align_test.cpp; adds Result fields, Trace, scale default 1)
- **Blockers**: none (field sessions of 2026-10-28 not on this machine: parity on field data is an open evidence gap)
- **Uncommitted files**: none
- **Branch**: feat/lab-parity-mean
