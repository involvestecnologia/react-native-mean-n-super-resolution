#!/usr/bin/env python3
"""Compares the native merge of one burst against the burst-sr-lab golden (lab-parity-mean T13).

Reads <burst>/lab/{golden.json,mean.png,mean_x2.png} (lab_golden.py) and
<burst>/native/{native.json,mean.png,mean_x2.png} (sr_parity_cli), applies the spec tolerances:
  LP-PAR-01  same refIndex, same used (ids and order), same dropped (index and reason)
  LP-PAR-02  max displacement of the 4 image corners between native and lab H < 0.05 px
  LP-PAR-03  scale-1 mean: PSNR >= 45 dB and max abs diff <= 2 outside an 8 px border
  LP-PAR-04  scale-2 output vs lab bicubic_x2(mean): max abs diff <= 2 outside a 16 px border

Drop reasons are compared as text, except the cc of an "ECC cc X < 0.9, dropped" reason, which may
differ by CC_TOLERANCE (see validation.md: the cc of a frame that does not match the reference is
ill-conditioned; accepted frames agree to ~1e-6).

Usage: python3 scripts/parity/compare.py build/parity/<burst> [--json]
"""
import argparse
import json
import math
import re
import sys
from pathlib import Path

import cv2
import numpy as np

MAX_CORNER_PX = 0.05
MIN_PSNR_DB = 45.0
MAX_DIFF = 2
BORDER_X1 = 8
BORDER_X2 = 16
CC_TOLERANCE = 0.01
LOW_CC = re.compile(r"^ECC cc (-?\d+\.\d+) < (.*)$")


def expected_drops(golden):
    """Lab drops that native reports: every blur drop, and ECC drops only for frames native visits.

    The lab aligns every frame; native walks the blur-kept frames in temporal order and stops at the
    n-th accepted one, so an ECC drop after golden["used"][-1] in that walk is never seen natively.
    """
    drops = golden["dropped"]
    if len(golden["used"]) < len(golden["order"]):
        # the walk: blur-kept frames sorted by |id - ref|, ties by id (Python's stable sort over ids)
        blurred = {d["index"] for d in drops if d["reason"].endswith("(blur)")}
        total = max([*golden["order"], *(d["index"] for d in drops)]) + 1
        walk = sorted((i for i in range(total) if i not in blurred), key=lambda i: abs(i - golden["refIndex"]))
        last = walk.index(golden["used"][-1])
        visited = set(walk[:last + 1])
        drops = [d for d in drops if d["reason"].endswith("(blur)") or d["index"] in visited]
    return drops


def same_reason(native, lab):
    """Text equality, with the cc of a low-correlation drop allowed to differ by CC_TOLERANCE."""
    if native == lab:
        return True, 0.0
    a, b = LOW_CC.match(native), LOW_CC.match(lab)
    if a and b and a.group(2) == b.group(2):
        delta = abs(float(a.group(1)) - float(b.group(1)))
        return delta <= CC_TOLERANCE, delta
    return False, math.inf


def compare_ids(native, golden):
    """LP-PAR-01. Returns (ok, notes, max cc delta of low-cc reasons)."""
    notes = []
    if native["refIndex"] != golden["refIndex"]:
        notes.append(f"refIndex native {native['refIndex']} lab {golden['refIndex']}")
    if native["used"] != golden["used"]:
        notes.append(f"used native {native['used']} lab {golden['used']}")
    nat = {d["index"]: d["reason"] for d in native["dropped"]}
    lab = {d["index"]: d["reason"] for d in expected_drops(golden)}
    if sorted(nat) != sorted(lab):
        notes.append(f"dropped native {sorted(nat)} lab {sorted(lab)}")
    cc_delta = 0.0
    for i in sorted(set(nat) & set(lab)):
        ok, delta = same_reason(nat[i], lab[i])
        if not ok:
            notes.append(f"reason {i}: native {nat[i]!r} lab {lab[i]!r}")
        elif delta:
            cc_delta = max(cc_delta, delta)
    return not notes, notes, cc_delta


