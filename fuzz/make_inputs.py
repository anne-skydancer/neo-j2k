#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Fuzz seeds and hostile inputs, made from the generated corpus (deterministic).

usage: make_inputs.py --decode-seeds OUT [--corpus DIR]
           every corpus file with option bytes in front (fuzz/fuzz_common.h): the viewer's request
           at reduce 5 on its first 600 bytes and at reduce 2 on half the file, plain decodes at
           reduce 1-5, strict mode with one layer, a plain cut. Made at fuzz and test time.
       make_inputs.py --roundtrip-seeds OUT
           coding parameters and samples for the round-trip target (committed: fuzz/seeds/roundtrip)
       make_inputs.py --bombs OUT [--corpus DIR]
           resource bombs the fuzz filters must keep from the oracle (committed: fuzz/bombs); they
           are scanned by the filters' test, never decoded
       make_inputs.py --known-defects OUT [--corpus DIR]
           the inputs of the oracle's open resource defects (committed: tests/known-defects; OD-3
           and OD-5 in doc/oracle.md)

The corpus defaults to corpus/generated next to this script's folder.
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOC, SIZ, COD, COC, QCD, SOT, SOD, EOC = 0xFF4F, 0xFF51, 0xFF52, 0xFF53, 0xFF5C, 0xFF90, 0xFF93, 0xFFD9
PLAIN, STRICT, VIEWER = 0, 1, 2


def write(out, name, data):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name), "wb") as f:
        f.write(bytes(data))


def corpus_files(corpus):
    names = sorted(n for n in os.listdir(corpus) if n.endswith(".j2k"))
    for n in names:
        with open(os.path.join(corpus, n), "rb") as f:
            yield n, f.read()


def first_sot(cs):
    """Offset of the first SOT, following the main header's marker lengths."""
    pos = 2
    while pos + 4 <= len(cs):
        marker, length = struct.unpack_from(">HH", cs, pos)
        if marker == SOT:
            return pos
        pos += 2 + length
    raise ValueError("no SOT")


def segment(marker, body):
    return struct.pack(">HH", marker, 2 + len(body)) + bytes(body)


