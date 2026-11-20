#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Drive nj2k_large (bench/nj2k_large.cpp): the large-image gate of doc/bench.md.

usage: run_large.py --exe <nj2k_large> --files <dir> [--images g4096_97,...] [--threads 1,4,8]
                    [--decoders oracle-mem,neo-mem] [--rounds 3] [--budget-min 15] [--pin 7]
                    [--log FILE]
       run_large.py --exe <nj2k_large> --files <dir> --gen
       run_large.py --exe <nj2k_large> --files <dir> --sat <image> [--hosts 8] [--decodes 4]
                    [--pool 8] [--rounds 3] [--budget-min 15]

The gate: every image, round, thread count and decoder in its own process, the counts and decoders
taking turns in each round and the order of the counts reversed every other round; the 16,384-pixel
image two rounds at most. The run is time-boxed and never waits for a quiet machine: the total CPU
load is sampled every 5 s (typeperf on Windows, /proc/stat on Linux) and printed with the table. The
summary gives each image, count and decoder's median time and highest peak, and checks that every
decode of an image gave one hash.

--gen writes the images of the gate into --files (g4096_97: 4,096^2 one tile 9/7; g4096t_53: 4,096^2 in
16 tiles of 1,024, 5/3; g8192_97: 8,192^2 one tile 9/7; g16384t_97: 16,384^2 in 256 tiles of 1,024, 9/7;
and the odd sizes o4095_97 and o8191_97, 4,095 x 4,097 and 8,191 x 8,193 in one tile, 9/7).

--sat runs the saturated host: <hosts> threads decoding the image, one codec each, first on their own
threads and then all lent one pool, taking turns each round; it prints decodes a second and CPU time.
"""
import argparse
import os
import statistics
import subprocess
import sys
import threading
import time

IMAGES = {
    "g4096_97": (4096, 4096, 0, 1),
    "g4096t_53": (4096, 4096, 1024, 0),
    "g8192_97": (8192, 8192, 0, 1),
    "g16384t_97": (16384, 16384, 1024, 1),
    "o4095_97": (4095, 4097, 0, 1),
    "o8191_97": (8191, 8193, 0, 1),
}
GATE = ["g4096_97", "g4096t_53", "g8192_97", "g16384t_97"]


class Load:
    """The machine's total CPU load, sampled every 5 s while the run lasts."""

    def __init__(self):
        self.samples = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        prev = self.read()
        while not self.stop.wait(5):
            cur = self.read()
            if prev and cur and cur[1] > prev[1]:
                self.samples.append(100.0 * (1 - (cur[0] - prev[0]) / (cur[1] - prev[1])))
            prev = cur

    @staticmethod
    def read():
        if sys.platform == "win32":
            import ctypes

            idle, kernel, user = (ctypes.c_ulonglong(), ctypes.c_ulonglong(), ctypes.c_ulonglong())
            if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user)):
                return None
            return idle.value, kernel.value + user.value  # kernel time includes idle time
        try:
            with open("/proc/stat") as f:
                v = [int(x) for x in f.readline().split()[1:]]
            return v[3] + v[4], sum(v)
        except OSError:
            return None

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join()

    def text(self):
        if not self.samples:
            return "load: not sampled"
        return "load: total CPU %.0f %% mean, %.0f %% max (%d samples of 5 s)" % (
            statistics.mean(self.samples), max(self.samples), len(self.samples))


def run(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True, check=False).stdout.strip()
    return out.splitlines()[-1] if out else "FAILED (no output)"


