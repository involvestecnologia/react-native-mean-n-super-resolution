# lab-parity-mean Tasks

## Execution Protocol (MANDATORY -- do not skip)

Implement these tasks with the `tlc-spec-driven` skill: **activate it by name and follow its Execute flow and Critical Rules.** Do not search for skill files by filesystem path. The skill is the source of truth for the full flow (per-task cycle, sub-agent delegation, adequacy review, Verifier, discrimination sensor).

**If the skill cannot be activated, STOP and tell the user — do not proceed without it.**

---

**Design**: `.specs/features/lab-parity-mean/design.md`
**Status**: In Progress — approved 2026-11-16

---

## Test Coverage Matrix

> From the existing matrix of `opencv-super-resolution` (same layers) plus the parity harness. Lessons applied: L-001 (assert pixels on skip paths), L-002 (assert encoder params), L-003 (exact expected values on degraded paths), L-004 (name the harness for the TurboModule path), L-005 (platform runtime ACs stay open until run).

| Code Layer | Required Test Type | Coverage Expectation | Location Pattern | Run Command |
| ---------- | ------------------ | -------------------- | ---------------- | ----------- |
| `cpp/sr/**` (Burst, Align, vendor/Ecc, Pipeline, Validate) | unit (host GoogleTest) | Every LP-CORE and C++ LP-API AC with exact expected values; every spec edge case | `cpp/tests/*_test.cpp` | `yarn test:native` |
| `src/**` | unit (Jest, native mocked) | LP-API-01..06 JS side, defaults pass-through, exports | `src/__tests__/*.test.ts` | `yarn test` |
| TurboModule adapter | build + device UAT | Compiles with the regenerated codegen structs; result fields reach JS on Android | — | `yarn turbo run build:android`; example app |
| Parity harness (`scripts/parity/**`, `sr_parity_cli`) | parity (local) | LP-PAR-01..05 on 4 synthetic bursts; field sessions = open gap until available | `scripts/parity/` | `LAB_DIR=… yarn test:parity` |
| Example app | unit (Jest) + manual UAT | burst runner passes new options/fields; screen shows `refIndex`/`used`/`dropped` | `example/src/burst/__tests__` | `yarn test` |
| README / STATE | none | build gate | — | — |

## Gate Check Commands

| Gate Level | When to Use | Command |
| ---------- | ----------- | ------- |
| Quick (native) | Tasks touching `cpp/sr/**`, `cpp/tests/**` | `yarn test:native` |
| Quick (JS) | Tasks touching `src/**`, `example/src/**` | `yarn test` |
| Full | TurboModule / platform glue | `yarn test && yarn test:native && yarn turbo run build:android` (iOS: CI `build-ios`) |
| Parity | Parity tasks | `LAB_DIR=$HOME/repos/burst-sr-lab yarn test:parity` |
| Build | End of each phase | `yarn lint && yarn typecheck && yarn test && yarn test:native && yarn prepare` |

---

## Execution Plan

### Phase 1: Building blocks (host-tested, parallel OK)

```
T1 [P]   vendored ECC
T2 [P] → T3 → T4    graySmall/sharpness → blur filter → reference + order
```

### Phase 2: Pipeline

```
T1 ─┐
T2 ─┼→ T5 (estimateWarp) ─┐
    │  T6 [P] (validate) ─┼→ T8 (run) 
    │  T7 [P] (mean, mem) ┘
```

### Phase 3: React Native

```
T8 → T9 (TS spec + validate.ts) → T10 (JSI adapter + Android build)
```

### Phase 4: Parity

```
T8 → T11 (sr_parity_cli) ─┐
     T12 [P] (synth + lab golden) ┼→ T13 (compare + run + results)
```

### Phase 5: Example and docs

```
T10 → T14 (example) ; T13 → T15 (README, STATE)
```

---

## Task Breakdown

### T1: Vendored ECC homography [P]

