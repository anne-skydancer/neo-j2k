#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""The openjp2 module as an application sees it: two openjp2 libraries loaded by path, driven through the same
OpenJPEG C API (opj_*), decode and encode, at the same thread counts (opj_codec_set_threads on both).

usage: api_bench.py --openjpeg PATH --neoj2k PATH --encode-exe nj2k_encode_bench --large-exe nj2k_large
                    [--sizes 4096,1024,64] [--threads 1,8] [--modes lossy,lossless] [--rounds 5]
                    [--ops encode,decode] [--files DIR] [--budget-ms 2000] [--csv FILE]
       api_bench.py --check --encode-exe E [--large-exe L] --lib PATH [--lib PATH ...]

--openjpeg is a real OpenJPEG 2.5.4 openjp2.dll (cmake -DNJ2K_BENCH_OPENJP2_DLL=ON builds one as nj2k_oracle_openjp2,
with the module's flags and static runtime); --neoj2k is the shipped module (neoj2k-viewer.dll of the release zip, or
the neoj2k_openjp2 target): never run from inside a zip. Neither is linked into a program here: bench/nj2k_encode_bench
(--backend lib:<path>) and bench/nj2k_large (dec lib:<path>) load them by path and call opj_* through resolved
pointers, the encode with the viewer's upload sequence (RLCP, 64 x 64 code-blocks, 5/3 lossless or 9/7 with layers),
the decode from memory of a generated 3-component 9/7 image of one tile.

Every measurement is a fresh process, so its peak memory is its own. Each cell (operation, size, mode, thread count)
runs --rounds times, the two libraries in turn, the order alternating from round to round. Before every run the load
of other processes is sampled over 2 s: at most 10 % or it waits 30 s and samples again, at most 20 times (a run
that still saw more is counted and said under the table). One table: times are medians of the rounds, the change is a
percent (minus is faster than OpenJPEG, which is the base), Faster says yes or no, Identical says whether every run
of both libraries gave one codestream (encode) or one set of samples (decode), peak memory the highest of the rounds.

--check runs a tiny encode (64 pixels, 1 and 2 threads, both modes) of every --lib and of the linked oracle, one run
each, and exits 1 unless every codestream checksum is the oracle's (the ctest of the harness); with --large-exe and
two --lib it also decodes a generated 64-pixel image through both and compares their sample hashes.
"""
import argparse
import csv
import os
import statistics
import subprocess
import sys
import time

LOAD_LIMIT = 10.0  # percent of the machine other processes may use
LOAD_WINDOW = 2.0  # seconds sampled before a run
LOAD_WAIT = 30.0  # seconds waited before sampling again
LOAD_RETRIES = 20
REPEATS = {64: 50, 1024: 10}  # decodes a process times (the median is taken); larger images one


def cpu_times():
    """(idle, total) of the machine's CPU time, or None."""
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


def other_load():
    """The machine's CPU load over LOAD_WINDOW seconds, in percent: nothing of ours runs while it is sampled."""
    a = cpu_times()
    time.sleep(LOAD_WINDOW)
    b = cpu_times()
    if not a or not b or b[1] <= a[1]:
        return 0.0
    return 100.0 * (1 - (b[0] - a[0]) / (b[1] - a[1]))


class Gate:
    def __init__(self, enabled):
        self.enabled = enabled
        self.noisy = 0  # runs that went ahead on a busy machine
        self.waits = 0

    def wait(self):
        if not self.enabled:
            return
        for _ in range(LOAD_RETRIES + 1):
            load = other_load()
            if load <= LOAD_LIMIT:
                return
            self.waits += 1
            print("load %.0f %% (limit %.0f %%), waiting %.0f s" % (load, LOAD_LIMIT, LOAD_WAIT), file=sys.stderr)
            time.sleep(LOAD_WAIT)
        self.noisy += 1


def run(cmd, timeout):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, ""
    if out.returncode != 0:
        sys.stderr.write(out.stdout + out.stderr)
        return None, out.stderr
    return (out.stdout.strip().splitlines() or [""])[-1], out.stderr


