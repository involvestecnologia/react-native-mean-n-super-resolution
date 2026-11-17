# lab-parity-mean Context

**Gathered:** 2026-11-16
**Spec:** `.specs/features/lab-parity-mean/spec.md`
**Status:** Ready for design

---

## Feature Boundary

Make `superResolve` produce the same image, from the same burst, as the `mean_N` method of the burst-sr-lab testbed (`~/repos/burst-sr-lab`, commit `05c7732`), with an optional bicubic upscale on top. The lab is the reference implementation; this library is its on-device port.

Lab sources the behavior is taken from:

- `algorithms/mfnr/filters.py`: `blur`, `apply_filters`, `MIN_KEEP = 3`, `DEFAULT = ["blur=0.80"]`
- `algorithms/mfnr/align.py`: `gray_small` (`SMALL = 1024`), `sharpness`, `pick_ref(first=8)`, `estimate_warp`, `warp`, `align` (`MIN_CC = 0.9`)
- `tools/session_report.py`: `phase_shift` (Hanning window CV_64F, `phaseCorrelate` on float64)
- `algorithms/mfnr/run.py:112-124`: temporal order `sorted(..., key=|id - id_ref|)`
- `algorithms/mfnr/methods.py`: `mean(b, n)` (float32 mean, `round`, uint8), `bicubic_x2` (`INTER_CUBIC`)
- `plans/modulo-nativo-mean-x2.md`: port plan, parity tolerances

## Differences from the current implementation (analysis 2026-11-16)

| Aspect | Lab `mean_N` | Current `cpp/sr` |
| ------ | ------------ | ---------------- |
| Output grid | mean at 1x (`frame_ref_x2` is a separate method) | each frame warped straight onto the s× grid, mean there |
| Warp interpolation | `warpPerspective`, `INTER_LINEAR \| WARP_INVERSE_MAP`, `BORDER_REPLICATE` | `warpAffine`, `INTER_CUBIC`, `BORDER_REPLICATE` |
| Reference | sharpest (Laplacian variance on 1024 px gray) of the first 8 survivors | always `paths[0]` |
| Frame filter | `blur=0.80`: drop sharpness < 0.80 × median, skipped if < 3 frames would remain | none |
| Motion model | phaseCorrelate init + ECC `MOTION_HOMOGRAPHY` (100 it / 1e-5, gaussFiltSize 5) on 1024 px gray | translation only (phaseCorrelate) |
| Acceptance | ECC cc ≥ 0.9; ECC error → dropped | NCC at the shift ≥ 0.85 |
| Frames averaged | N closest in time to the reference among the aligned ones | every aligned frame |
| Input | any count, N chosen separately | exactly 5, 15 or 30, all used |
| Align level | long side always scaled to 1024 (also upscaled) | only downscaled |
| Hanning window | CV_64F | CV_32F |
| Result | ref id, used ids, dropped with reason | path, width, height, framesUsed |

## Implementation Decisions (owner, 2026-11-16)

1. **Scale:** mean at 1x exactly like the lab, then, if `scale > 1`, `resize` with `INTER_CUBIC` by `scale`. Default `scale` becomes `1`.
2. **ECC:** vendor OpenCV's `ecc.cpp` into `cpp/sr/` with every `CV_Error`/throwing path turned into a failure return, so non-convergence drops the frame instead of aborting the process. New decision AD-006 amends the "no ECC" trade-off of AD-004.
3. **API:** free frame count, `n` (frames to average, default 8) and `blur` options, result gains `refIndex`, `used`, `dropped`. Breaking change.

## Agent's Discretion

- Two-pass decode (pass 1 keeps only sharpness, pass 2 decodes again to warp) vs keeping frames in memory — Design decides, guided by the memory budget.
- Parity harness layout (where the native CLI and the comparison script live).
