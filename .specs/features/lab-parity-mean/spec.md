# lab-parity-mean Specification

**Scope:** Complex (algorithm port with numeric parity, vendored OpenCV code, breaking API change).

## Problem Statement

`superResolve` was designed before the burst-sr-lab experiments and does something different from the lab's `mean_N`: it always uses `paths[0]` as reference, aligns by translation only, averages every frame on an upscaled grid, and has no frame filter. The lab measured `mean_N` on real G06 bursts (visibly cleaner whole-shelf photos), so the product needs the on-device result to match the lab, otherwise the lab's conclusions do not carry over to the app.

## Goals

- [ ] For the same burst and options, the native output matches the lab's `mean_N` within the parity tolerances (LP-PAR-01..04).
- [ ] `superResolve` exposes the lab's knobs (`n`, `blur`) and reports what the lab logs (reference, frames used, frames dropped with reason).
- [ ] No OpenCV call can abort the process because of ECC non-convergence (AD-004 still holds).

## Out of Scope

| Feature | Reason |
| ------- | ------ |
| `handheld_30_x2` and the other lab methods (NLM, FastDVDnet, VRT) | Only `mean_N` (+ bicubic) is ported |
| `processBurst` / `upscale` / `cancel()` API from the lab plan | Owner chose to extend `superResolve` instead |
| `timingsMs`, `peakBytes` in the result | Not requested; benchmark is a separate task |
| Local (block) alignment | Lab plan lists it as a separate plan |
| Burst capture changes in the example app | Example keeps its camera burst; only its call site adapts |
| Frame filters other than `blur` | Lab has only `blur` |

---

## Assumptions & Open Questions

| Assumption / decision | Chosen default | Rationale | Confirmed? |
| --------------------- | -------------- | --------- | ---------- |
| Frame count | 3 ≤ `paths.length` ≤ 30 | Lab `MIN_KEEP = 3`; lab captures at most 30 frames; bounds memory and time | y |
| `n` | Integer 2..30, default 8 | Lab `mean_8` is the default method; `mean_15`/`mean_30` also exist | y |
| `n` > frames available after filter/alignment | Averages all available frames, no error (`used.length < n`) | Lab `aligned[:n]` silently takes fewer | y |
| `blur` | Number in [0, 1], default 0.80, 0 disables the filter | Lab `blur=0.80`, `none` | y |
| `scale` | Integer 1..4, default **1** (was 2); applied after the mean with `INTER_CUBIC` | Owner decision 1; lab `mean_N` is 1x | y |
| Only the reference survives alignment | Reject `E_ALIGNMENT_FAILED` (unchanged contract: ≥ 2 frames) | Lab would return the reference unchanged; a rejection tells the caller the burst did not merge | y |
| Frame ids | `refIndex`, `used`, `dropped[].index` are indices into `paths` (lab "file numbers") | Lab logs use file numbers | y |
| Dropped reasons | Same text as the lab log, without the `frame NNN: ` prefix: `sharpness 0.72 of median < 0.8, dropped (blur)`, `ECC did not converge, dropped`, `ECC cc 0.873 < 0.9, dropped` | Owner: same behavior as the lab; easy to diff | y |
| Filter not applied (< 3 would remain) | Not reported in the result (lab logs a warning line only) | No per-frame drop happened | y (agent default) |
| Frames aligned but beyond `n` | Neither in `used` nor in `dropped`; the pipeline stops aligning once `n` frames are accepted | Same set as lab (lab aligns all, then takes first n) | y |
| Input size | All frames same size, else `E_SIZE_MISMATCH` (unchanged) | Lab assumes it | y |
| Output encoding | Unchanged: JPEG q95 / PNG by extension | Lab writes JPEG q95 | y |
| ECC source | `modules/video/src/ecc.cpp` from OpenCV 4.14.0 (opencv.json version), upstream license header kept verbatim | Owner decision 2; same version as the linked OpenCV | y |
| Parity reference | Lab Python code imported from a burst-sr-lab checkout (`LAB_DIR`), not a re-typed copy | Parity against the real lab code | y (agent default) |

**Open questions:** none — owner confirmed the rest on 2026-11-16; "agent default" rows were not asked and can still change in Design.

---

## User Stories

### P1: Lab-identical mean of a burst ⭐ MVP

**User Story**: As the product team, I want `superResolve` to merge a burst exactly like the lab's `mean_N` so that the quality we measured in the lab is what the app delivers.

