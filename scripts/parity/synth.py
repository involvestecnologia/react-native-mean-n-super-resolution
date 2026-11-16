#!/usr/bin/env python3
"""Deterministic synthetic bursts for the lab parity harness (lab-parity-mean T12).

Writes <out>/<burst>/frames/frame_NNN.jpg (JPEG q95, like the lab capture) for:
  homography  12 frames, 1600x1200 multi-scale texture + text, each warped by a random homography
              (|shift| <= 6 px, |rotation| <= 0.4 deg, |scale - 1| <= 0.3 %), Gaussian noise sigma 4
  blurred     homography + frames 3, 6, 9 Gaussian-blurred sigma 2 (lab plans/filtro-de-frames.md)
  unrelated   homography with frame 5 replaced by another scene (ECC low cc or no convergence)
  small       640x480 frames, so the 1024 px gray image is an upscale

Same seed, same bytes: running it twice gives identical files.

Usage: python3 scripts/parity/synth.py [--out build/parity] [--only NAME ...]
"""
import argparse
import math
from pathlib import Path

import cv2
import numpy as np

SEED = 20261116
NOISE_SIGMA = 4.0
JPEG = [cv2.IMWRITE_JPEG_QUALITY, 95]
BURSTS = ("homography", "blurred", "unrelated", "small")


def scene(w, h, rng):
    """Multi-scale texture (blurred noise at several scales), shapes and text, uint8 BGR."""
    acc = np.zeros((h, w, 3), np.float32)
    for sigma, weight in ((1.0, 0.25), (3.0, 0.35), (12.0, 0.4)):
        noise = rng.normal(0, 1, (h, w, 3)).astype(np.float32)
        layer = cv2.GaussianBlur(noise, (0, 0), sigma)
        acc += weight * layer / (layer.std() + 1e-6)
    img = np.clip(128 + 40 * acc, 0, 255).astype(np.uint8)
    for _ in range(25):
        x, y = int(rng.integers(0, w)), int(rng.integers(0, h))
        color = tuple(int(c) for c in rng.integers(0, 256, 3))
        cv2.rectangle(img, (x, y), (x + int(rng.integers(20, w // 6)), y + int(rng.integers(20, h // 6))),
                      color, int(rng.integers(1, 4)), cv2.LINE_AA)
    for i in range(12):
        x, y = int(rng.integers(0, w - w // 3)), int(rng.integers(30, h))
        scale = float(rng.uniform(0.6, 2.0))
        cv2.putText(img, f"R$ {rng.integers(1, 999)},{rng.integers(0, 99):02d} SKU{i:03d}", (x, y),
                    cv2.FONT_HERSHEY_SIMPLEX, scale, (20, 20, 20), max(1, int(scale * 2)), cv2.LINE_AA)
    return img


def random_homography(w, h, rng):
    """Rotation + uniform scale about the centre, translation, tiny perspective terms."""
    deg = rng.uniform(-0.4, 0.4)
    s = 1 + rng.uniform(-0.003, 0.003)
    tx, ty = rng.uniform(-6, 6, 2)
    a = math.radians(deg)
    cx, cy = w / 2, h / 2
    c, si = s * math.cos(a), s * math.sin(a)
    H = np.array([[c, -si, cx - c * cx + si * cy + tx],
                  [si, c, cy - si * cx - c * cy + ty],
                  [0, 0, 1]], np.float64)
    H[2, :2] = rng.uniform(-2e-7, 2e-7, 2)
    return H


def frame(base, H, rng):
    w, h = base.shape[1], base.shape[0]
    f = cv2.warpPerspective(base, H, (w, h), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT_101)
    noisy = f.astype(np.float32) + rng.normal(0, NOISE_SIGMA, f.shape).astype(np.float32)
    return np.clip(np.rint(noisy), 0, 255).astype(np.uint8)


def burst(name):
    """List of uint8 BGR frames for one burst; each burst has its own seeded generator."""
    rng = np.random.default_rng([SEED, BURSTS.index(name)])
    w, h, count = (640, 480, 10) if name == "small" else (1600, 1200, 12)
    base = scene(w, h, rng)
    # why: every frame is warped (an unwarped one would always be the sharpest), so the reference comes from
    # the ~1 % sharpness spread and is not always frame 0.
    frames = [frame(base, random_homography(w, h, rng), rng) for _ in range(count)]
    if name == "blurred":
        for k in (3, 6, 9):
            frames[k] = cv2.GaussianBlur(frames[k], (0, 0), 2.0)
    if name == "unrelated":
        frames[5] = frame(scene(w, h, rng), random_homography(w, h, rng), rng)
    return frames


def write(out, name, frames):
    d = out / name / "frames"
    d.mkdir(parents=True, exist_ok=True)
    for old in d.glob("frame_*.jpg"):
        old.unlink()
    for k, f in enumerate(frames):
        ok, buf = cv2.imencode(".jpg", f, JPEG)
        if not ok:
            raise SystemExit(f"could not encode {name} frame {k}")
        (d / f"frame_{k:03d}.jpg").write_bytes(buf.tobytes())
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=Path("build/parity"))
    ap.add_argument("--only", nargs="+", choices=BURSTS, default=list(BURSTS))
    args = ap.parse_args()
    for name in args.only:
        d = write(args.out, name, burst(name))
        print(f"{name}: {len(list(d.glob('frame_*.jpg')))} frames -> {d}")


if __name__ == "__main__":
    main()
