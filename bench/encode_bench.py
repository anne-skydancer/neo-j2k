#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Stage 6a's encode bench: the viewer's upload encode of neo-j2k and of the oracle (OpenJPEG at the
pinned fork commit), time and peak memory, a fresh process for each encoder and size so the peak is its own.

usage: encode_bench.py --exe path/to/nj2k_encode_bench[.exe] [--sizes 64,128,...] [--comps 3]
                       [--budget-ms 2000] [--timeout 120] [--csv FILE]

Prints a Markdown table: bytes (and whether the codestreams are identical), median and best time of each,
the ratio of the medians, and each encoder's peak memory above its start. Time-boxed: each run takes at
most --budget-ms of encoding after its first, and --timeout seconds in all. The machine is not waited
for: run it as it is, say what else was running, and gate on nothing (stage 6a reports, stage 6b speeds).
"""
import argparse
import csv
import subprocess
import sys


def run(exe, backend, size, comps, mode, budget_ms, timeout):
    cmd = [exe, "--backend", backend, "--size", str(size), "--comps", str(comps), "--mode", mode,
           "--budget-ms", str(budget_ms), "--min-iters", "3"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        return None
    row = next(csv.reader([out.stdout.strip().splitlines()[-1]]))
    keys = ["backend", "w", "h", "comps", "mode", "bytes", "checksum", "iters", "median_ms", "min_ms", "peak_mb"]
    return dict(zip(keys, row))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--sizes", default="64,128,256,512,1024,2048")
    ap.add_argument("--comps", type=int, default=3)
    ap.add_argument("--budget-ms", type=int, default=2000)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--csv")
    a = ap.parse_args()
    rows = []
    print("| Size | Mode | Bytes | Identical | Oracle ms (median / best) | neo-j2k ms (median / best) | neo-j2k / oracle "
          "| Oracle peak MB | neo-j2k peak MB |")
    print("|---|---|---|---|---|---|---|---|---|")
    for size in [int(s) for s in a.sizes.split(",")]:
        for mode in ("lossy", "lossless"):
            o = run(a.exe, "oracle", size, a.comps, mode, a.budget_ms, a.timeout)
            n = run(a.exe, "neoj2k", size, a.comps, mode, a.budget_ms, a.timeout)
            if not o or not n:
                print("| %d | %s | | | run failed | | | | |" % (size, mode))
                continue
            rows += [o, n]
            ratio = float(n["median_ms"]) / float(o["median_ms"])
            print("| %d x %d x %d | %s | %s | %s | %.1f / %.1f | %.1f / %.1f | %.2f | %s | %s |" % (
                size, size, a.comps, mode, o["bytes"], "yes" if o["checksum"] == n["checksum"] and o["bytes"] == n["bytes"]
                else "NO", float(o["median_ms"]), float(o["min_ms"]), float(n["median_ms"]), float(n["min_ms"]), ratio,
                o["peak_mb"], n["peak_mb"]))
            sys.stdout.flush()
    if a.csv and rows:
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)


if __name__ == "__main__":
    main()
