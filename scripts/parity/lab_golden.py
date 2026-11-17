#!/usr/bin/env python3
"""Lab side of the parity harness (lab-parity-mean T12): runs burst-sr-lab's own mean_N code on a
burst and writes what scripts/parity/compare.py checks the native output against.

Replicates algorithms/mfnr/run.py:112-124 (filters, reference, alignment, temporal order) and then
methods.mean and methods.bicubic_x2. Writes into <burst>/lab/ (or --out):
  golden.json  refIndex, order (every aligned frame, temporal order), used (first n of order),
               dropped (every lab drop: blur and ECC, including ECC drops past the n-th used frame),
               warps (full-resolution H and cc of used[1:], from the lab's estimate_warp)
  mean.png     methods.mean(burst, n)
  mean_x2.png  methods.bicubic_x2(mean)

Usage: LAB_DIR=~/repos/burst-sr-lab python3 scripts/parity/lab_golden.py build/parity/<burst> [--n 8]
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

LAB_FILES = ("algorithms/mfnr/align.py", "algorithms/mfnr/filters.py", "algorithms/mfnr/methods.py",
             "tools/session_report.py")
LOG_DROP = re.compile(r"^frame (\d{3}): (.*)$")


def lab_modules(lab_dir):
    """Imports the lab's align, filters and methods from lab_dir, or exits with a clear message."""
    if not lab_dir:
        raise SystemExit("lab_golden: set LAB_DIR (or --lab) to a burst-sr-lab checkout")
    lab = Path(lab_dir).expanduser().resolve()
    missing = [f for f in LAB_FILES if not (lab / f).is_file()]
    if missing:
        raise SystemExit(f"lab_golden: {lab} is not a burst-sr-lab checkout (missing {', '.join(missing)})")
    sys.path.insert(0, str(lab / "algorithms" / "mfnr"))
    import align  # noqa: E402  (align.py adds tools/ to sys.path itself)
    import filters  # noqa: E402
    import methods  # noqa: E402
    return align, filters, methods


def parse_drops(log):
    """Lab log lines "frame NNN: <reason>" -> [{index, reason}] (warning lines are not drops)."""
    drops = []
    for line in log:
        m = LOG_DROP.match(line)
        if m:
            drops.append({"index": int(m.group(1)), "reason": m.group(2)})
    return drops


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("burst", type=Path, help="directory with frames/frame_NNN.jpg")
    ap.add_argument("--lab", default=os.environ.get("LAB_DIR"), help="burst-sr-lab checkout (default $LAB_DIR)")
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--filters", nargs="+", default=["blur=0.80"])
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    align, filters, methods = lab_modules(args.lab)
    import cv2
    import numpy as np

    paths = sorted((args.burst / "frames").glob("frame_*.jpg"))
    if not paths:
        raise SystemExit(f"lab_golden: no frames/frame_*.jpg in {args.burst}")
    out = args.out or args.burst / "lab"
    out.mkdir(parents=True, exist_ok=True)

    # run.py:112-124 (load == cv2.imread, no rotation)
    frames = [cv2.imread(str(p)) for p in paths]
    frames, ids, filter_log = filters.apply_filters(frames, args.filters)
    ref = align.pick_ref(frames)
    aligned, kept, align_log = align.align(frames, ref, ids)
    order = sorted(range(len(kept)), key=lambda i: abs(ids[kept[i]] - ids[ref]))
    burst = methods.Burst([aligned[i] for i in order], [frames[kept[i]] for i in order], 0.0)

    mean = methods.mean(burst, args.n)
    cv2.imwrite(str(out / "mean.png"), mean)
    cv2.imwrite(str(out / "mean_x2.png"), methods.bicubic_x2(mean))

    order_ids = [ids[kept[i]] for i in order]
    used = order_ids[:args.n]
    ref_small, _ = align.gray_small(frames[ref])
    warps = []
    for fid in used[1:]:
        H, cc = align.estimate_warp(ref_small, frames[ids.index(fid)])
        warps.append({"index": fid, "H": np.asarray(H, np.float64).tolist(), "cc": float(cc)})

    golden = {"refIndex": ids[ref], "order": order_ids, "used": used,
              "dropped": parse_drops(filter_log + align_log), "warps": warps,
              "width": int(mean.shape[1]), "height": int(mean.shape[0]), "log": filter_log + align_log}
    (out / "golden.json").write_text(json.dumps(golden, indent=2) + "\n")
    print(f"{args.burst.name}: ref {golden['refIndex']}, used {used}, dropped "
          f"{[(d['index'], d['reason']) for d in golden['dropped']]} -> {out}")


if __name__ == "__main__":
    main()