def gate(a, log):
    images = a.images.split(",")
    counts = [int(x) for x in a.threads.split(",")]
    decoders = a.decoders.split(",")
    rows = {}
    start = time.monotonic()
    boxed = False
    with Load() as load:
        for img in images:
            path = os.path.join(a.files, img + ".j2k")
            rounds = min(a.rounds, 2) if img.startswith("g16384") else a.rounds
            for r in range(rounds):
                for n in (counts if r % 2 == 0 else list(reversed(counts))):
                    for dname in decoders:
                        cmd = [a.exe, "dec", dname, path, "--threads", str(n)]
                        if n == 1 and a.pin >= 0:
                            cmd += ["--pin", str(a.pin)]
                        line = run(cmd)
                        print("%s %s %d %d %s" % (img, dname, n, r + 1, line), file=log, flush=True)
                        f = line.split()
                        if len(f) >= 7 and f[0] == "ms":
                            rows.setdefault((img, n, dname), []).append((float(f[1]), float(f[3]), f[5], f[6]))
                if time.monotonic() - start > a.budget_min * 60:
                    boxed = True
                    break
            if boxed:
                break
    print("elapsed %.0f s%s; %s" % (time.monotonic() - start, ", time box reached" if boxed else "", load.text()))
    print("| Image | N | " + " | ".join("%s ms / MB" % d for d in decoders) + " | %s / %s |" % (decoders[-1], decoders[0]))
    print("|---|---|" + "---|" * len(decoders) + "---|")
    hashes = {}
    for img in images:
        for n in counts:
            cells, med = [], []
            for dname in decoders:
                v = rows.get((img, n, dname))
                if not v:
                    cells.append("-")
                    med.append(None)
                    continue
                m = statistics.median(x[0] for x in v)
                med.append(m)
                cells.append("%.0f / %.0f" % (m, max(x[1] for x in v)))
                for x in v:
                    hashes.setdefault(img, set()).add(x[2] if x[3] == "ok" else "FAILED")
            ratio = "%.2f" % (med[-1] / med[0]) if med[0] and med[-1] else "-"
            print("| %s | %d | %s | %s |" % (img, n, " | ".join(cells), ratio))
    for img, h in sorted(hashes.items()):
        print("%s: %s" % (img, "one hash, every decode" if len(h) == 1 and "FAILED" not in h else "DIFFERENT: %s" % sorted(h)))


def saturated(a, log):
    path = os.path.join(a.files, a.sat + ".j2k")
    res = {0: [], a.pool: []}
    start = time.monotonic()
    with Load() as load:
        for r in range(a.rounds):
            for n in ((0, a.pool) if r % 2 == 0 else (a.pool, 0)):
                line = run([a.exe, "sat", path, str(a.hosts), str(a.decodes), str(n)])
                print("%s sat %d %d %s" % (a.sat, n, r + 1, line), file=log, flush=True)
                f = line.split()
                if len(f) >= 9 and f[0] == "wall_ms":
                    res[n].append((float(f[1]), float(f[3]), float(f[5]), f[8]))
            if time.monotonic() - start > a.budget_min * 60:
                break
    print("elapsed %.0f s; %s" % (time.monotonic() - start, load.text()))
    print("| %s, %d hosts x %d decodes | decodes/s | CPU ms a decode | wall ms |" % (a.sat, a.hosts, a.decodes))
    print("|---|---|---|---|")
    for n, v in res.items():
        if v:
            per = a.hosts * a.decodes
            print("| %s | %.2f | %.0f | %.0f |" % ("one thread each" if n == 0 else "one pool of %d" % n,
                                                 statistics.median(x[2] for x in v),
                                                 statistics.median(x[1] for x in v) / per,
                                                 statistics.median(x[0] for x in v)))
    print("every decode as the one-thread hash:", all(x[3] == "ok" for v in res.values() for x in v))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", required=True)
    p.add_argument("--files", required=True)
    p.add_argument("--gen", action="store_true")
    p.add_argument("--images", default=",".join(GATE))
    p.add_argument("--threads", default="1,4,8")
    p.add_argument("--decoders", default="oracle-mem,neo-mem")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--budget-min", type=float, default=15)
    p.add_argument("--pin", type=int, default=7)
    p.add_argument("--sat")
    p.add_argument("--hosts", type=int, default=8)
    p.add_argument("--decodes", type=int, default=4)
    p.add_argument("--pool", type=int, default=8)
    p.add_argument("--log")
    a = p.parse_args()
    a.exe = os.path.abspath(a.exe)
    if sys.platform == "win32":
        import ctypes

        # no error box from a child that crashes (inherited): SEM_FAILCRITICALERRORS,
        # SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    if a.gen:
        os.makedirs(a.files, exist_ok=True)
        for name, (w, h, tile, lossy) in IMAGES.items():
            out = os.path.join(a.files, name + ".j2k")
            if not os.path.exists(out):
                print(run([a.exe, "gen", out, str(w), str(h), str(tile), str(lossy)]))
        return
    log = open(a.log, "a") if a.log else open(os.devnull, "w")
    with log:
        if a.sat:
            saturated(a, log)
        else:
            gate(a, log)


if __name__ == "__main__":
    main()