def corner_error(native, golden):
    """LP-PAR-02: max over used[1:] of the max distance between the 4 corners mapped by each H."""
    w, h = golden["width"], golden["height"]
    corners = np.array([[0, 0, 1], [w - 1, 0, 1], [w - 1, h - 1, 1], [0, h - 1, 1]], np.float64).T
    lab = {x["index"]: np.array(x["H"], np.float64) for x in golden["warps"]}
    nat = {x["index"]: np.array(x["H"], np.float64) for x in native["warps"]}
    worst = 0.0
    for i in sorted(set(lab) & set(nat)):
        a, b = nat[i] @ corners, lab[i] @ corners
        d = np.linalg.norm(a[:2] / a[2] - b[:2] / b[2], axis=0).max()
        worst = max(worst, float(d))
    return worst


def interior_max_diff(a, b, border):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return int(d[border:-border, border:-border].max())


def psnr(a, b):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return math.inf if mse == 0 else 10 * math.log10(255 ** 2 / mse)


def read(path):
    img = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"compare: cannot read {path}")
    return img


def compare(burst):
    lab_dir, nat_dir = burst / "lab", burst / "native"
    golden = json.loads((lab_dir / "golden.json").read_text())
    native = json.loads((nat_dir / "native.json").read_text())

    ids_ok, notes, cc_delta = compare_ids(native, golden)
    corner = corner_error(native, golden)

    row = {"burst": burst.name, "ids": ids_ok, "ccDelta": cc_delta, "corner": corner,
           "psnr": None, "diff": None, "diffX2": None, "notes": notes}
    m_nat, m_lab = read(nat_dir / "mean.png"), read(lab_dir / "mean.png")
    x_nat, x_lab = read(nat_dir / "mean_x2.png"), read(lab_dir / "mean_x2.png")
    if m_nat.shape != m_lab.shape or x_nat.shape != x_lab.shape:
        notes.append(f"shape native {m_nat.shape}/{x_nat.shape} lab {m_lab.shape}/{x_lab.shape}")
    else:
        row["psnr"] = psnr(m_nat, m_lab)
        row["diff"] = interior_max_diff(m_nat, m_lab, BORDER_X1)
        row["diffX2"] = interior_max_diff(x_nat, x_lab, BORDER_X2)

    checks = [ids_ok, corner < MAX_CORNER_PX, row["psnr"] is not None and row["psnr"] >= MIN_PSNR_DB,
              row["diff"] is not None and row["diff"] <= MAX_DIFF,
              row["diffX2"] is not None and row["diffX2"] <= MAX_DIFF]
    if corner >= MAX_CORNER_PX:
        notes.append(f"corner error {corner:.4f} px >= {MAX_CORNER_PX}")
    if row["psnr"] is not None:
        if row["psnr"] < MIN_PSNR_DB:
            notes.append(f"PSNR {row['psnr']:.1f} dB < {MIN_PSNR_DB}")
        if row["diff"] > MAX_DIFF:
            notes.append(f"x1 max abs diff {row['diff']} > {MAX_DIFF} outside {BORDER_X1} px")
        if row["diffX2"] > MAX_DIFF:
            notes.append(f"x2 max abs diff {row['diffX2']} > {MAX_DIFF} outside {BORDER_X2} px")
    row["pass"] = all(checks)
    return row


HEADER = f"{'burst':<14} {'ids/reasons':<12} {'corner px':>9} {'PSNR dB':>8} {'diff x1':>7} {'diff x2':>7}  result"


def format_row(r):
    psnr_text = "-" if r["psnr"] is None else ("inf" if math.isinf(r["psnr"]) else f"{r['psnr']:.1f}")
    ids = ("match" if r["ids"] else "MISMATCH") + (f" ~{r['ccDelta']:.3f}" if r["ccDelta"] else "")
    line = (f"{r['burst']:<14} {ids:<12} {r['corner']:>9.4f} {psnr_text:>8} "
            f"{'-' if r['diff'] is None else r['diff']:>7} {'-' if r['diffX2'] is None else r['diffX2']:>7}  "
            f"{'pass' if r['pass'] else 'FAIL'}")
    return "\n".join([line] + [f"    {n}" for n in r["notes"]])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("burst", type=Path, help="directory with lab/ and native/ outputs")
    ap.add_argument("--json", action="store_true", help="print the row as JSON")
    args = ap.parse_args()
    row = compare(args.burst)
    print(json.dumps(row) if args.json else HEADER + "\n" + format_row(row))
    sys.exit(0 if row["pass"] else 1)


if __name__ == "__main__":
    main()
