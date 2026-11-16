#!/usr/bin/env python3
"""Parity harness runner (lab-parity-mean T13): synth -> lab golden -> native cli -> compare.

For each synthetic burst (synth.py) and each field session under --sessions (a directory of
<session>/frames/frame_NNN.jpg, as pulled by the lab's tools/pull_session.sh), runs lab_golden.py and
sr_parity_cli with the same n and blur, then prints one compare.py row per burst. Exits 1 if any
burst fails.

Usage: LAB_DIR=~/repos/burst-sr-lab python3 scripts/parity/run.py [--sessions DIR] [--n 8] [--blur 0.8]
       (yarn test:parity builds sr_parity_cli first, via the host test build)
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import compare  # noqa: E402
import lab_golden  # noqa: E402
import synth  # noqa: E402


def cli_path():
    exe = "sr_parity_cli.exe" if os.name == "nt" else "sr_parity_cli"
    for p in (ROOT / "build/native-tests" / exe, ROOT / "build/native-tests/Release" / exe):
        if p.is_file():
            return p
    raise SystemExit("run: sr_parity_cli not built; run `yarn test:native` (or `yarn test:parity`) first")


def step(label, cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{label} exited {r.returncode}: {(r.stderr or r.stdout).strip()}")


def run_burst(burst, lab, cli, n, blur):
    frames = sorted((burst / "frames").glob("frame_*.jpg"))
    for sub in ("lab", "native"):
        shutil.rmtree(burst / sub, ignore_errors=True)
    step("lab_golden", [sys.executable, HERE / "lab_golden.py", burst, "--lab", lab, "--n", n, "--filters", f"blur={blur:.2f}"])
    step("sr_parity_cli", [cli, "--n", n, "--blur", blur, "--out", burst / "native", *frames])
    return compare.compare(burst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lab", default=os.environ.get("LAB_DIR"), help="burst-sr-lab checkout (default $LAB_DIR)")
    ap.add_argument("--sessions", type=Path, help="directory of field sessions (<session>/frames/*.jpg)")
    ap.add_argument("--out", type=Path, default=ROOT / "build/parity")
    ap.add_argument("--n", type=int, default=8)
    ap.add_argument("--blur", type=float, default=0.8)
    args = ap.parse_args()

    lab_golden.lab_modules(args.lab)  # fails fast with the clear LAB_DIR message
    cli = cli_path()
    step("synth", [sys.executable, HERE / "synth.py", "--out", args.out])
    bursts = [args.out / name for name in synth.BURSTS]
    if args.sessions:
        found = sorted(p for p in args.sessions.iterdir() if (p / "frames").is_dir())
        if not found:
            raise SystemExit(f"run: no <session>/frames/ under {args.sessions}")
        # why: lab and native outputs go next to the frames; keep them out of the session folder.
        for s in found:
            dst = args.out / "sessions" / s.name
            (dst / "frames").mkdir(parents=True, exist_ok=True)
            for f in (s / "frames").glob("frame_*.jpg"):
                if not (dst / "frames" / f.name).exists():
                    shutil.copy2(f, dst / "frames" / f.name)
            bursts.append(dst)

    print(compare.HEADER)
    failed = 0
    for burst in bursts:
        try:
            row = run_burst(burst, args.lab, cli, args.n, args.blur)
        except RuntimeError as e:
            row = {"burst": burst.name, "ids": False, "ccDelta": 0, "corner": 0.0, "psnr": None,
                   "diff": None, "diffX2": None, "pass": False, "notes": [str(e)]}
        failed += not row["pass"]
        print(compare.format_row(row), flush=True)
    if not args.sessions:
        print("field sessions: not run (pass --sessions DIR); open evidence gap")
    print(f"{len(bursts) - failed}/{len(bursts)} bursts pass")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
