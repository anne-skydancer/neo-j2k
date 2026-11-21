#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Package the openjp2 module's library as a zip that stands alone: the library, its install instructions, its
checksum and the licences, nothing else.

Nothing in the download is called OpenJPEG or openjp2: the library is shipped as neoj2k-viewer.dll
(libneoj2k-viewer.so on Linux), and the instructions rename it to openjp2.dll when it goes into the viewer's
folder, the one place where the viewer's own name for it matters.

usage: package_openjp2.py --library build/.../openjp2.dll [--out dist] [--platform win-x64]

Before it writes anything it checks the library (and stops if a check fails):
  - it exports exactly the functions of OpenJPEG's openjpeg.h and no nj2k_* (tools/check_openjp2.py);
  - on Windows it imports no C or C++ runtime (MSVCP*, VCRUNTIME*, UCRTBASE, api-ms-win-crt-*): a build with
    NJ2K_MSVC_STATIC_RUNTIME=ON, which needs nothing installed on the machine it is dropped into;
  - it says "provided by neo-j2k <version>" with the version of include/neoj2k/neoj2k.h.

The zip is neoj2k-viewer-<version>-<platform>.zip, with fixed timestamps and member order, so the same library
gives the same zip.
"""
import argparse
import hashlib
import importlib.util
import os
import re
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, "tools")
HEADER = os.path.join(ROOT, "include", "neoj2k", "neoj2k.h")
OPENJPEG = os.path.join(ROOT, "oracle", "openjp2", "openjpeg.h")
INSTALL = os.path.join(ROOT, "modules", "openjp2", "INSTALL.md")
LICENCES = ["LICENSE", "LICENSE.OpenJPEG", "NOTICE.md"]
RUNTIME = re.compile(r"^(msvcp|vcruntime|ucrtbase|api-ms-win-crt-|msvcr|concrt|vcomp)", re.I)
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def version():
    with open(HEADER, encoding="utf-8") as f:
        text = f.read()
    return ".".join(re.search(r"#define NJ2K_VERSION_%s (\d+)" % k, text).group(1) for k in ("MAJOR", "MINOR", "PATCH"))


def pe_imports(data):
    """The DLL names in the import directory of a PE image."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    coff = pe + 4
    nsections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    rva, _ = struct.unpack_from("<II", data, dirs + 8)  # directory 1: imports
    sections = []
    sec = opt + opt_size
    for i in range(nsections):
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, max(vsize, rawsize), rawptr))

    def off(r):
        for va, size, raw in sections:
            if va <= r < va + size:
                return raw + r - va
        raise ValueError("RVA outside the sections")

    names = []
    if not rva:
        return names
    d = off(rva)
    while True:
        name_rva = struct.unpack_from("<I", data, d + 12)[0]
        if not name_rva:
            break
        p = off(name_rva)
        names.append(data[p:data.index(b"\0", p)].decode("ascii"))
        d += 20
    return names


def check_library(path, data):
    problems = []
    check = load("check_openjp2")
    declared = check.header_functions(OPENJPEG)
    exported = load("check_exports").library_exports(path)
    if sorted(exported) != declared:
        problems.append("exports are not exactly openjpeg.h's %d functions: missing %s, extra %s" % (
            len(declared), sorted(set(declared) - set(exported)), sorted(set(exported) - set(declared))))
    if data[:2] == b"MZ":
        runtime = [n for n in pe_imports(data) if RUNTIME.match(n)]
        if runtime:
            problems.append("it imports a C/C++ runtime (%s): build with NJ2K_MSVC_STATIC_RUNTIME=ON" % ", ".join(runtime))
    want = ("provided by neo-j2k %s" % version()).encode("ascii")
    if want not in data:
        problems.append("it does not carry the string '%s' (a stale build?)" % want.decode())
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--platform", default=None, help="win-x64 (the default for a .dll) or linux-x64")
    a = ap.parse_args()
    with open(a.library, "rb") as f:
        data = f.read()
    problems = check_library(a.library, data)
    for p in problems:
        print("package_openjp2: " + p, file=sys.stderr)
    if problems:
        return 1
    platform = a.platform or ("win-x64" if data[:2] == b"MZ" else "linux-x64")
    member = "neoj2k-viewer.dll" if data[:2] == b"MZ" else "libneoj2k-viewer.so"
    digest = hashlib.sha256(data).hexdigest()
    members = [(member, data), ("INSTALL.md", open(INSTALL, "rb").read()),
               ("SHA256SUMS", ("%s  %s\n" % (digest, member)).encode("ascii"))]
    for name in LICENCES:
        with open(os.path.join(ROOT, name), "rb") as f:
            members.append((name, f.read()))
    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, "neoj2k-viewer-%s-%s.zip" % (version(), platform))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, blob in members:
            info = zipfile.ZipInfo(name, FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, blob)
    with open(out, "rb") as f:
        zip_digest = hashlib.sha256(f.read()).hexdigest()
    print("package_openjp2: wrote %s (%d bytes)" % (os.path.relpath(out, ROOT), os.path.getsize(out)))
    print("  %s sha256 %s" % (member, digest))
    print("  zip sha256 %s" % zip_digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
