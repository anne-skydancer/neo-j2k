#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Check what the openjp2 module exports: exactly the functions OpenJPEG's openjpeg.h declares (the oracle's:
OpenJPEG 2.5.4 plus the fork's decoder limits API), all opj_*, and none of neo-j2k's own (nj2k_*), so that a
viewer which loads OpenJPEG finds what it expects and nothing it does not; and a Windows library says in its
version resource that it is neo-j2k and not OpenJPEG.

usage: check_openjp2.py --library openjp2.dll|libopenjp2.so.7 --header oracle/openjp2/openjpeg.h
       check_openjp2.py --selftest

The library is read by tools/check_exports.py's readers (a PE export-table parser; `nm -D` for ELF).
"""
import argparse
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# in the Windows version resource (modules/openjp2/openjp2.rc.in): what Explorer shows for a file that a viewer's
# folder calls openjp2.dll
IDENTITY = "This is neo-j2k, not OpenJPEG."


def load_check_exports():
    spec = importlib.util.spec_from_file_location("check_exports", os.path.join(HERE, "check_exports.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def header_functions(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return sorted(set(re.findall(r"\bOPJ_API\b[^;{]*?\b(opj_\w+)\s*\(", text)))


def selftest():
    import tempfile
    sample = ("OPJ_API int OPJ_CALLCONV opj_a(void);\n/* OPJ_API void opj_b(void); */\n"
              "OPJ_API opj_x_t* OPJ_CALLCONV\n opj_c(int);\n// OPJ_API void opj_d(void);\n")
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as f:
        f.write(sample)
    try:
        assert header_functions(f.name) == ["opj_a", "opj_c"], header_functions(f.name)
    finally:
        os.unlink(f.name)
    print("check_openjp2: selftest passed")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library")
    ap.add_argument("--header")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.library or not a.header:
        ap.error("--library and --header are required")
    declared = header_functions(a.header)
    if not declared:
        print("check_openjp2: no function read from " + a.header, file=sys.stderr)
        return 2
    exported = load_check_exports().library_exports(a.library)
    problems = []
    with open(a.library, "rb") as f:
        data = f.read()
    if data[:2] == b"MZ" and IDENTITY.encode("utf-16-le") not in data:
        problems.append("the file does not say what it is (its version resource lacks '%s')" % IDENTITY)
    for name in exported:
        if not name.startswith("opj_"):
            problems.append("exported outside the opj_ namespace: " + name)
    missing = sorted(set(declared) - set(exported))
    extra = sorted(n for n in set(exported) - set(declared) if n.startswith("opj_"))
    if missing:
        problems.append("declared by openjpeg.h but not exported: %s" % missing)
    if extra:
        problems.append("exported but not declared by openjpeg.h: %s" % extra)
    for p in problems:
        print("check_openjp2: " + p, file=sys.stderr)
    if problems:
        return 1
    print("check_openjp2: %s exports exactly the %d functions openjpeg.h declares" % (a.library, len(declared)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