**What**: `sr::vendor::eccHomography` copied from OpenCV 4.14.0 `modules/video/src/ecc.cpp` per design (homography path only, two `StsNoConv` → `nullopt`, `checkRange(map)` after each update); record AD-006 in `.specs/STATE.md` (amends AD-004 trade-off "No ECC").
**Where**: `cpp/sr/vendor/Ecc.h`, `cpp/sr/vendor/Ecc.cpp`, `cpp/tests/ecc_test.cpp`, `cpp/tests/CMakeLists.txt`, `android/CMakeLists.txt`, `.specs/STATE.md`
**Depends on**: None
**Reuses**: spike `/tmp/ecc-spike/ecc_vendored.cpp` (regenerate from upstream; do not depend on /tmp)
**Requirement**: LP-CORE-05, LP-DOC-02, LP-DOC-04

**Done when**:
- [ ] File keeps the upstream license header verbatim and lists every change from upstream with the upstream tag
- [ ] Test: on a synthetic homography pair, H and rho equal `cv::findTransformECC(…, MOTION_HOMOGRAPHY, {COUNT|EPS,100,1e-5}, noArray(), 5)` exactly (`NORM_INF == 0`, rho `==`)
- [ ] Test: flat input → `nullopt`; initial map shifted 900 px → `nullopt`; uncorrelated noise → `nullopt` or value < 0.9; bad arguments → `nullopt`; none aborts
- [ ] AD-006 written (decision, reason = spike evidence, trade-off = vendored code to track on OpenCV bumps)

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): vendor opencv ecc homography without aborting errors`
**Status**: ✅ Done (3efe012) — `yarn test:native` 57/57; Ecc.* 5 tests

---

### T2: `graySmall` and `sharpness` [P]

**What**: `sr::graySmall` and `sr::sharpness` per design.
**Where**: `cpp/sr/Burst.h`, `cpp/sr/Burst.cpp`, `cpp/tests/burst_test.cpp`, both CMakeLists
**Depends on**: None
**Requirement**: LP-CORE-01 (sharpness definition), edge case "smaller than 1024"

**Done when**:
- [ ] 2000×1500 BGR → 1024×768 CV_32F and `s == 1024/2000`; 640×480 → 1024×768 (upscaled); 1500×2000 portrait → 768×1024
- [ ] `sharpness` equals `σ²` of `cv::Laplacian(g, CV_32F)` computed in the test independently (rel. error < 1e-9); flat image → 0
- [ ] Gray values match `cvtColor`+`resize(INTER_AREA)`+`convertTo` exactly

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): add lab gray image and sharpness helpers`
**Status**: ✅ Done (dc99000) — `yarn test:native` 57/57; Burst.* 6 tests

---

### T3: Blur filter

**What**: `sr::applyBlurFilter` with lab median, `MIN_KEEP = 3`, `ratio == 0` no-op and lab reason text.
**Where**: `cpp/sr/Burst.{h,cpp}`, `cpp/tests/burst_test.cpp`
**Depends on**: T2
**Requirement**: LP-CORE-01, edge cases "filter would leave < 3", "blur = 0"

**Done when**:
- [ ] `{10,10,10,10,4}` at 0.8 → drops index 4 with reason exactly `sharpness 0.40 of median < 0.8, dropped (blur)`
- [ ] Even count median: `{1,2,3,4}` median 2.5 (a value of 1.9 at 0.8 → dropped, 2.0 → kept)
- [ ] `{10,10,1,1}` at 0.8 (median 5.5, would keep 2) → nothing dropped; ratio 0 → nothing dropped; ratio 0.85 formats as `< 0.85` *(example corrected during T3: `{10,1,1,1}` has median 1 and drops nothing)*
- [ ] Kept indices are in ascending original order

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): add lab blur frame filter`
**Status**: ✅ Done (17939bb) — `yarn test:native` 63/63; BlurFilter.* 6 tests; reason strings cross-checked against the lab f-strings with numpy

---

### T4: Reference choice and temporal order

**What**: `sr::pickReference` (first 8 kept, first max wins) and `sr::temporalOrder` (stable by `|i − ref|`).
**Where**: `cpp/sr/Burst.{h,cpp}`, `cpp/tests/burst_test.cpp`
**Depends on**: T3
**Requirement**: LP-CORE-02, LP-CORE-03, edge case "fewer than 8 survive"

**Done when**:
- [ ] Sharpest at kept position 9 is ignored (only first 8 compete); tie → lower index
- [ ] kept `{0,1,2,4,5,7}`, ref 4 → order exactly `{4,5,2,1,7,0}` (distances 0,1,2,3,3,4; tie 1/7 → lower index first)
- [ ] 3 kept → reference chosen among all 3

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): add lab reference choice and temporal order`
**Status**: ✅ Done (5d280db) — `yarn test:native` 70/70; Reference.* 4, TemporalOrder.* 3 tests; expected values cross-checked with np.argmax / sorted in Python

