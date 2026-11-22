#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Reads the ebc_replay outputs of one build (enc-<size>-<mode>.txt, dec-<size>-<mode>.txt) and prints the
study's tables in Markdown: the ratio of every timed variant to the base (f5 / wagg) and to the library.
usage: tables.py <dir> [enc|dec] [x-base|x-head]"""
import os
import re
import sys

d = sys.argv[1]
side = sys.argv[2] if len(sys.argv) > 2 else 'enc'
col = sys.argv[3] if len(sys.argv) > 3 else 'x-base'
cfgs = [(s, m) for s in (256, 512, 1024) for m in ('lossy', 'lossless')]
rows = {}
order = []
present = []
for s, m in cfgs:
    p = os.path.join(d, '%s-%d-%s.txt' % (side, s, m))
    if not os.path.exists(p):
        continue
    present.append((s, m))
    for line in open(p, encoding='utf-8', errors='replace'):
        mm = re.match(r'\s+(\S+)\s+([0-9.]+) Mcyc\s+x (?:head|lib) ([0-9.]+) \(([0-9.]+)-([0-9.]+)\)\s+x \S+ ([0-9.]+)', line)
        if not mm:
            continue
        name = mm.group(1)
        if name not in rows:
            rows[name] = {}
            order.append(name)
        rows[name][(s, m)] = (float(mm.group(3)), float(mm.group(6)))
hdr = '| Variant | ' + ' | '.join('%d² %s' % (s, 'y' if m == 'lossy' else 'l') for s, m in present) + ' |'
print(hdr)
print('|---|' + '---|' * len(present))
k = 1 if col == 'x-base' else 0
for n in order:
    cells = []
    for c in present:
        v = rows[n].get(c)
        cells.append('%.3f' % v[k] if v else '-')
    print('| %s | %s |' % (n, ' | '.join(cells)))
