#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Every file neo-j2k adds carries its licence: an SPDX-License-Identifier: BSD-2-Clause line near the
top. Files ported from OpenJPEG keep OpenJPEG's notice as well. The vendored oracle (oracle/openjp2,
OpenJPEG's own files with their own notices), the generated corpus and binary files are exempt.

usage: check_headers.py [--root DIR]
"""
import argparse
import os
import subprocess
import sys

EXEMPT_DIRS = ("oracle/openjp2/", "corpus/")
EXEMPT_FILES = {"LICENSE", ".gitattributes", ".gitignore", "CMakePresets.json"}  # JSON has no comments
TEXT_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".py", ".cmake", ".txt", ".md", ".yml", ".yaml",
                 ".json", ".in", ".map", ".sh", ".cmd", ".dict", ".toml", ".patch")
OPENJPEG_DERIVED = ("include/neoj2k/neoj2k.h", "src/image.cpp", "src/defaults.cpp", "src/header.cpp",
                    "src/jp2.cpp", "src/codec.cpp", "src/stream.cpp")


def tracked_files(root):
    out = subprocess.run(["git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard"],
                         capture_output=True, text=True, check=True).stdout
    return sorted(set(out.split("\n")) - {""})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    a = ap.parse_args()
    problems, checked = [], 0
    for rel in tracked_files(a.root):
        if rel.startswith(EXEMPT_DIRS) or os.path.basename(rel) in EXEMPT_FILES:
            continue
        name = os.path.basename(rel)
        if not (rel.endswith(TEXT_SUFFIXES) or name in ("CMakeLists.txt", "Dockerfile", "PIN")):
            continue
        path = os.path.join(a.root, rel)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            head = f.read(4000)
        checked += 1
        if "SPDX-License-Identifier: BSD-2-Clause" not in head:
            problems.append(rel + ": no SPDX-License-Identifier: BSD-2-Clause near the top")
        if rel in OPENJPEG_DERIVED and "Universite catholique de Louvain" not in head and "Herve Drolon" not in head:
            problems.append(rel + ": derived from OpenJPEG but OpenJPEG's notice is missing")
    for p in problems:
        print("check_headers: " + p, file=sys.stderr)
    if problems:
        return 1
    print("check_headers: %d files carry their licence" % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