---

### T5: `estimateWarp` (phaseCorrelate + ECC)

**What**: Add `estimateWarp` + `dropReason` per design next to `estimateTransform`. *(Changed during T5: deleting `estimateTransform` here would break `Pipeline.cpp` until T8, so T8 removes it together with `align_test.cpp`.)*
**Where**: `cpp/sr/Align.h`, `cpp/sr/Align.cpp`, `cpp/tests/warp_test.cpp`
**Depends on**: T1, T2
**Requirement**: LP-CORE-04, LP-CORE-05, edge cases "flat", "non-finite shift"

**Done when**:
- [ ] Synthetic 2048×1536 frame warped by a known homography (shift + 0.3° + 0.2 % scale) → returned full-res H maps the 4 corners within 0.05 px of the truth
- [ ] Uncorrelated frame → `LowCorrelation`, reason matches `^ECC cc 0\.\d{3} < 0\.9, dropped$`
- [ ] Flat frame → `NoConvergence`, reason exactly `ECC did not converge, dropped`
- [ ] 1×N gray input does not reach OpenCV (guard) → `NoConvergence`
- [ ] `H` returned as CV_32F *(the planned "same product in the test" check would only restate the code; precision is covered by the corner test and, against the lab, by T13)*

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): align frames with phase correlation and ecc homography`
**Status**: ✅ Done (4395b0f) — `yarn test:native` 77/77; EstimateWarp.* 7 tests. Measured: corner error 0.027 px vs truth (shift + 0.3° + 0.2 % scale, 2048×1536), noisy frame cc 0.85, unrelated scene cc 0.43 (both LowCorrelation)

---

### T6: Request/Result types and C++ validation [P]

**What**: `Request` gains `n` and `blur`; `sr::validate` with the new rules. *(Changed during T6: the new `Result` fields, `Trace` and the `scale` default 2 → 1 change `run` behavior, so they move to T8; `Drop` already exists from T3.)*
**Where**: `cpp/sr/Pipeline.h`, `cpp/sr/Validate.cpp`, `cpp/tests/validate_test.cpp`
**Depends on**: T2 (for `Drop`)
**Requirement**: LP-API-02..05 (C++ side), LP-API-06

**Done when**:
- [ ] Accepts 3 and 30 paths; rejects 2 and 31 with `E_INVALID_ARGUMENT`
- [ ] `n`: 2 and 30 ok; 1, 31 rejected. `blur`: 0, 0.8, 1 ok; −0.01, 1.01, NaN, inf rejected. `scale` 1..4 ok, 0/5 rejected. Bad extension rejected
- [ ] New-rule messages asserted literally: `paths must contain 3 to 30 entries, got N`, `n must be an integer from 2 to 30, got N`, `blur must be a number from 0 to 1, got X`; T9 uses the same prefixes in JS (the shown value may differ, e.g. NaN). Messages of unchanged rules stay as they are

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native)!: validate lab options n and blur and free frame count`
**Status**: ✅ Done (a279a3f) — `yarn test:native` 83/83; Validate.* 13 tests

---

### T7: Half-to-even mean and memory estimate [P]

**What**: `sr::meanRoundHalfEven` and the new `estimatePeakBytes(w, h, scale, frames)`. *(Added next to the 3-argument version, which `run` still uses; T8 removes it.)*
**Where**: `cpp/sr/Pipeline.{h,cpp}`, `cpp/tests/pipeline_test.cpp`
**Depends on**: None
**Requirement**: LP-CORE-07, edge case memory