def encode_once(exe, lib, size, mode, threads, budget_ms, min_iters, timeout):
    cmd = [exe, "--backend", lib, "--size", str(size), "--comps", "3", "--mode", mode, "--threads", str(threads),
           "--budget-ms", str(budget_ms), "--min-iters", str(min_iters)]
    line, err = run(cmd, timeout)
    if line is None:
        return None
    f = next(csv.reader([line]))
    return {"checksum": f[6], "bytes": f[5], "ms": float(f[8]), "peak": float(f[10]), "version": err.strip()}


def decode_once(exe, lib, path, threads, repeat, timeout):
    line, _ = run([exe, "dec", lib, path, "--threads", str(threads), "--repeat", str(repeat)], timeout)
    f = (line or "").split()
    if len(f) < 7 or f[0] != "ms" or f[6] != "ok":
        return None
    return {"checksum": f[5], "ms": float(f[1]), "peak": float(f[3])}


def generate(exe, folder, size):
    path = os.path.join(folder, "g%d_97.j2k" % size)
    if not os.path.exists(path):
        line, _ = run([exe, "gen", path, str(size), str(size), "0", "1"], 600)
        if not line or "written" not in line:
            sys.exit("could not generate " + path)
    return path


def percent(new, base):
    return 100.0 * (new - base) / base if base else float("nan")


def measure(libs, rounds, gate, once):
    """One cell's runs of both libraries, a fresh process each, the order alternating by round."""
    runs = {name: [] for name, _ in libs}
    for r in range(rounds):
        for name, spec in (libs if r % 2 == 0 else list(reversed(libs))):
            gate.wait()
            res = once(spec)
            if res is None:
                return None
            runs[name].append(res)
    return runs


