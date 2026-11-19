#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Vendor or verify the test oracle: OpenJPEG's library sources at the pinned fork commit, plus patches.

oracle/PIN names the repository, the commit and the patches (in oracle/patches/, applied in order).
The library sources the shipped package compiles (no GPU back-ends, no tools) are copied to
oracle/openjp2/ with the fork's LICENSE.

usage: oracle_sync.py [--verify] [--commit SHA] [--source DIR]
  (no option)    fetch the pinned commit, apply the patches, replace oracle/openjp2/
  --verify       do the same in a temporary folder and compare with oracle/openjp2/; exit 1 on any difference
  --commit SHA   pin another commit (written to oracle/PIN; give --patches '' to drop the patches)
  --patches L    space-separated patch list for a new pin
  --source DIR   take the sources from a local checkout of the fork instead of fetching
                 (its HEAD must be the pinned commit unless --allow-dirty)

Needs git. The fetch is one commit deep: GitHub serves any reachable commit by its hash.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.path.join(ROOT, "oracle")
DEST = os.path.join(ORACLE, "openjp2")
PIN = os.path.join(ORACLE, "PIN")

# What the shipped package compiles (fork src/lib/openjp2/CMakeLists.txt, OPENJPEG_SRCS, with
# OPJ_ENABLE_OPENCL=OFF and OPJ_ENABLE_CUDA=OFF) and the headers those sources include.
SOURCES = [
    "thread.c", "bio.c", "cio.c", "dwt.c", "event.c", "ht_dec.c", "image.c", "invert.c", "j2k.c", "jp2.c",
    "mct.c", "mqc.c", "openjpeg.c", "opj_clock.c", "pi.c", "t1.c", "t2.c", "tcd.c", "tgt.c",
    "function_list.c", "opj_malloc.c", "sparse_array.c",
]
HEADERS = [
    "thread.h", "bio.h", "cio.h", "dwt.h", "event.h", "image.h", "invert.h", "j2k.h", "jp2.h", "mct.h",
    "mqc.h", "mqc_inl.h", "openjpeg.h", "opj_clock.h", "pi.h", "t1.h", "t2.h", "tcd.h", "tgt.h",
    "function_list.h", "opj_codec.h", "opj_common.h", "opj_includes.h", "opj_intmath.h", "opj_malloc.h",
    "sparse_array.h", "t1_luts.h", "t1_ht_luts.h", "tls_keys.h",
    "opj_config.h.cmake.in", "opj_config_private.h.cmake.in",
]
LIBDIR = "src/lib/openjp2"


def read_pin():
    pin = {}
    with open(PIN, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            pin[key.strip()] = value.strip()
    pin["patches"] = pin.get("patches", "").split()
    return pin


def write_pin(pin):
    lines = []
    with open(PIN, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                lines.append(line.rstrip("\n"))
    lines.append("repo=" + pin["repo"])
    lines.append("commit=" + pin["commit"])
    lines.append("patches=" + " ".join(pin["patches"]))
    with open(PIN, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def git(args, cwd):
    subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "advice.detachedHead=false"] + args, cwd=cwd,
                   check=True, stdout=subprocess.DEVNULL)


def fetch(pin, work):
    git(["init", "-q", work], cwd=None)
    git(["fetch", "-q", "--depth", "1", pin["repo"], pin["commit"]], cwd=work)
    git(["checkout", "-q", "FETCH_HEAD"], cwd=work)


def copy_source_tree(src, work):
    """A local checkout: copy only what is vendored, plus LICENSE, normalised to LF."""
    os.makedirs(os.path.join(work, LIBDIR))
    for name in SOURCES + HEADERS:
        with open(os.path.join(src, LIBDIR, name), "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")
        with open(os.path.join(work, LIBDIR, name), "wb") as f:
            f.write(data)
    with open(os.path.join(src, "LICENSE"), "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    with open(os.path.join(work, "LICENSE"), "wb") as f:
        f.write(data)
    git(["init", "-q", "."], cwd=work)


def apply_patches(pin, work):
    for name in pin["patches"]:
        path = os.path.join(ORACLE, "patches", name)
        git(["apply", "--whitespace=nowarn", path], cwd=work)


def stage(pin, work, source):
    if source:
        copy_source_tree(source, work)
    else:
        fetch(pin, work)
    apply_patches(pin, work)
    out = os.path.join(work, "_out")
    os.makedirs(out)
    for name in SOURCES + HEADERS:
        shutil.copyfile(os.path.join(work, LIBDIR, name), os.path.join(out, name))
    shutil.copyfile(os.path.join(work, "LICENSE"), os.path.join(out, "LICENSE"))
    return out


def lf_bytes(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n")


def compare(staged, dest):
    want = sorted(os.listdir(staged))
    have = sorted(os.listdir(dest)) if os.path.isdir(dest) else []
    problems = []
    for name in sorted(set(want) | set(have)):
        if name not in have:
            problems.append("missing in oracle/openjp2: " + name)
        elif name not in want:
            problems.append("not part of the pinned sources: " + name)
        elif lf_bytes(os.path.join(staged, name)) != lf_bytes(os.path.join(dest, name)):
            problems.append("differs from the pinned commit plus patches: " + name)
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--commit")
    ap.add_argument("--patches")
    ap.add_argument("--source")
    a = ap.parse_args()
    pin = read_pin()
    if a.commit:
        pin["commit"] = a.commit
    if a.patches is not None:
        pin["patches"] = a.patches.split()
    with tempfile.TemporaryDirectory(prefix="nj2k-oracle-") as tmp:
        work = os.path.join(tmp, "src")
        staged = stage(pin, work, a.source)
        if a.verify:
            problems = compare(staged, DEST)
            for p in problems:
                print("oracle_sync: " + p, file=sys.stderr)
            if problems:
                return 1
            print("oracle_sync: oracle/openjp2 equals %s at %s plus %s" %
                  (pin["repo"], pin["commit"][:12], " ".join(pin["patches"]) or "no patches"))
            return 0
        if os.path.isdir(DEST):
            shutil.rmtree(DEST)
        os.makedirs(DEST)
        for name in os.listdir(staged):
            with open(os.path.join(staged, name), "rb") as f:
                data = f.read().replace(b"\r\n", b"\n")
            with open(os.path.join(DEST, name), "wb") as f:
                f.write(data)
        if a.commit or a.patches is not None:
            write_pin(pin)
        print("oracle_sync: vendored %d files from %s at %s plus %s" %
              (len(os.listdir(DEST)), pin["repo"], pin["commit"][:12], " ".join(pin["patches"]) or "no patches"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