**Done when**:
- [ ] Sums `{5,7,9,11}` with count 2 → `{2,4,4,6}` (half to even); count 6 sum 15 → 2 (2.5 → 2), sum 21 → 4 (3.5 → 4); 30·255 / 30 → 255
- [ ] `estimatePeakBytes` equals the design formula for (4000×3000, 4, 30) and (4000×3000, 1, 3), literal values in the test; 12 MP/30/4 < 1 GiB; 50 MP/30/4 > 1 GiB; 24 MP/30/1 < 1 GiB

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native): add half-to-even mean and new memory estimate`
**Status**: ✅ Done — `yarn test:native` 88/88; MeanRoundHalfEven.* 3, PipelinePreflight.LabEstimate* 2 tests; rounding cross-checked with numpy float32

---

### T8: Two-pass `run`

**What**: Rewrite `sr::run` per design flow (pass 1, filter, reference, order, pre-flight, pass 2 until `n`, mean, release, bicubic, encode), filling `Result` and optional `Trace`.
**Where**: `cpp/sr/Pipeline.cpp`, `cpp/tests/pipeline_test.cpp`, `cpp/tests/fixtures.{h,cpp}`
**Depends on**: T3, T4, T5, T6, T7
**Also from T6**: `Result` fields (`refIndex`, `used`, `dropped`), `Trace`, `Request::scale` default 1
**Also from T7**: remove the 3-argument `estimatePeakBytes` and rewrite its tests on the 4-argument one
**Also**: remove `estimateTransform` from `Align.*` and delete `cpp/tests/align_test.cpp` (moved here from T5); move any still-relevant pipeline cases from it into `pipeline_test.cpp`
**Requirement**: LP-CORE-06, LP-CORE-08, LP-CORE-09, LP-API-01 (C++ fields), LP-API-07

**Done when**:
- [ ] Identical frames (scale 1) → output equals input exactly; `used.size() == min(n, frames)`; `dropped` empty; `used[0] == refIndex`
- [ ] Burst of 10 with frame 3 blurred → `dropped` has index 3 with blur reason; frame 3 not in `used`; output pixels equal the mean computed in the test from the expected `used` set (L-001)
- [ ] Burst with an unrelated frame at index 1, ref 0 → `dropped` has index 1 with ECC reason; output equals the mean of the rest
- [ ] `n = 3` on 10 identical-geometry frames → `used == {ref, …}` in temporal order, exactly 3 entries; frames beyond are neither used nor dropped
- [ ] Sharpest frame not at index 0 → `refIndex` is that frame, output geometry is that frame's
- [ ] `scale = 2` → output equals `resize(mean, INTER_CUBIC)` exactly, dims 2×; default scale → 1×
- [ ] Only the reference accepted → `E_ALIGNMENT_FAILED`; size mismatch → `E_SIZE_MISMATCH` with index before any warp; JPEG q95 still asserted (L-002)
- [ ] Old tests that encoded the previous behavior (scale 2 default, `paths[0]` reference, NCC skip) are rewritten, not deleted silently (listed in the commit body)

**Tests**: unit · **Gate**: quick (native)
**Commit**: `feat(native)!: merge bursts like the burst-sr-lab mean_N pipeline`
**Status**: ✅ Done (418f7c4) — `yarn test:native` 86/86 (−6 align_test, −1 3-arg formula test, +5 new). Mutants caught: no stop at `n`, reference = first kept, no pass-1 size check. *(Changed during T8: the memory pre-flight runs right after decoding `paths[0]` (frame count is known up front) instead of after pass 1, so an over-budget burst fails before decoding the rest; same formula and budget. Added a guard: frames whose graySmall would have a side < 2 px reject `E_ALIGNMENT_FAILED` "too thin to align", because `cv::resize` asserts on an empty target. The `E_ALIGNMENT_FAILED` message lists the drops as `index=N <reason>;`. Adapter `toScale` still defaults to 2 until T10.)*

---

### T9: TS spec, public types and JS validation

**What**: Update `src/NativeMeanNSuperResolution.ts` (options `n`, `blur`; `SuperResolveDropped`; result fields), `src/types.ts` (docs, narrow types), `src/index.tsx` export, `src/validate.ts` new rules.
**Where**: `src/*.ts(x)`, `src/__tests__/superResolve.test.ts`, `src/__tests__/index.test.ts`
**Depends on**: T6
**Requirement**: LP-API-01..06 (JS side)

**Done when**:
- [ ] Jest covers the same accept/reject table and literal messages as T6
- [ ] Defaults pass through untouched (`{}` reaches native when no options)
- [ ] `SuperResolveDropped` exported from the package; `yarn typecheck` passes

**Tests**: unit (Jest) · **Gate**: quick (JS)
**Commit**: `feat!: expose n, blur and merge metadata in superResolve`
**Status**: ✅ Done (2ed388f) — `yarn test` 89/89, `yarn typecheck`, `yarn lint` clean. Same accept/reject table as T6 plus JS-only cases (strings, `null`, non-integral `n`); literal messages for paths/n/blur. *(Changed during T9: `example/src/burst/__tests__/burstRunner.test.ts` result fixture gained `refIndex`/`used`/`dropped` so `tsc` passes; the example's own changes stay in T14.)*

---

### T10: JSI adapter and Android build

**What**: Update `Bridging<>` aliases and option mapping (`n` default 8, `blur` default 0.8, `scale` default 1, invalid → rejected values) and fill `refIndex`/`used`/`dropped`; add `Burst.cpp`, `vendor/Ecc.cpp` to `android/CMakeLists.txt` if not done in T1/T2.
**Where**: `cpp/MeanNSuperResolutionImpl.{h,cpp}`, `android/CMakeLists.txt`
**Depends on**: T8, T9
**Requirement**: LP-API-01 (bridge), L-004 (harness: Android build + example UAT)

**Tools**: MCP: `context7` (RN codegen C++ struct/array bridging) · Skill: NONE

**Done when**:
- [ ] `yarn turbo run build:android` succeeds (codegen regenerates the structs, adapter compiles)
- [ ] Example run on Android logs a result with `refIndex`, `used`, `dropped` (captured in validation)
- [ ] iOS: CI `build-ios` green, or recorded as open gap (L-005)

**Tests**: build + UAT · **Gate**: full
**Commit**: `feat(turbomodule): bridge lab options and merge metadata`
**Status**: ⚠️ Partial (3a3e38a) — Android build ✅ (`app:bundleDebug`, arm64-v8a, 5m52s, 2026-11-16; codegen regenerated `Options<n, blur, scale, outputPath>`, `Dropped<index, reason>`, `Result<…, refIndex, used, dropped>`; adapter compiled and linked). Open (L-005): example run on an Android device logging `refIndex`/`used`/`dropped` (no device attached), iOS CI `build-ios`. *(Build ran as `sh ./gradlew …` because `example/android/gradlew` is committed as 100644 and `yarn turbo run build:android` fails locally with EACCES; pre-existing, not fixed here.)* `Burst.cpp`/`vendor/Ecc.cpp` were already in `android/CMakeLists.txt` (T1/T2).

---

### T11: `sr_parity_cli`

**What**: Host CLI target that runs `sr::run` with a `Trace` on given frames and writes `native.json` (refIndex, used, dropped, warps with H and cc) plus outputs at scale 1 and 2 as PNG.
**Where**: `cpp/tests/parity_cli.cpp`, `cpp/tests/CMakeLists.txt`
**Depends on**: T8
**Requirement**: LP-PAR-01..04 (native side)

**Done when**:
- [ ] `build/native-tests/sr_parity_cli --n 8 --out /tmp/x frames/*.jpg` writes `native.json`, `mean.png`, `mean_x2.png`
- [ ] JSON validated by `python3 -m json.tool`

**Tests**: smoke run · **Gate**: quick (native)
**Commit**: `test(parity): add native parity cli`
**Status**: ✅ Done — built by `yarn test:native` (86/86 still pass). Smoke run on 10 synthetic 1600×1200 JPEGs (shifts, frame 3 blurred, frame 6 rotated): exit 0, `native.json` valid (`json.tool`), refIndex 5, dropped 3 (blur 0.22) and 6 (ECC cc 0.021), 8 used, `mean.png` 1600×1200, `mean_x2.png` 3200×2400, 15 s. Errors: `SrError` → stderr + exit 1, bad usage → exit 2. *(Also: `--blur B` option; `mean_x2.png` comes from a second `sr::run` at scale 2 so the native bicubic path is what gets compared; JSON also carries width/height.)*

---

### T12: Synthetic bursts and lab golden [P]

**What**: `scripts/parity/synth.py` (bursts `homography`, `blurred`, `unrelated`, `small`, fixed seed) and `scripts/parity/lab_golden.py` (imports lab modules from `$LAB_DIR`, replicates `run.py:112-124`, writes `golden.json`, `mean.png`, `mean_x2.png`).
**Where**: `scripts/parity/synth.py`, `scripts/parity/lab_golden.py`
**Depends on**: None
**Requirement**: LP-PAR-05, LP-PAR-01..04 (lab side)

**Done when**:
- [ ] Running `synth.py` twice gives byte-identical JPEGs
- [ ] `lab_golden.py` on `blurred` reports frames 3, 6, 9 dropped by blur (lab's own code)
- [ ] Script fails with a clear message when `LAB_DIR` is missing or wrong

**Tests**: smoke run · **Gate**: parity
**Commit**: `test(parity): add synthetic bursts and lab golden generator`

---

### T13: Compare, runner and parity results

**What**: `scripts/parity/compare.py` (tolerances LP-PAR-01..04), `scripts/parity/run.py` (synth → golden → cli → compare, optional `--sessions`), `package.json` `test:parity`; run it and record results in `validation.md`.
**Where**: `scripts/parity/compare.py`, `scripts/parity/run.py`, `package.json`, `.specs/features/lab-parity-mean/validation.md`
**Depends on**: T11, T12
**Requirement**: LP-PAR-01..05

**Done when**:
- [ ] `LAB_DIR=… yarn test:parity` prints one row per burst with ids/reasons match, max corner error, PSNR, max abs diff, pass/fail
- [ ] All 4 synthetic bursts pass, or any tolerance change is justified with numbers in `validation.md`
- [ ] Field sessions: run if present; otherwise listed as open evidence gap

**Tests**: parity · **Gate**: parity + build
**Commit**: `test(parity): compare native merge against burst-sr-lab`

---

### T14: Example app

**What**: Example uses the new API: frame count list no longer tied to the 5/15/30 rule (keep the camera choices, update the `invariant` comment), result panel shows `refIndex`, `used`, `dropped`.
**Where**: `example/src/burst/frameCounts.ts`, `example/src/burst/types.ts`, `example/src/burst/burstRunner.ts`, `example/src/screens/HomeScreen.tsx`, `example/src/burst/__tests__/burstRunner.test.ts`
**Depends on**: T10
**Requirement**: LP-DOC-03

**Done when**:
- [ ] Jest example tests pass with the new result shape
- [ ] Android UAT screenshot/log shows the new fields

**Tests**: unit + UAT · **Gate**: quick (JS)
**Commit**: `feat(example): show merge metadata from superResolve`

---

### T15: README and decision log

**What**: README: pipeline description (lab `mean_N` + bicubic), options table (`n`, `blur`, `scale` default 1), result fields, error table updates, memory formula, migration note (5/15/30 rule removed, scale default 2 → 1, new fields), credit to burst-sr-lab; update CLAUDE.md "Project" line and OpenCV notes (ECC now vendored); STATE handoff.
**Where**: `README.md`, `CLAUDE.md`, `.specs/STATE.md`
**Depends on**: T13
**Requirement**: LP-DOC-01, LP-DOC-02

**Done when**:
- [ ] README options/result match `src/types.ts` exactly
- [ ] CLAUDE.md no longer says "alignment is translation-only phaseCorrelate, not ECC"

**Tests**: none · **Gate**: build
**Commit**: `docs: document lab-parity mean pipeline and migration`

---

## Requirement Coverage

| Requirement | Tasks |
| ----------- | ----- |
| LP-CORE-01 | T2, T3 |
| LP-CORE-02, 03 | T4 |
| LP-CORE-04 | T5 |
| LP-CORE-05 | T1, T5 |
| LP-CORE-06, 08, 09 | T8 |
| LP-CORE-07 | T7 |
| LP-PAR-01..05 | T11, T12, T13 |
| LP-API-01 | T8, T9, T10 |
| LP-API-02..06 | T6, T9 |
| LP-API-07 | T8 |
| LP-DOC-01 | T15 |
| LP-DOC-02 | T1, T15 |
| LP-DOC-03 | T14 |
| LP-DOC-04 | T1 |

**Coverage:** 25 requirements, all mapped, 0 unmapped.
