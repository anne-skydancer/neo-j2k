#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Generate modules/openjp2/opj_forward.inc, the forwarders of the openjp2 module (the OpenJPEG adapter).

The module exports OpenJPEG's functions under OpenJPEG's names (opj_*), the 55 of the oracle's
openjpeg.h (OpenJPEG 2.5.4 plus the fork's decoder limits API). Each one that needs nothing of its own
calls its neo-j2k twin (nj2k_*) with the same arguments: neo-j2k's structures have OpenJPEG's layout
(`api_unit`), so the call is the translation. The functions in HAND are written by hand in
modules/openjp2/openjp2.cpp, where the adapter has something to add.

usage: gen_openjp2.py            write the file
       gen_openjp2.py --check    exit 1 if the committed file is stale, or if a function of the oracle's
                                 openjpeg.h is neither generated nor in HAND
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = os.path.join(ROOT, "include", "neoj2k", "neoj2k.h")
COMPAT = os.path.join(ROOT, "include", "neoj2k", "neoj2k_openjpeg.h")
OPENJPEG = os.path.join(ROOT, "oracle", "openjp2", "openjpeg.h")
OUT = os.path.join(ROOT, "modules", "openjp2", "opj_forward.inc")

# Written by hand in openjp2.cpp: the CPU check before a codec exists (the module takes the place of a
# library nobody asked about the CPU), the threads OpenJPEG starts and neo-j2k's core never does, the
# codec's end (it joins its threads), and the version string.
HAND = {
    "opj_create_decompress",
    "opj_create_compress",
    "opj_destroy_codec",
    "opj_codec_set_threads",
    "opj_has_thread_support",
    "opj_version",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def openjpeg_functions():
    with open(OPENJPEG, encoding="utf-8") as f:
        text = strip_comments(f.read())
    return sorted(set(re.findall(r"\bOPJ_API\b[^;{]*?\b(opj_\w+)\s*\(", text)))


def split_params(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [" ".join(p.split()) for p in out]


def prototypes():
    with open(API, encoding="utf-8") as f:
        text = strip_comments(f.read())
    found = {}
    for ret, name, args in re.findall(
            r"NJ2K_API\s+([^;{]*?)\s*NJ2K_CALLCONV\s+(nj2k_\w+)\s*\(([^;{]*?)\)\s*;", text, flags=re.S):
        ret = " ".join(ret.split())
        params = split_params(args)
        if params == ["void"]:
            params = []
        names = []
        for p in params:
            m = re.search(r"(\w+)\s*(\[\s*\w*\s*\])?$", p)
            if not m or p == m.group(1):
                raise SystemExit("gen_openjp2: no parameter name in '%s' of %s" % (p, name))
            names.append(m.group(1))
        found[name] = (ret, params, names)
    return found


def type_of(param):
    """A parameter without its name: 'const char * const * p_options' -> 'const char * const *'."""
    m = re.search(r"(\w+)\s*(\[\s*\w*\s*\])?$", param)
    base = param[:m.start(1)] if m and param != m.group(1) else param
    return " ".join((base + ((m.group(2) or "") if m else "")).split())


def squeeze(text):
    return re.sub(r"\s+", "", text)


def to_neo(text):
    return re.sub(r"\bOPJ_", "NJ2K_", re.sub(r"\bopj_", "nj2k_", text))


def openjpeg_prototypes():
    with open(OPENJPEG, encoding="utf-8") as f:
        text = strip_comments(f.read())
    found = {}
    for ret, name, args in re.findall(
            r"\bOPJ_API\s+([^;{]*?)\s*OPJ_CALLCONV\s+(opj_\w+)\s*\(([^;{]*?)\)\s*;", text, flags=re.S):
        params = split_params(args)
        if params == ["void"]:
            params = []
        found[name] = (" ".join(ret.split()), [type_of(p) for p in params])
    return found


def check_abi(funcs, protos):
    """Every function: return and parameter types of openjpeg.h equal neoj2k.h's, names mapped."""
    oracle = openjpeg_prototypes()
    problems = []
    for f in funcs:
        twin = "nj2k_" + f[len("opj_"):]
        if f not in oracle:
            problems.append("%s: prototype not read from openjpeg.h" % f)
            continue
        oret, oparams = oracle[f]
        ret, params, _ = protos[twin]
        nret, nparams = " ".join(ret.split()), [type_of(p) for p in params]
        if (squeeze(to_neo(oret)) != squeeze(nret)
                or [squeeze(to_neo(p)) for p in oparams] != [squeeze(p) for p in nparams]):
            problems.append("%s: openjpeg.h says %s(%s), neoj2k.h says %s(%s)"
                            % (f, oret, ",".join(oparams), nret, ",".join(nparams)))
    if problems:
        raise SystemExit("gen_openjp2: the ABI differs:\n  " + "\n  ".join(problems))


def api_level():
    with open(COMPAT, encoding="utf-8") as f:
        text = f.read()
    v = [re.search(r"#define OPJ_VERSION_%s (\d+)" % k, text).group(1) for k in ("MAJOR", "MINOR", "BUILD")]
    return ".".join(v)


def render():
    funcs = openjpeg_functions()
    protos = prototypes()
    missing = [f for f in sorted(HAND) if f not in funcs]
    if missing:
        raise SystemExit("gen_openjp2: HAND names that openjpeg.h does not declare: %s" % ", ".join(missing))
    for f in funcs:
        if "nj2k_" + f[len("opj_"):] not in protos:
            raise SystemExit("gen_openjp2: %s has no neo-j2k twin (add it to neoj2k.h)" % f)
    check_abi(funcs, protos)
    lines = [
        "/* SPDX-License-Identifier: BSD-2-Clause",
        "   Copyright (c) 2026, Anne Skydancer (neo-j2k)",
        "",
        "   The openjp2 module's forwarders: OpenJPEG's names over neo-j2k's functions. Generated by",
        "   tools/gen_openjp2.py from include/neoj2k/neoj2k.h and the oracle's openjpeg.h; do not edit.",
        "   CI checks that it is current. The functions in HAND (the generator) are in openjp2.cpp. */",
        "",
        '#define NJ2K_OPENJPEG_API_LEVEL "%s"' % api_level(),
        "",
    ]
    lines.append("/* the functions of openjp2.cpp, declared here so that a signature that differs is an error */")
    for f in sorted(HAND):
        ret, params, _ = protos["nj2k_" + f[len("opj_"):]]
        lines.append("OPJ_EXPORT %s %s(%s);" % (ret, f, ", ".join(params) if params else "void"))
    lines.append("")
    count = 0
    for f in funcs:
        if f in HAND:
            continue
        twin = "nj2k_" + f[len("opj_"):]
        if twin not in protos:
            raise SystemExit("gen_openjp2: %s has no neo-j2k twin %s (add it to HAND or to neoj2k.h)" % (f, twin))
        ret, params, names = protos[twin]
        call = "%s(%s)" % (twin, ", ".join(names))
        body = call if ret == "void" else "return " + call
        lines.append("OPJ_EXPORT %s %s(%s)" % (ret, f, ", ".join(params) if params else "void"))
        lines.append("{")
        lines.append("    %s;" % body)
        lines.append("}")
        lines.append("")
        count += 1
    lines.append("/* %d forwarders, %d functions by hand: the %d of openjpeg.h */" % (count, len(HAND), len(funcs)))
    return "\n".join(lines) + "\n"


def main():
    text = render()
    if "--check" in sys.argv:
        try:
            with open(OUT, encoding="utf-8", newline="") as f:
                current = f.read()
        except OSError:
            current = None
        if current != text:
            print("gen_openjp2: %s is stale; run tools/gen_openjp2.py" % os.path.relpath(OUT, ROOT))
            return 1
        print("gen_openjp2: opj_forward.inc is current (%d functions of openjpeg.h)" % len(openjpeg_functions()))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("gen_openjp2: wrote %s" % os.path.relpath(OUT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