**Why P1**: It is the purpose of the feature.

**Acceptance Criteria** (`LP-CORE-*`):

1. WHEN called with `blur > 0` THEN system SHALL compute each frame's sharpness as the variance of `Laplacian(CV_32F)` of its gray image resized to long side 1024 with `INTER_AREA` as float32, and SHALL drop frames with sharpness < `blur` × median of the burst, unless fewer than 3 frames would remain, in which case no frame is dropped by the filter.
2. WHEN frames remain after the filter THEN the reference SHALL be the sharpest among the first 8 remaining frames (first index wins on ties) and `refIndex` SHALL be its index in `paths`.
3. WHEN choosing frames to average THEN system SHALL visit remaining frames in order of `|index − refIndex|` (lower index first on ties), reference first, and stop once `n` frames are accepted.
4. WHEN aligning a frame THEN system SHALL estimate the initial translation with `phaseCorrelate` (Hanning window CV_64F, float64 inputs) between the 1024 px gray images, refine it with ECC `MOTION_HOMOGRAPHY` (100 iterations / eps 1e-5, gaussFiltSize 5), and map it to full resolution with `H = S⁻¹·H·S`.
5. WHEN ECC fails to converge THEN the frame SHALL be dropped with reason `ECC did not converge, dropped`; WHEN ECC correlation < 0.9 THEN dropped with `ECC cc <cc:.3f> < 0.9, dropped`; the process SHALL NOT abort in either case.
6. WHEN a frame is accepted THEN it SHALL be warped with `warpPerspective(INTER_LINEAR | WARP_INVERSE_MAP, BORDER_REPLICATE)` at the original resolution and accumulated.
7. WHEN the accepted frames are averaged THEN each output pixel SHALL be the mean of the accepted frames rounded to nearest (half to even) and saturated to uint8.
8. WHEN `scale = s > 1` THEN the mean SHALL be resized by `s` with `INTER_CUBIC`; `width`/`height` SHALL be `s ×` the reference size. WHEN `scale` is omitted THEN output size SHALL equal the input size.
9. WHEN all frames are pixel-identical THEN output SHALL equal the input (scale 1) exactly, `used.length = min(n, frames)` and `dropped` empty.

**Independent Test**: Host GoogleTest on synthetic bursts with known homographies, one blurred frame and one unrelated frame: assert `refIndex`, `used`, `dropped` (indices and exact reasons) and output pixels against expected values.

---

### P1: Parity with the lab's Python code ⭐ MVP

**User Story**: As the developer porting the lab, I want an automated comparison against the lab's own code so that I can prove the port matches it.

**Why P1**: "Same behavior" is only true if measured.

**Acceptance Criteria** (`LP-PAR-*`):

1. WHEN the parity harness runs on a burst THEN native and lab SHALL agree on dropped frames (index and reason), `refIndex` and `used` (same ids, same order).
2. WHEN comparing homographies of accepted frames THEN the maximum displacement of the 4 image corners between native and lab SHALL be < 0.05 px.
3. WHEN comparing the scale-1 mean THEN PSNR SHALL be ≥ 45 dB against the lab PNG and the max absolute difference ≤ 2 levels outside an 8 px border.
4. WHEN comparing `scale = 2` output against the lab's `bicubic_x2(mean)` THEN max absolute difference SHALL be ≤ 2 outside a 16 px border.
5. WHEN the harness runs THEN it SHALL cover at least: one synthetic burst (homography + noise, deterministic seed), one synthetic burst with 3 blurred frames (lab `plans/filtro-de-frames.md` test), and the 6 field sessions of 2026-10-28 when available locally (otherwise recorded as an open evidence gap, L-005).

**Independent Test**: `yarn test:parity` (or equivalent) prints a per-burst pass/fail table; tolerances failing due to JPEG decoder differences are adjusted and recorded in `validation.md`, as the lab plan allows.

---

### P1: API with lab options and metadata ⭐ MVP

**User Story**: As an app developer, I want to pass `n` and `blur` and get back what the merge did so that I can tune and debug bursts.

**Why P1**: The lab behavior depends on these knobs; the breaking change ships together with the algorithm.

**Acceptance Criteria** (`LP-API-*`):