def table(rows, gate, rounds):
    print("| Operation | Image | Mode | Threads | OpenJPEG ms | neo-j2k ms | Change % | Faster | Identical "
          "| OpenJPEG peak MB | neo-j2k peak MB | Peak change % |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for row in rows:
        op, image, mode, threads, runs = row
        if runs is None:
            print("| %s | %s | %s | %d | run failed | | | | | | | |" % (op, image, mode, threads))
            continue
        ref, neo = runs["openjpeg"], runs["neoj2k"]
        t_ref, t_neo = statistics.median(x["ms"] for x in ref), statistics.median(x["ms"] for x in neo)
        p_ref, p_neo = max(x["peak"] for x in ref), max(x["peak"] for x in neo)
        same = len({x["checksum"] for x in ref + neo}) == 1
        change = percent(t_neo, t_ref)
        print("| %s | %s | %s | %d | %.2f | %.2f | %+.1f | %s | %s | %.1f | %.1f | %+.1f |" % (
            op, image, mode, threads, t_ref, t_neo, change, "Yes" if change < 0 else "No", "Yes" if same else "No",
            p_ref, p_neo, percent(p_neo, p_ref)))
        sys.stdout.flush()
    print()
    print("%d rounds a row, a fresh process each, the two libraries in turn (order alternating); times are medians, "
          "minus is faster than OpenJPEG; %d runs went ahead on a machine busier than %.0f %% (%d waits of %.0f s)." % (
              rounds, gate.noisy, LOAD_LIMIT, gate.waits, LOAD_WAIT))


def check(a):
    """The harness's own test: every library's encode is the linked oracle's, byte for byte (the checksum column)."""
    bad = 0
    for mode in ("lossy", "lossless"):
        for threads in (1, 2):
            ref = encode_once(a.encode_exe, "oracle", 64, mode, 1, 1, 1, a.timeout)
            if ref is None:
                sys.exit("the linked oracle did not encode")
            for i, path in enumerate(a.lib):
                got = encode_once(a.encode_exe, "lib:%s=lib%d" % (path, i), 64, mode, threads, 1, 1, a.timeout)
                ok = got is not None and got["checksum"] == ref["checksum"] and got["bytes"] == ref["bytes"]
                print("%s, %d threads, %s: %s" % (mode, threads, path, "same codestream as the oracle" if ok else "DIFFERENT"))
                if got and got["version"]:
                    print("  " + got["version"])
                bad += not ok
    if a.large_exe and len(a.lib) >= 2:
        import tempfile

        with tempfile.TemporaryDirectory() as folder:
            image = generate(a.large_exe, folder, 64)
            hashes = []
            for threads in (1, 2):
                for i, path in enumerate(a.lib):
                    got = decode_once(a.large_exe, "lib:%s=lib%d" % (path, i), image, threads, 1, a.timeout)
                    hashes.append(got["checksum"] if got else None)
            ok = None not in hashes and len(set(hashes)) == 1
            print("decode, 64 pixels, %d libraries, 1 and 2 threads: %s" % (len(a.lib), "one set of samples" if ok else "DIFFERENT"))
            bad += not ok
    return 1 if bad else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--openjpeg", help="a real OpenJPEG 2.5.4 openjp2.dll (the base of every change)")
    p.add_argument("--neoj2k", help="the module under test (neoj2k-viewer.dll, openjp2.dll of the module)")
    p.add_argument("--encode-exe")
    p.add_argument("--large-exe")
    p.add_argument("--sizes", default="4096,1024,64")
    p.add_argument("--threads", default="1,8")
    p.add_argument("--modes", default="lossy,lossless", help="encode modes")
    p.add_argument("--ops", default="encode,decode")
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--files", help="folder of the generated images (default: a new one beside the table's csv or temp)")
    p.add_argument("--budget-ms", type=int, default=2000, help="encode time a process spends after its first")
    p.add_argument("--timeout", type=int, default=900)
    p.add_argument("--no-gate", action="store_true", help="do not wait for a quiet machine")
    p.add_argument("--csv")
    p.add_argument("--check", action="store_true")
    p.add_argument("--lib", action="append", default=[])
    a = p.parse_args()
    if sys.platform == "win32":
        import ctypes

        # no error box from a child that crashes (inherited)
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    if a.check:
        if not a.encode_exe or not a.lib:
            p.error("--check needs --encode-exe and --lib")
        a.encode_exe = os.path.abspath(a.encode_exe)
        a.large_exe = a.large_exe and os.path.abspath(a.large_exe)
        a.lib = [os.path.abspath(x) for x in a.lib]
        sys.exit(check(a))
    if not (a.openjpeg and a.neoj2k):
        p.error("--openjpeg and --neoj2k are needed")
    ops = a.ops.split(",")
    if ("encode" in ops and not a.encode_exe) or ("decode" in ops and not a.large_exe):
        p.error("--encode-exe and --large-exe are needed for the operations asked")
    libs = [("openjpeg", "lib:%s=openjpeg" % os.path.abspath(a.openjpeg)),
            ("neoj2k", "lib:%s=neoj2k" % os.path.abspath(a.neoj2k))]
    sizes = [int(s) for s in a.sizes.split(",")]
    counts = [int(s) for s in a.threads.split(",")]
    gate = Gate(not a.no_gate)
    rows = []
    folder = a.files
    if "decode" in ops:
        import tempfile

        folder = folder or tempfile.mkdtemp(prefix="api_bench_")
        os.makedirs(folder, exist_ok=True)
    for size in sizes:
        for threads in counts:
            if "encode" in ops:
                for mode in a.modes.split(","):
                    runs = measure(libs, a.rounds, gate, lambda spec: encode_once(
                        os.path.abspath(a.encode_exe), spec, size, mode, threads, a.budget_ms, 3, a.timeout))
                    rows.append(("encode", "%d x %d x 3" % (size, size), mode, threads, runs))
            if "decode" in ops:
                image = generate(os.path.abspath(a.large_exe), folder, size)
                repeat = REPEATS.get(size, 1)
                runs = measure(libs, a.rounds, gate, lambda spec: decode_once(
                    os.path.abspath(a.large_exe), spec, image, threads, repeat, a.timeout))
                rows.append(("decode", "%d x %d x 3" % (size, size), "lossy 9/7, one tile", threads, runs))
    table(rows, gate, a.rounds)
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["operation", "image", "mode", "threads", "library", "round", "ms", "peak_mb", "checksum"])
            for op, image, mode, threads, runs in rows:
                for name, rs in (runs or {}).items():
                    for i, x in enumerate(rs):
                        w.writerow([op, image, mode, threads, name, i + 1, x["ms"], x["peak"], x["checksum"]])


if __name__ == "__main__":
    main()
