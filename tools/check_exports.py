#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Check what the shared library exports: exactly the functions neoj2k.h declares, all nj2k_*, and the
committed list abi/exports.txt (so every API change shows in review).

usage: check_exports.py --library neoj2k.dll|libneoj2k.so --header include/neoj2k/neoj2k.h --list abi/exports.txt
       check_exports.py --header include/neoj2k/neoj2k.h --write-list abi/exports.txt
       check_exports.py --selftest

Windows DLLs are read with a small PE export-table parser (no tools needed); ELF shared objects with
`nm -D --defined-only`.
"""
import argparse
import re
import struct
import subprocess
import sys


def header_functions(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return sorted(set(re.findall(r"NJ2K_API\b[^;{]*?\b(nj2k_\w+)\s*\(", text)))


def pe_exports(data):
    """Names in the export directory of a PE image."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    coff = pe + 4
    nsections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dirs)
    sections = []
    sec = opt + opt_size
    for i in range(nsections):
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError("RVA outside the sections")

    if not export_rva:
        return []
    d = off(export_rva)
    nnames, names_rva = struct.unpack_from("<I", data, d + 24)[0], struct.unpack_from("<I", data, d + 32)[0]
    names = []
    for i in range(nnames):
        p = off(struct.unpack_from("<I", data, off(names_rva) + 4 * i)[0])
        end = data.index(b"\0", p)
        names.append(data[p:end].decode("ascii"))
    return sorted(names)


def elf_exports(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            name = parts[2].split("@")[0]
            names.append((parts[1], name))
    # the version node symbol (NJ2K_0, type A) is not an export of code or data
    return sorted(n for t, n in names if t != "A")


def library_exports(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] == b"MZ":
        return pe_exports(data)
    if data[:4] == b"\x7fELF":
        return elf_exports(path)
    raise ValueError("unknown library format: " + path)


def read_list(path):
    with open(path, encoding="utf-8") as f:
        return sorted(l.strip() for l in f if l.strip() and not l.startswith("#"))


def selftest():
    sample = "NJ2K_API int NJ2K_CALLCONV nj2k_a(void);\n/* NJ2K_API void nj2k_b(void); */\nNJ2K_API nj2k_x_t* NJ2K_CALLCONV\n nj2k_c(int);\n"
    import tempfile, os
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as f:
        f.write(sample)
    try:
        assert header_functions(f.name) == ["nj2k_a", "nj2k_c"], header_functions(f.name)
    finally:
        os.unlink(f.name)
    print("check_exports: selftest passed")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library")
    ap.add_argument("--header")
    ap.add_argument("--list")
    ap.add_argument("--write-list")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    declared = header_functions(a.header)
    if a.write_list:
        with open(a.write_list, "w", encoding="utf-8", newline="\n") as f:
            f.write("# SPDX-License-Identifier: BSD-2-Clause\n")
            f.write("# The shared library's exports: tools/check_exports.py --write-list after an API change.\n")
            f.write("\n".join(declared) + "\n")
        print("check_exports: wrote %d names to %s" % (len(declared), a.write_list))
        return 0
    problems = []
    listed = read_list(a.list)
    if listed != declared:
        problems.append("abi/exports.txt and neoj2k.h differ: listed only %s; declared only %s" % (
            sorted(set(listed) - set(declared)), sorted(set(declared) - set(listed))))
    exported = library_exports(a.library)
    for name in exported:
        if not name.startswith("nj2k_"):
            problems.append("exported outside the nj2k_ namespace: " + name)
    missing = sorted(set(declared) - set(exported))
    extra = sorted(set(exported) - set(declared))
    if missing:
        problems.append("declared but not exported: %s" % missing)
    if extra:
        problems.append("exported but not declared: %s" % extra)
    for p in problems:
        print("check_exports: " + p, file=sys.stderr)
    if problems:
        return 1
    print("check_exports: %s exports exactly the %d functions neoj2k.h declares" % (a.library, len(declared)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