1. WHEN `superResolve(paths, options?)` resolves THEN the result SHALL be `{ path, width, height, framesUsed, refIndex, used: number[], dropped: { index: number; reason: string }[] }` with `framesUsed = used.length` and `used[0] = refIndex`.
2. WHEN `paths.length` is outside 3..30 or an entry is not a non-empty string THEN reject `E_INVALID_ARGUMENT` before native starts.
3. WHEN `n` is not an integer in 2..30 THEN reject `E_INVALID_ARGUMENT`.
4. WHEN `blur` is not a finite number in [0, 1] THEN reject `E_INVALID_ARGUMENT`.
5. WHEN `scale` is not 1..4 or `outputPath` has a bad extension THEN reject `E_INVALID_ARGUMENT` (unchanged).
6. WHEN the same rule is checked THEN `src/validate.ts` and `cpp/sr/Validate.cpp` SHALL agree (Jest and GoogleTest cover the same cases).
7. WHEN fewer than 2 frames are accepted THEN reject `E_ALIGNMENT_FAILED`; decode, size, write and memory errors keep their current codes.

**Independent Test**: Jest for JS validation and result typing; GoogleTest for C++ validation and metadata.

---

### P2: Docs, example and decision log

**User Story**: As a library user, I want the README and example to describe the new behavior so that I can migrate.

**Acceptance Criteria** (`LP-DOC-*`):

1. WHEN reading README THEN it SHALL describe the lab pipeline, `n`, `blur`, new `scale` default, result fields, and a migration note for the breaking change (5/15/30 rule removed, `scale` default 2 → 1).
2. WHEN reading `.specs/STATE.md` THEN AD-006 SHALL record the vendored ECC and amend AD-004's "no ECC" trade-off.
3. WHEN running the example app THEN its burst screen SHALL call the new API and show `refIndex`, `used` and `dropped`.
4. WHEN the vendored `ecc.cpp` is in the repo THEN it SHALL keep the upstream license header verbatim and list every change from upstream.

---

## Edge Cases

- WHEN the burst is smaller than 1024 px on its long side THEN the 1024 px gray image SHALL be upscaled with `INTER_AREA` (lab `gray_small` does).
- WHEN fewer than 8 frames survive the filter THEN the reference SHALL be chosen among all of them.
- WHEN the filter would leave fewer than 3 frames THEN no frame is dropped by the filter.
- WHEN a frame's 1024 px gray image is flat (zero variance) THEN ECC SHALL fail as "did not converge" and drop the frame, without abort.
- WHEN `phaseCorrelate` returns a non-finite shift THEN the frame SHALL be dropped as "ECC did not converge" (guard before ECC, AD-004).
- WHEN `blur = 0` THEN no sharpness filter runs; sharpness is still computed for the reference choice.
- WHEN the memory estimate (accumulator + working frames + upscaled output) exceeds 1 GiB THEN reject `E_INTERNAL` before allocating (AD-004).

---

## Implicit-Requirement Dimensions Sweep

| Dimension | Resolution |
| --------- | ---------- |
| Input validation & bounds | LP-API-02..06 |
| Failure / partial-failure states | Per-frame drops reported in `dropped` (LP-CORE-01, 05); < 2 accepted → LP-API-07 |
| Idempotency / retry / duplicate handling | Deterministic for same inputs/build (unchanged); duplicate paths are separate frames |
| Auth boundaries & rate limits | N/A: on-device computation |
| Concurrency / ordering | Unchanged: independent concurrent calls; temporal order defined by `paths` order (LP-CORE-03) |
| Data lifecycle / expiry | Unchanged (temp/cache default output) |
| Observability | `refIndex`, `used`, `dropped` in the result (LP-API-01) |
| External-dependency failure | Vendored ECC removes abort on non-convergence (LP-CORE-05) |
| State-transition integrity | N/A: stateless function |

---

## Requirement Traceability

| Requirement ID | Story | Phase | Status |
| -------------- | ----- | ----- | ------ |
| LP-CORE-01..09 | P1 Lab-identical mean | Specify | Pending |
| LP-PAR-01..05 | P1 Parity with lab | Specify | Pending |
| LP-API-01..07 | P1 API | Specify | Pending |
| LP-DOC-01..04 | P2 Docs/example/decisions | Specify | Pending |

---

## Success Criteria

- [ ] Parity harness passes on the synthetic bursts and, when available, the 6 field sessions.
- [ ] No process abort across ECC-failure tests (flat frame, unrelated frame, huge shift).
- [ ] `yarn lint`, `yarn typecheck`, `yarn test`, `yarn test:native`, `yarn prepare` and CI example builds pass.
