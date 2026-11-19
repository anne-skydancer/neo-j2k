<!-- SPDX-License-Identifier: BSD-2-Clause -->
<!-- Copyright (c) 2026, Anne Skydancer (neo-j2k) -->

# Departures from OpenJPEG

neo-j2k follows OpenJPEG 2.5.4 on every valid complete stream, and the standard where OpenJPEG is wrong
on cut or invalid input (plan T5). Each departure is listed here with the test that holds it. There is no
compatibility switch. Departures are numbered DP-n and the oracle's known defects OD-n (`doc/oracle.md`),
apart from the owner's decisions D1-D15 and the fork audit's findings O1-O3.

| # | Case | OpenJPEG 2.5.4 | neo-j2k | Test |
|---|---|---|---|---|
| DP-1 | A packet cut inside a code-block's contribution, with mode switches (BYPASS, TERMALL) | the shipped DLL drops the code-block and every later one in the packet, earlier layers included | keeps every codeword segment completed before the cut (stage 1a's truncation fix, `c51e47ed`, which the oracle carries). ISO 15444-1 B.11 only allows dropping whole packets; keeping completed segments goes beyond it on purpose (plan audit B5) | `oracle_cuts.*` (the oracle has the semantics); `oracle_layers` (cuts at layer boundaries equal the whole file limited to those layers) |
| DP-2 | The DC shift at the 2^31 edge | decided differently on Windows and Linux (`tcd.c` clamp) | the same on every platform | stage 1c (crafted streams) |
| DP-3 | A tile whose tile-parts hold no data bytes (packed headers, every packet empty) | refused (`opj_j2k_decode_tile`) | candidate: decode it (valid per the standard); decided in stage 1c | `corpus/generated/edge_ppt_empty_tiles_97.j2k` (the reference records OpenJPEG's refusal) |
| DP-4 | A cut packed-header (PPM, PPT) stream with EPH | refused ("Expected EPH marker": oracle defect OD-1) | the layers that arrived complete decode | `oracle_layers` lists the case as a known oracle defect; from stage 1c neo-j2k must match the whole file limited to those layers |