def cut_byte(size, keep):
    """The option byte that keeps at least `keep` of `size` bytes (fuzz_common.h: 1 + (size - 1) * n / 255)."""
    if size <= keep:
        return 255
    return min(254, -(-(keep - 1) * 255 // (size - 1)))


# ---- decode seeds ----------------------------------------------------------------------------------

def decode_seeds(corpus, out):
    count = 0
    for i, (name, cs) in enumerate(corpus_files(corpus)):
        stem = name[:-4]
        variants = {
            "viewer-r5-first600": (5, VIEWER, 0, cut_byte(len(cs), 600)),
            "viewer-r2-half": (2, VIEWER, 0, 128),
            "plain-r%d" % (1 + i % 5): (1 + i % 5, PLAIN, 0, 255),
            "strict-r0-layer1": (0, STRICT, 1, 255),
            "plain-r0-cut30": (0, PLAIN, 0, 77),
        }
        for tag, options in variants.items():
            write(out, "%s.%s.bin" % (stem, tag), bytes(options) + cs)
            count += 1
    return count


# ---- round-trip seeds --------------------------------------------------------------------------------

def roundtrip_seed(w, h, comps, prec, sgnd=False, numres=6, cbw=6, cbh=6, prog=0, mode=0, layers=1, sop=False,
                   eph=False, mct=False, seed=1, samples=192):
    """The 12 parameter bytes fuzz_roundtrip.cpp reads, then pseudo-random sample bytes."""
    p = bytes([w - 1, h - 1, comps - 1, prec - 1, 1 if sgnd else 0, numres - 1, (cbw - 2) + 5 * (cbh - 2), prog,
               mode, layers - 1, (1 if sop else 0) | (2 if eph else 0) | (4 if mct else 0), 0])
    x = seed * 2654435761 % (1 << 32) or 1
    data = bytearray()
    for _ in range(samples):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        data.append(x & 0xFF)
    return p + bytes(data)


ROUNDTRIP = [
    # (name, parameters): sizes, components, precisions, resolutions, code-blocks, the five
    # progressions, every mode switch, layers, SOP, EPH and the colour transform
    ("size_1x1_g8", dict(w=1, h=1, comps=1, prec=8, numres=1)),
    ("upload_32_rgb", dict(w=32, h=32, comps=3, prec=8, numres=6, prog=1, mct=True)),
    ("upload_64_rgba", dict(w=64, h=64, comps=4, prec=8, numres=6, prog=1, mct=True)),
    ("rpcl_48x40_rgba_sop_eph", dict(w=48, h=40, comps=4, prec=8, numres=5, cbw=5, cbh=5, prog=2, layers=3,
                                      sop=True, eph=True)),
    ("pcrl_17x37_rgb12", dict(w=17, h=37, comps=3, prec=12, numres=4, cbw=4, cbh=3, prog=3, layers=2, mct=True)),
    ("cprl_64x17_5c16s", dict(w=64, h=17, comps=5, prec=16, sgnd=True, numres=5, cbw=2, cbh=2, prog=4, layers=4)),
    ("lrcp_7x9_g1", dict(w=7, h=9, comps=1, prec=1, numres=2, cbw=2, cbh=6)),
    ("lrcp_3x5_2c10", dict(w=3, h=5, comps=2, prec=10, numres=2, cbw=2, cbh=2, layers=2)),
    ("mode_bypass", dict(w=33, h=31, comps=3, prec=8, numres=4, cbw=4, cbh=4, mode=1, layers=3, mct=True)),
    ("mode_reset", dict(w=33, h=31, comps=3, prec=8, numres=4, cbw=4, cbh=4, mode=2, layers=2)),
    ("mode_vsc", dict(w=33, h=31, comps=1, prec=8, numres=4, cbw=4, cbh=4, mode=8, layers=2)),
    ("mode_pterm", dict(w=33, h=31, comps=3, prec=8, numres=4, cbw=4, cbh=4, mode=16, layers=2, mct=True)),
    ("mode_segmark", dict(w=33, h=31, comps=1, prec=8, numres=4, cbw=4, cbh=4, mode=32, layers=2)),
    ("mode_all_but_termall_16", dict(w=40, h=40, comps=1, prec=16, numres=4, cbw=4, cbh=4, mode=59, layers=3)),
    ("mode_all_but_termall_rgb", dict(w=29, h=23, comps=3, prec=8, numres=3, cbw=3, cbh=3, mode=59, layers=4,
                                      sop=True, eph=True, mct=True)),
    ("layers4_48_rgb_sop_eph", dict(w=48, h=48, comps=3, prec=8, numres=5, layers=4, sop=True, eph=True, mct=True)),
    ("signed_40_g12", dict(w=40, h=40, comps=1, prec=12, sgnd=True, numres=4, cbw=3, cbh=5)),
    ("tall_1x64_g8", dict(w=1, h=64, comps=1, prec=8, numres=1, cbw=2, cbh=6)),
    ("wide_64x1_rgb8", dict(w=64, h=1, comps=3, prec=8, numres=1, cbw=6, cbh=2, mct=True)),
]


def roundtrip_seeds(out):
    for i, (name, params) in enumerate(ROUNDTRIP):
        write(out, "roundtrip-%s.bin" % name, roundtrip_seed(seed=i + 1, **params))
    return len(ROUNDTRIP)


# ---- bombs and known defects -------------------------------------------------------------------------

def set_psot(cs, psot):
    out = bytearray(cs)
    sot = first_sot(cs)
    struct.pack_into(">I", out, sot + 6, psot)
    return out


def fullres(w, h, comps, xcb, ycb, levels=7, tile_markers=b""):
    """A one-tile, one-layer 5/3 codestream of w x h whose code-blocks are 2^(xcb+2) x 2^(ycb+2), with 8
    data bytes: OpenJPEG builds the code-block structures of every resolution at full size."""
    siz = segment(SIZ, struct.pack(">HIIIIIIIIH", 0, w, h, 0, 0, w, h, 0, 0, comps) + bytes([7, 1, 1]) * comps)
    cod = segment(COD, bytes([0, 0, 0, 1, 0, levels, xcb, ycb, 0, 1]))
    qcd = segment(QCD, bytes([0x20]) + bytes([0x40] * (3 * levels + 1)))
    data = bytes(8)
    sot = struct.pack(">HHHIBB", SOT, 10, 0, 12 + len(tile_markers) + 2 + len(data), 0, 1)
    return struct.pack(">H", SOC) + siz + cod + qcd + sot + tile_markers + struct.pack(">H", SOD) + data + \
        struct.pack(">H", EOC)


def insert_in_tile_header(cs, markers):
    """Puts marker segments into the first tile-part header (after SOT) and lengthens its Psot."""
    sot = first_sot(cs)
    psot = struct.unpack_from(">I", cs, sot + 6)[0]
    out = bytearray(cs[:sot + 12]) + markers + cs[sot + 12:]
    struct.pack_into(">I", out, sot + 6, psot + len(markers))
    return out


def main_cod(cs):
    pos = 2
    while True:
        marker, length = struct.unpack_from(">HH", cs, pos)
        if marker == COD:
            return bytearray(cs[pos:pos + 2 + length])
        pos += 2 + length


def bombs(corpus, out):
    with open(os.path.join(corpus, "sl_32_rgb_97.j2k"), "rb") as f:
        sl32 = f.read()
    sot = first_sot(sl32)
    files = {}
    # OD-3: a Psot of 256 MiB, in plain sight
    files["decode-psot-256m.j2k"] = set_psot(sl32, 1 << 28)
    # the same tile-part hidden in an unknown main-header marker (FF70), whose length this filter's
    # reader would follow while OpenJPEG scans past it for the next marker it knows
    hidden = struct.pack(">HHHIBB", SOT, 10, 0, 1 << 28, 0, 1) + struct.pack(">H", SOD)
    files["decode-psot-hidden-unknown.j2k"] = sl32[:sot] + struct.pack(">HH", 0xFF70, 2 + len(hidden)) + hidden + \
        sl32[sot:]
    # ... and with a length that runs past the end, so the reader gives up on the main header
    files["decode-psot-hidden-unknown-long.j2k"] = sl32[:sot] + struct.pack(">HH", 0xFF70, 0xFFF0) + hidden + \
        sl32[sot:]
    # OD-5: full-resolution code-block structures behind a reduced request (option bytes: reduce,
    # mode, layers, cut)
    files["decode-fullres-cblk4-8192-r5.bin"] = bytes([5, PLAIN, 0, 255]) + fullres(8192, 8192, 1, 0, 0)
    files["decode-fullres-cblk4-16384-r5.bin"] = bytes([5, PLAIN, 0, 255]) + fullres(16384, 16384, 1, 0, 0)
    files["decode-fullres-cblk4-65536-r7.bin"] = bytes([7, VIEWER, 0, 255]) + fullres(65536, 65536, 1, 0, 0, levels=8)
    files["decode-fullres-cblk4-2048-rgba-r5.bin"] = bytes([5, VIEWER, 0, 255]) + fullres(2048, 2048, 4, 0, 0)
    # the same code-blocks from a COC in the tile-part header, the main COD's being 64 x 64
    coc = segment(COC, bytes([0, 0, 7, 0, 0, 0, 1]))
    files["decode-tile-coc-cblk4-8192-r5.bin"] = bytes([5, PLAIN, 0, 255]) + fullres(8192, 8192, 1, 4, 4,
                                                                                      tile_markers=coc)
    # 65,535 layers (CVE-2023-39327's packet walk) from a tile-part COD, and from a second main COD
    cod = main_cod(sl32)
    struct.pack_into(">H", cod, 6, 65535)
    files["decode-tile-cod-65535-layers.j2k"] = insert_in_tile_header(sl32, cod)
    first = sl32.index(bytes(main_cod(sl32)))
    end = first + len(main_cod(sl32))
    files["decode-main-cod-repeated-65535-layers.j2k"] = sl32[:end] + cod + sl32[end:]
    # fork audit S2: 65,535 tiles of 3 components (1 x 1 tiles of 255 x 257), for the header pass
    siz = segment(SIZ, struct.pack(">HIIIIIIIIH", 0, 255, 257, 0, 0, 1, 1, 0, 0, 3) + bytes([7, 1, 1]) * 3)
    s2 = struct.pack(">H", SOC) + siz + main_cod(sl32) + bytes(sl32[sl32.index(b"\xff\x5c"):sot]) + \
        struct.pack(">HHHIBBH", SOT, 10, 0, 14, 0, 1, SOD) + struct.pack(">H", EOC)
    files["header-siz-65535-tiles.j2k"] = s2
    # ... and the same SIZ behind an unknown marker, which OpenJPEG scans past to find it
    files["header-siz-behind-unknown.j2k"] = struct.pack(">H", SOC) + struct.pack(">HH", 0xFF30, 4) + bytes(2) + s2[2:]
    for name, data in files.items():
        write(out, name, data)
    return len(files)


def known_defects(corpus, out):
    with open(os.path.join(corpus, "sl_32_rgb_97.j2k"), "rb") as f:
        sl32 = f.read()
    # OD-3: the 782-byte texture with a declared tile-part length of 1 GiB
    write(out, "od3-psot-1g.j2k", set_psot(sl32, 1 << 30))
    # OD-5: a 119-byte 2048 x 2048 RGBA header with 4 x 4 code-blocks, decoded at reduce 5 (64 x 64)
    write(out, "od5-fullres-cblk4-2048-rgba.j2k", fullres(2048, 2048, 4, 0, 0))
    return 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=os.path.join(ROOT, "corpus", "generated"))
    ap.add_argument("--decode-seeds")
    ap.add_argument("--roundtrip-seeds")
    ap.add_argument("--bombs")
    ap.add_argument("--known-defects")
    a = ap.parse_args()
    if not (a.decode_seeds or a.roundtrip_seeds or a.bombs or a.known_defects):
        ap.error("nothing to make")
    if a.decode_seeds:
        print("make_inputs: %d decode seeds in %s" % (decode_seeds(a.corpus, a.decode_seeds), a.decode_seeds))
    if a.roundtrip_seeds:
        print("make_inputs: %d round-trip seeds in %s" % (roundtrip_seeds(a.roundtrip_seeds), a.roundtrip_seeds))
    if a.bombs:
        print("make_inputs: %d bombs in %s" % (bombs(a.corpus, a.bombs), a.bombs))
    if a.known_defects:
        print("make_inputs: %d known-defect inputs in %s" % (known_defects(a.corpus, a.known_defects),
                                                               a.known_defects))
    return 0


if __name__ == "__main__":
    sys.exit(main())
