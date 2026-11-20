// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The decode of a codestream after its main header, after OpenJPEG 2.5.4's j2k.c at the oracle's
// pin: opj_j2k_decode_tiles, opj_j2k_read_tile_header, opj_j2k_read_sot,
// opj_j2k_need_nb_tile_parts_correction, opj_j2k_read_sod, opj_j2k_find_final_eoc,
// opj_j2k_decode_tile, opj_j2k_update_image_data and opj_j2k_are_all_used_components_decoded, with
// the same states, checks and outcomes, over the codestream's bytes in memory (a stream's bytes
// are read into memory first, and the stream's offsets, length and seek function are kept, so that
// every cut and every short stream ends as in OpenJPEG). The tile-part markers are read by the
// header model (header.h), with its departures (doc/departures.md). Departure DP-3: a tile whose
// tile-parts arrived without data bytes, its packet headers in PPM or PPT, decodes as the standard
// says (OpenJPEG refuses it); a tile that never arrived, or one with neither data nor packed packet
// headers, is still not decoded. OpenJPEG's notice:
//
/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2014, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux
 * Copyright (c) 2003-2014, Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * Copyright (c) 2010-2011, Kaori Hagihara
 * Copyright (c) 2011-2012, Centre National d'Etudes Spatiales (CNES), France
 * Copyright (c) 2012, CS Systemes d'Information, France
 * Copyright (c) 2017, IntoPIX SA <support@intopix.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "decode.h"

#include <climits>
#include <cstring>

#include "codec.h"
#include "output.h"
#include "parallel.h"
#include "tile.h"

namespace nj2k::detail {

namespace {

// OpenJPEG's decoder states (j2k.h)
constexpr std::uint32_t kTphsot = 0x0008, kTph = 0x0010, kNeoc = 0x0040, kData = 0x0080, kEoc = 0x0100, kErr = 0x8000;
constexpr std::size_t kEnd = SIZE_MAX;  // opj_stream_read_data's (OPJ_SIZE_T)-1

inline std::uint32_t be16(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 8) | p[1];
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

struct Range {
    std::uint64_t pos;
    std::uint32_t len;
};

// Whether the caller's planes can take rows from several threads at once (stage 5): no byte is in
// two samples, that is a plane's rows lie apart (a row's samples within its row stride), and two
// planes either lie apart or are interleaved in common rows (the same strides, their samples apart
// within a row, and both planes' rows within one row stride). Then bands of rows, blocks and stripes
// never write a byte another part writes. One thread writes planes that share bytes in its fixed
// order (the last write wins); parts would race over them, so such a decode keeps to one thread.
// (nj2k_decode_memory checks only that a plane's samples fit their sample stride: with one thread,
// shared bytes were harmless.)
bool planes_apart(const nj2k_output_t& o, const OutComp* comps) noexcept
{
    constexpr std::uint32_t kMaxChecked = 32;  // more planes than that: one thread
    if (o.numplanes > kMaxChecked) {
        return false;
    }
    const std::uint64_t ss = o.sample_type == NJ2K_SAMPLE_U8 ? 1 : o.sample_type == NJ2K_SAMPLE_U16 ? 2 : 4;
    struct Span {
        std::uint64_t lo, hi, row0, span, ars;
        bool used;
    };
    Span sp[kMaxChecked];
    for (std::uint32_t k = 0; k < o.numplanes; ++k) {
        const nj2k_plane_t& pl = o.planes[k];
        const OutComp& c = comps[pl.component];
        sp[k].used = c.w != 0 && c.h != 0;
        if (!sp[k].used) {
            continue;
        }
        const std::uint64_t ars = pl.row_stride < 0 ? 0 - std::uint64_t(pl.row_stride) : std::uint64_t(pl.row_stride);
        const std::uint64_t span = std::uint64_t(c.w - 1) * pl.sample_stride + ss;  // (2^32 - 1)^2 + 4 at most: fits
        if ((c.h > 1 && span > ars) || (c.h > 1 && ars > (UINT64_MAX >> 2) / (c.h - 1)) || span > (UINT64_MAX >> 2)) {
            return false;
        }
        const std::uint64_t down = std::uint64_t(c.h - 1) * ars;
        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(pl.data);
        const std::uint64_t lo = pl.row_stride < 0 ? base - down : base;
        if (lo > base || lo > UINT64_MAX - down - span) {
            return false;
        }
        sp[k] = Span{lo, lo + down + span, base, span, ars, true};
    }
    for (std::uint32_t p = 0; p < o.numplanes; ++p) {
        for (std::uint32_t q = p + 1; q < o.numplanes; ++q) {
            const Span& a = sp[p];
            const Span& b = sp[q];
            if (!a.used || !b.used || a.hi <= b.lo || b.hi <= a.lo) {
                continue;
            }
            const nj2k_plane_t& pa = o.planes[p];
            const nj2k_plane_t& pb = o.planes[q];
            if (pa.row_stride != pb.row_stride || pa.sample_stride != pb.sample_stride) {
                return false;
            }
            const std::uint64_t first = a.row0 < b.row0 ? a.row0 : b.row0;
            const std::uint64_t end = a.row0 + a.span > b.row0 + b.span ? a.row0 + a.span : b.row0 + b.span;
            const std::uint64_t off = ((a.row0 > b.row0 ? a.row0 - b.row0 : b.row0 - a.row0)) % pa.sample_stride;
            if ((sp[p].ars && end - first > a.ars) || off < ss || off > pa.sample_stride - ss) {
                return false;
            }
        }
    }
    return true;
}


// A tile's walk state (OpenJPEG keeps it in its opj_tcp_t), made when the tile first appears.
struct TileWalk {
    std::int32_t current_tile_part_number;  // -1 before any
    std::uint32_t nb_tile_parts;
    bool seen;      // a tile-part reached its SOD since the tile was last decoded (DP-3)
    bool has_data;  // OpenJPEG's m_data != NULL: a tile-part gave it bytes
    bool cut;       // the bytes ended before a tile-part's declared length
    bool ppt_merged;
    Range* ranges;
    std::uint32_t nranges, ranges_cap;
};

struct Walk {
    Decoder& d;
    MainHeader& mh;
    const CodestreamBytes& b;
    OutComp* comps;
    const OutSink& sink;
    const Messages& msg;
    HeaderSettings settings{};
    std::uint64_t pos = 0;
    std::uint32_t state = kTphsot;
    bool can_decode = false;
    bool last_tile_part = false;
    bool correction_checked = false;
    std::uint32_t correction = 0;
    std::uint32_t sot_length = 0;
    std::uint32_t current_tile = 0;
    bool truncated = false;
    static constexpr std::uint32_t kPage = 128;
    TileWalk** pages = nullptr;  // pages of kPage tiles, made as tiles appear
    std::uint32_t page_count = 0;
    PackedCursor ppm{nullptr, 0};
    std::uint64_t image_need = UINT64_MAX;  // image_working_memory, once a tile needs it
    bool direct_tile = false;                // the tile decodes into the caller's planes (direct_output)
    // threads in one image (stage 5): the host's executor, when the codec has one and the caller's
    // planes share no byte (planes_apart); the current tile's stages run through it when the tile is
    // large enough (tile_exec, null otherwise)
    Jobs jobs{};
    bool have_jobs = false;
    const Jobs* tile_exec = nullptr;

    Walk(Decoder& dec, const CodestreamBytes& bytes, OutComp* c, const OutSink& s) noexcept
        : d(dec), mh(*dec.header), b(bytes), comps(c), sink(s), msg(dec.messages)
    {
        have_jobs = d.jobs(jobs) && (sink.image_planes || planes_apart(*sink.output, comps));
    }

    // The current tile through the executor (stage 5), when the host lent one (it does that only when
    // it has idle cores) and the tile's largest component has parallel_tile samples or more: the
    // code-blocks' costs and Tier-1's runs (plan_t1), a Tier-1 scratch a run, and the wavelet's larger
    // scratch. When any of that cannot be had, what was had for it is given back and the tile runs on this thread, which decodes it the same.
    // Its working memory is outside the decoder limits' estimate (so that the limits refuse the same
    // images with or without an executor): per run a Tier-1 scratch (28 KiB) with room for the
    // largest code-block of its run (over all runs at most the tile's coded bytes, which the decoder
    // holds already), per part a wavelet line, and 8 bytes per code-block (its estimated cost).
    bool threaded_tile(Tile& t, TileJobs& tj) noexcept
    {
        if (!have_jobs || t.largest_component_samples() < d.parallel_tile) {
            return false;
        }
        bool ok = d.blocks.grow(d.allocator, t.cblk_costs_bytes());
        if (ok) {
            t.plan_t1(tj, 2 * jobs.width, d.blocks.p);  // more runs than threads: a slow run is not the tail
            ok = d.t1_parts.ensure(d.allocator, tj.runs);
        }
        for (std::uint32_t p = 0; ok && p < tj.runs; ++p) {
            ok = d.t1_parts.t1[p].reserve(d.allocator, tj.run_bytes[p]);
        }
        const std::size_t one = t.dwt_scratch_bytes_needed();
        if (ok && !d.dwt.grow(d.allocator, t.dwt_scratch_bytes_needed(jobs.width))) {
            ok = false;
        }
        if (!ok) {
            // this thread's decode then has the room it would have had without an executor
            d.t1_parts.release(d.allocator);
            d.blocks.release(d.allocator);
            if (d.dwt.size > one) {
                d.dwt.release(d.allocator);
            }
            return false;
        }
        tj.jobs = &jobs;
        tj.t1 = d.t1_parts.t1;
        return true;
    }

    // ---- the stream, over the bytes in memory -------------------------------------------------
    std::int64_t bytes_left() const noexcept
    {
        // opj_stream_get_number_byte_left: 0 when the host gave no length
        return b.claimed_end ? static_cast<std::int64_t>(b.claimed_end) - static_cast<std::int64_t>(pos) : 0;
    }
    // opj_stream_read_data: n, fewer at the end, or kEnd when nothing is left
    std::size_t read(std::uint8_t* dst, std::size_t n) noexcept
    {
        if (n == 0) {
            return 0;
        }
        const std::uint64_t avail = b.actual_end > pos ? b.actual_end - pos : 0;
        if (avail == 0) {
            return kEnd;
        }
        const std::size_t take = n < avail ? n : static_cast<std::size_t>(avail);
        if (dst) {
            std::memcpy(dst, b.base + (pos - b.origin), take);
        }
        pos += take;
        return take;
    }
    bool seek(std::uint64_t to) noexcept
    {
        if (!b.has_seek || to < b.origin || to > b.actual_end) {
            return false;
        }
        pos = to;
        return true;
    }
    // opj_stream_read_skip: n, fewer at the end, or -1 when nothing could be skipped
    std::int64_t skip(std::uint64_t n) noexcept
    {
        const std::uint64_t avail = b.actual_end > pos ? b.actual_end - pos : 0;
        if (n <= avail) {
            pos += n;
            return static_cast<std::int64_t>(n);
        }
        pos += avail;
        return avail ? static_cast<std::int64_t>(avail) : -1;
    }

    // The tiles of page p: 128, fewer in the last page (a single tile's page holds one, so a small
    // image's decode clears no more than it uses).
    std::uint32_t page_tiles(std::uint32_t p) const noexcept
    {
        const std::uint32_t left = mh.tile_count() - p * kPage;
        return left < kPage ? left : kPage;
    }
    // the walk state of a tile (tile < mh.tile_count()), made when it first appears
    TileWalk* tile_walk(std::uint32_t tile) noexcept
    {
        if (!pages) {
            page_count = (mh.tile_count() + kPage - 1) / kPage;
            pages = d.work.make_array<TileWalk*>(page_count);
            if (!pages) {
                return nullptr;
            }
        }
        TileWalk*& page = pages[tile / kPage];
        if (!page) {
            const std::uint32_t n = page_tiles(tile / kPage);
            page = d.work.make_array<TileWalk>(n);
            if (!page) {
                return nullptr;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                page[i].current_tile_part_number = -1;
            }
        }
        return &page[tile % kPage];
    }
    // the walk state of a tile, or null when it never appeared
    TileWalk* find_walk(std::uint32_t tile) const noexcept
    {
        if (!pages) {
            return nullptr;
        }
        TileWalk* page = pages[tile / kPage];
        return page ? &page[tile % kPage] : nullptr;
    }

    // Whether a tile is decoded: OpenJPEG decodes a tile whose tile-parts gave it data bytes. DP-3: a
    // tile whose tile-parts arrived whole without data bytes, its packet headers packed in PPM or PPT,
    // decodes too, as the standard has it; without packed headers its packets are missing, and it is
    // refused as in OpenJPEG.
    bool decodable(std::uint32_t tile, const TileWalk* tw) const noexcept
    {
        if (!tw || !tw->seen) {
            return false;
        }
        if (tw->has_data || mh.has_ppm) {
            return true;
        }
        const TileOverrides* ov = mh.overrides(tile);
        return ov && ov->ppt.size != 0;
    }

    nj2k_status_t nomem() noexcept
    {
        msg.emit(Messages::Error, "Not enough memory to decode\n");
        return d.work.budget_failed() ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
    }

    // ---- opj_j2k_read_sot ------------------------------------------------------------------------
    nj2k_status_t read_sot(const std::uint8_t* p, std::uint32_t n) noexcept
    {
        if (n != 8) {
            // twice, as OpenJPEG gives it: opj_j2k_get_sot_values, then opj_j2k_read_sot
            msg.emit(Messages::Error, "Error reading SOT marker\n");
            msg.emit(Messages::Error, "Error reading SOT marker\n");
            return NJ2K_E_HEADER;
        }
        current_tile = be16(p);
        const std::uint32_t tot_len = be32(p + 2);
        const std::uint32_t current_part = p[6];
        std::uint32_t num_parts = p[7];
        if (current_tile >= mh.tile_count()) {
            msg.emit(Messages::Error, "Invalid tile number %u\n", current_tile);
            return NJ2K_E_HEADER;
        }
        TileWalk* tw = tile_walk(current_tile);
        if (!tw) {
            return nomem();
        }
        // ISO 15444-1 A.4.2: tile-parts in increasing order
        if (tw->current_tile_part_number + 1 != static_cast<std::int32_t>(current_part)) {
            msg.emit(Messages::Error, "Invalid tile part index for tile number %u. Got %u, expected %d\n",
                     current_tile, current_part, tw->current_tile_part_number + 1);
            return NJ2K_E_HEADER;
        }
        tw->current_tile_part_number = static_cast<std::int32_t>(current_part);
        if ((tot_len != 0) && (tot_len < 14)) {
            if (tot_len == 12) {
                msg.emit(Messages::Warning, "Empty SOT marker detected: Psot=%u.\n", tot_len);
            } else {
                msg.emit(Messages::Error, "Psot value is not correct regards to the JPEG2000 norm: %u.\n", tot_len);
                return NJ2K_E_HEADER;
            }
        }
        if (!tot_len) {
            last_tile_part = true;
        }
        if (tw->nb_tile_parts != 0 && current_part >= tw->nb_tile_parts) {
            msg.emit(Messages::Error,
                     "In SOT marker, TPSot (%u) is not valid regards to the previous number of tile-part (%u), "
                     "giving up\n",
                     current_part, tw->nb_tile_parts);
            last_tile_part = true;
            return NJ2K_E_HEADER;
        }
        if (num_parts != 0) {
            num_parts += correction;
            if (tw->nb_tile_parts && current_part >= tw->nb_tile_parts) {
                msg.emit(Messages::Error,
                         "In SOT marker, TPSot (%u) is not valid regards to the current number of tile-part (%u), "
                         "giving up\n",
                         current_part, tw->nb_tile_parts);
                last_tile_part = true;
                return NJ2K_E_HEADER;
            }
            if (current_part >= num_parts) {
                msg.emit(Messages::Error,
                         "In SOT marker, TPSot (%u) is not valid regards to the current number of tile-part "
                         "(header) (%u), giving up\n",
                         current_part, num_parts);
                last_tile_part = true;
                return NJ2K_E_HEADER;
            }
            tw->nb_tile_parts = num_parts;
        }
        if (tw->nb_tile_parts) {
            if (tw->nb_tile_parts == (current_part + 1)) {
                can_decode = true;
            }
        }
        sot_length = !last_tile_part ? tot_len - 12 : 0;
        state = kTph;
        return NJ2K_OK;
    }

    // opj_j2k_find_final_eoc: whether the codestream's last two bytes are EOC
    bool find_final_eoc(std::int64_t left, bool& ends_with_eoc) noexcept
    {
        ends_with_eoc = true;
        if (left < 2) {
            ends_with_eoc = false;
            return true;
        }
        if (!b.has_seek) {
            return true;
        }
        const std::uint64_t at = pos;
        std::uint8_t two[2] = {};
        if (seek(at + static_cast<std::uint64_t>(left) - 2) && read(two, 2) == 2) {
            ends_with_eoc = be16(two) == mk::EOC;
        }
        if (!seek(at)) {
            msg.emit(Messages::Error, "Problem with seek function\n");
            return false;
        }
        return true;
    }

    // OpenJPEG refuses a reduce factor at or above the resolutions of a COD or COC it reads, tile-part
    // headers included (opj_j2k_read_SPCod_SPCoc); neo-j2k checks each tile-component after marker
    // precedence (DP-5), when a tile-part header that changes the coding style is complete. Under
    // either policy: NJ2K_REDUCE_CLAMP lowers the factor to the main header's coarsest level, and a
    // tile whose own coding gives a component fewer resolutions than that has no level of the
    // image's size.
    nj2k_status_t check_tile_reduce() noexcept
    {
        if (d.factor == 0) {
            return NJ2K_OK;
        }
        const TileOverrides* ov = mh.overrides(current_tile);
        if (!ov || (!ov->has_cod && ov->coc.size == 0)) {
            return NJ2K_OK;
        }
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            TileComponentParams p{};
            if (const nj2k_status_t st = resolve_tile_component(mh, ov, c, msg, p); st != NJ2K_OK) {
                return st;
            }
            if (d.factor >= p.style->numresolutions) {
                msg.emit(Messages::Error,
                         "Error decoding component %u.\nThe number of resolutions to remove (%u) is greater or equal "
                         "than the number of resolutions of this component (%u)\nModify the cp_reduce parameter.\n\n",
                         c, d.factor, unsigned(p.style->numresolutions));
                return NJ2K_E_LIMIT;
            }
        }
        return NJ2K_OK;
    }

    // ---- opj_j2k_read_sod (the tile-part's data: where it lies; tile_bytes() reads it there) ----
    nj2k_status_t read_sod() noexcept
    {
        TileWalk* tw = find_walk(current_tile);
        // the tile-part's data length comes from what arrived, not from its header (a cut Psot = 0
        // tile-part): no data then means the bytes ended at its SOD, not an empty tile-part (DP-3)
        bool length_from_cut = false;
        if (last_tile_part) {
            const std::int64_t left = bytes_left();
            bool ends_with_eoc = true;
            if (!d.strict && !find_final_eoc(left, ends_with_eoc)) {
                return NJ2K_E_DATA;
            }
            if (!ends_with_eoc) {
                // Psot = 0 runs the tile-part to the EOC marker, but a cut codestream has none
                sot_length = static_cast<std::uint32_t>(left);
                truncated = true;
                length_from_cut = true;
            } else {
                sot_length = static_cast<std::uint32_t>(left - 2);
            }
        } else if (sot_length >= 2) {
            sot_length -= 2;
        }
        bool pb_detected = false;
        if (sot_length) {
            if (static_cast<std::int64_t>(sot_length) > bytes_left()) {
                if (d.strict) {
                    msg.emit(Messages::Error, "Tile part length size inconsistent with stream length\n");
                    return NJ2K_E_DATA;
                }
                msg.emit(Messages::Warning, "Tile part length size inconsistent with stream length\n");
            }
            if (sot_length > UINT_MAX - 2) {
                msg.emit(Messages::Error,
                         "p_j2k->m_specific_param.m_decoder.m_sot_length > UINT_MAX - OPJ_COMMON_CBLK_DATA_EXTRA");
                return NJ2K_E_DATA;
            }
            // OpenJPEG allocates sot_length bytes here, before reading them (OD-3); neo-j2k records
            // where the bytes are
        } else {
            pb_detected = true;
        }
        std::size_t read_size = 0;
        const std::uint64_t at = pos;
        if (!pb_detected) {
            read_size = read(nullptr, sot_length);
        }
        if (!pb_detected || !length_from_cut) {
            // the tile-part arrived (DP-3: one that declares no data bytes counts, one whose bytes
            // ended at its SOD does not; decodable() decides)
            tw->seen = true;
        }
        if (read_size != sot_length) {
            if (read_size == kEnd) {
                msg.emit(Messages::Error, "Stream too short\n");
                return NJ2K_E_NEED_DATA;
            }
            state = kNeoc;
            truncated = true;
            tw->cut = true;
        } else {
            state = kTphsot;
        }
        if (length_from_cut) {
            tw->cut = true;
        }
        if (!pb_detected) {
            tw->has_data = true;
            if (read_size) {
                if (!grow(d.work, tw->ranges, tw->nranges, tw->ranges_cap, tw->nranges + 1)) {
                    return nomem();
                }
                tw->ranges[tw->nranges++] = Range{at, static_cast<std::uint32_t>(read_size)};
            }
        }
        return NJ2K_OK;
    }

    // opj_j2k_need_nb_tile_parts_correction (OpenJPEG issue 254: TPsot == TNsot)
    nj2k_status_t need_correction(std::uint32_t tile_no, bool& needed) noexcept
    {
        needed = false;
        if (!b.has_seek) {
            return NJ2K_OK;
        }
        const std::uint64_t backup = pos;
        std::uint32_t l_tile_no = 0, l_tot_len = 0, l_current_part = 0, l_num_parts = 0;
        for (;;) {
            std::uint8_t h[10] = {};
            if (read(h, 2) != 2) {
                if (!seek(backup)) {
                    return NJ2K_E_DATA;
                }
                return NJ2K_OK;
            }
            if (be16(h) != mk::SOT) {
                if (!seek(backup)) {
                    return NJ2K_E_DATA;
                }
                return NJ2K_OK;
            }
            if (read(h, 2) != 2) {
                msg.emit(Messages::Error, "Stream too short\n");
                return NJ2K_E_NEED_DATA;
            }
            if (be16(h) != 10) {
                msg.emit(Messages::Error, "Inconsistent marker size\n");
                return NJ2K_E_HEADER;
            }
            if (read(h, 8) != 8) {
                msg.emit(Messages::Error, "Stream too short\n");
                return NJ2K_E_NEED_DATA;
            }
            l_tile_no = be16(h);
            l_tot_len = be32(h + 2);
            l_current_part = h[6];
            l_num_parts = h[7];
            if (l_tile_no == tile_no) {
                break;
            }
            if (l_tot_len < 14U) {
                if (!seek(backup)) {
                    return NJ2K_E_DATA;
                }
                return NJ2K_OK;
            }
            l_tot_len -= 12U;
            if (skip(l_tot_len) != static_cast<std::int64_t>(l_tot_len)) {
                if (!seek(backup)) {
                    return NJ2K_E_DATA;
                }
                return NJ2K_OK;
            }
        }
        if (l_current_part == l_num_parts) {
            needed = true;
        }
        if (!seek(backup)) {
            return NJ2K_E_DATA;
        }
        return NJ2K_OK;
    }

    // ---- opj_j2k_read_tile_header ----------------------------------------------------------------
    nj2k_status_t read_tile_header(bool& go_on) noexcept
    {
        std::uint32_t current_marker = mk::SOT;
        std::uint8_t h[2] = {};
        if (state == kEoc) {
            current_marker = mk::EOC;
        } else if (state != kTphsot) {
            msg.emit(Messages::Error, "The codestream ended before its next tile-part\n");
            return NJ2K_E_DATA;
        }
        while (!can_decode && current_marker != mk::EOC) {
            while (current_marker != mk::SOD) {
                if (bytes_left() == 0) {
                    state = kNeoc;
                    break;
                }
                if ((state & kTph) && segmentless(current_marker)) {
                    if (current_marker < 0xFF30 || current_marker > 0xFF3F) {
                        // SOC, EOC or EPH in a tile-part header (SOD ends it): not a marker of a
                        // tile-part header, and no segment to read (OpenJPEG fails on it too)
                        msg.emit(Messages::Error, "Marker is not compliant with its position\n");
                        return NJ2K_E_HEADER;
                    }
                    // a marker without a segment: skipped (DP-7), the length it leaves counted. Where
                    // a SOT is expected (between tile-parts) it is read as OpenJPEG reads it, below,
                    // and refused for its position
                    if (sot_length != 0) {
                        if (sot_length < 2) {
                            msg.emit(Messages::Error, "Sot length is less than marker size + marker ID\n");
                            return NJ2K_E_HEADER;
                        }
                        sot_length -= 2;
                    }
                    msg.emit(Messages::Warning, "Marker 0x%04X without a segment skipped in a tile-part header\n",
                             current_marker);
                    if (read(h, 2) != 2) {
                        msg.emit(Messages::Error, "Stream too short\n");
                        return NJ2K_E_NEED_DATA;
                    }
                    current_marker = be16(h);
                    continue;
                }
                if (read(h, 2) != 2) {
                    msg.emit(Messages::Error, "Stream too short\n");
                    return NJ2K_E_NEED_DATA;
                }
                std::uint32_t marker_size = be16(h);
                if (marker_size < 2) {
                    msg.emit(Messages::Error, "Inconsistent marker size\n");
                    return NJ2K_E_HEADER;
                }
                // OpenJPEG issue 226
                if (current_marker == 0x8080 && bytes_left() == 0) {
                    state = kNeoc;
                    break;
                }
                if ((state & kTph) && sot_length != 0) {
                    if (sot_length < marker_size + 2) {
                        msg.emit(Messages::Error, "Sot length is less than marker size + marker ID\n");
                        return NJ2K_E_HEADER;
                    }
                    sot_length -= (marker_size + 2);
                }
                marker_size -= 2;
                // where the marker may be
                const bool is_sot = current_marker == mk::SOT;
                if (current_marker < 0xFF00) {
                    msg.emit(Messages::Error, "A marker ID was expected (0xff--) instead of %.8x\n", current_marker);
                    return NJ2K_E_HEADER;
                }
                if ((state & kTphsot) && !is_sot) {
                    msg.emit(Messages::Error, "Marker is not compliant with its position\n");
                    return NJ2K_E_HEADER;
                }
                if ((state & kTph) && is_sot) {
                    msg.emit(Messages::Error, "Marker is not compliant with its position\n");
                    return NJ2K_E_HEADER;
                }
                if (!is_sot && current_marker >= 0xFF70 && current_marker <= 0xFF7F) {
                    msg.emit(Messages::Error, "Marker 0x%04X is a JPEG 2000 Part 2 extension, not supported\n",
                             current_marker);
                    return NJ2K_E_UNSUPPORTED;
                }
                if (static_cast<std::int64_t>(marker_size) > bytes_left()) {
                    msg.emit(Messages::Error, "Marker size inconsistent with stream length\n");
                    return NJ2K_E_NEED_DATA;
                }
                const std::uint8_t* body = nullptr;
                std::uint8_t sot_body[8] = {};
                if (is_sot) {
                    if (marker_size > sizeof sot_body) {
                        // OpenJPEG reads the segment, then refuses its size
                        if (read(nullptr, marker_size) != marker_size) {
                            msg.emit(Messages::Error, "Stream too short\n");
                            return NJ2K_E_NEED_DATA;
                        }
                        msg.emit(Messages::Error, "Error reading SOT marker\n");
                        return NJ2K_E_HEADER;
                    }
                    if (marker_size && read(sot_body, marker_size) != marker_size) {
                        msg.emit(Messages::Error, "Stream too short\n");
                        return NJ2K_E_NEED_DATA;
                    }
                    if (const nj2k_status_t st = read_sot(sot_body, marker_size); st != NJ2K_OK) {
                        msg.emit(Messages::Error, "Fail to read the current marker segment (%#x)\n", current_marker);
                        return st;
                    }
                } else {
                    std::uint8_t* copy = nullptr;
                    if (marker_size) {
                        copy = static_cast<std::uint8_t*>(d.tile_arena.allocate(marker_size, 1));
                        if (!copy) {
                            return d.tile_arena.failure() == Arena::Failure::budget ? NJ2K_E_LIMIT : NJ2K_E_NO_MEMORY;
                        }
                        if (read(copy, marker_size) != marker_size) {
                            msg.emit(Messages::Error, "Stream too short\n");
                            return NJ2K_E_NEED_DATA;
                        }
                    }
                    body = copy;
                    if (const nj2k_status_t st =
                            read_tile_part_marker(d.tile_arena, msg, settings, mh, static_cast<std::uint16_t>(current_tile),
                                                  static_cast<std::uint16_t>(current_marker), body, marker_size);
                        st != NJ2K_OK) {
                        msg.emit(Messages::Error, "Fail to read the current marker segment (%#x)\n", current_marker);
                        return st;
                    }
                }
                if (read(h, 2) != 2) {
                    msg.emit(Messages::Error, "Stream too short\n");
                    return NJ2K_E_NEED_DATA;
                }
                current_marker = be16(h);
            }
            if (bytes_left() == 0 && state == kNeoc) {
                break;
            }
            if (const nj2k_status_t st = check_tile_reduce(); st != NJ2K_OK) {
                return st;
            }
            if (const nj2k_status_t st = read_sod(); st != NJ2K_OK) {
                return st;
            }
            if (can_decode && !correction_checked) {
                // OpenJPEG issue 254
                bool needed = false;
                correction_checked = true;
                const TileWalk* tw = find_walk(current_tile);
                if (tw->nb_tile_parts == 1) {
                    // a single tile-part declared: no check (as OpenJPEG since 2024)
                } else if (const nj2k_status_t st = need_correction(current_tile, needed); st != NJ2K_OK) {
                    msg.emit(Messages::Error, "opj_j2k_apply_nb_tile_parts_correction error\n");
                    return st;
                }
                if (needed) {
                    can_decode = false;
                    correction = 1;
                    for (std::uint32_t p = 0; pages && p < page_count; ++p) {
                        if (!pages[p]) {
                            continue;
                        }
                        for (std::uint32_t i = 0, n = page_tiles(p); i < n; ++i) {
                            if (pages[p][i].nb_tile_parts != 0U) {
                                pages[p][i].nb_tile_parts += 1;
                            }
                        }
                    }
                    msg.emit(Messages::Warning, "Non conformant codestream TPsot==TNsot.\n");
                }
            }
            if (!can_decode && !d.strict && last_tile_part && bytes_left() == 0) {
                // the last tile-part (Psot = 0) of a cut codestream took every byte left
                state = kNeoc;
                break;
            }
            if (!can_decode) {
                if (read(h, 2) != 2) {
                    // likely non-conformant SPOT6 files: the last row of tiles with TPsot == 0 and
                    // TNsot == 0, no EOC, and no other tile-part found
                    if (current_tile + 1 == mh.tile_count()) {
                        std::uint32_t l_tile_no = 0;
                        for (; l_tile_no < mh.tile_count(); ++l_tile_no) {
                            const TileWalk* tw = find_walk(l_tile_no);
                            const std::int32_t cur = tw ? tw->current_tile_part_number : -1;
                            const std::uint32_t nb = tw ? tw->nb_tile_parts : 0;
                            if (cur == 0 && nb == 0) {
                                break;
                            }
                        }
                        if (l_tile_no < mh.tile_count()) {
                            msg.emit(Messages::Info,
                                     "Tile %u has TPsot == 0 and TNsot == 0, but no other tile-parts were found. EOC "
                                     "is also missing.\n",
                                     l_tile_no);
                            current_tile = l_tile_no;
                            current_marker = mk::EOC;
                            state = kEoc;
                            break;
                        }
                    }
                    msg.emit(Messages::Error, "Stream too short\n");
                    return NJ2K_E_NEED_DATA;
                }
                current_marker = be16(h);
            }
        }
        if (current_marker == mk::EOC) {
            if (state != kEoc) {
                current_tile = 0;
                state = kEoc;
            }
        }
        // tiles with a single tile-part of TPsot == 0 and TNsot == 0
        if (!can_decode) {
            while (current_tile < mh.tile_count()) {
                if (decodable(current_tile, find_walk(current_tile))) {
                    break;
                }
                ++current_tile;
            }
            if (current_tile == mh.tile_count()) {
                go_on = false;
                return NJ2K_OK;
            }
        }
        go_on = true;
        state |= kData;
        return NJ2K_OK;
    }

    // ---- one tile: its layout, packets and samples --------------------------------------------
    // The tile's bytes after SOD, in one piece: where they lie when they came in one tile-part,
    // gathered into the work arena otherwise (OpenJPEG concatenates every tile-part's data).
    bool tile_bytes(const TileWalk& tw, const std::uint8_t*& data, std::uint32_t& size) noexcept
    {
        data = nullptr;
        size = 0;
        if (tw.nranges == 1) {
            data = b.base + (tw.ranges[0].pos - b.origin);
            size = tw.ranges[0].len;
            return true;
        }
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < tw.nranges; ++i) {
            total += tw.ranges[i].len;
        }
        if (total == 0) {
            return true;
        }
        if (total > UINT_MAX) {
            return false;
        }
        auto* buf = d.work.raw_array<std::uint8_t>(static_cast<std::size_t>(total));
        if (!buf) {
            return false;
        }
        std::size_t at = 0;
        for (std::uint32_t i = 0; i < tw.nranges; ++i) {
            std::memcpy(buf + at, b.base + (tw.ranges[i].pos - b.origin), tw.ranges[i].len);
            at += tw.ranges[i].len;
        }
        data = buf;
        size = static_cast<std::uint32_t>(total);
        return true;
    }

    // opj_j2k_merge_ppt: the tile's PPT markers in Zppt order, in one buffer
    nj2k_status_t merge_ppt(TileWalk& tw, const TileOverrides* ov, bool& has_ppt, PackedCursor& ppt) noexcept
    {
        has_ppt = ov && ov->ppt.size;
        ppt = PackedCursor{nullptr, 0};
        if (!has_ppt) {
            return NJ2K_OK;
        }
        if (tw.ppt_merged) {
            msg.emit(Messages::Error, "opj_j2k_merge_ppt() has already been called\n");
            msg.emit(Messages::Error, "Failed to merge PPT data\n");
            return NJ2K_E_HEADER;
        }
        tw.ppt_merged = true;
        // in Zppt order (each index once: the header model refuses a repeated one)
        auto& list = const_cast<ArenaList<PackedChunk>&>(ov->ppt);
        for (std::uint32_t i = 1; i < list.size; ++i) {
            const PackedChunk c = list.data[i];
            std::uint32_t j = i;
            while (j > 0 && list.data[j - 1].z > c.z) {
                list.data[j] = list.data[j - 1];
                --j;
            }
            list.data[j] = c;
        }
        std::uint64_t total = 0;
        for (const PackedChunk& c : list) {
            total += c.size;
        }
        auto* buf = total ? d.work.raw_array<std::uint8_t>(static_cast<std::size_t>(total)) : nullptr;
        if (total && !buf) {
            return nomem();
        }
        std::size_t at = 0;
        for (const PackedChunk& c : list) {
            if (c.size) {
                std::memcpy(buf + at, c.data, c.size);
            }
            at += c.size;
        }
        ppt = PackedCursor{buf, static_cast<std::uint32_t>(total)};
        return NJ2K_OK;
    }

    // opj_j2k_update_image_data for one component, up to the copy: the tile's decoded region that
    // goes into the image (cc, its source and its place there), the image plane made (nj2k_decode);
    // skip when nothing of the component is copied, clear when the caller's planes of it are to be
    // cleared first (its first tile, which does not cover it)
    struct Prepared {
        CopyComp cc;
        bool skip, clear;
    };
    nj2k_status_t prepare_component(std::uint32_t compno, const Tile& t, std::uint32_t resno_decoded,
                                    Prepared& out) noexcept
    {
        const TileComp& tc = t.comps[compno];
        OutComp& dst = comps[compno];
        dst.resno_decoded = resno_decoded;
        out.skip = true;
        out.clear = false;
        if (d.selected_mask && !d.selected_mask[compno]) {
            return NJ2K_OK;  // partial component decoding
        }
        if (!tc.data) {
            return NJ2K_OK;
        }
        const Resolution& res = tc.res[resno_decoded];
        const std::int32_t res_x0 = res.x0, res_y0 = res.y0, res_x1 = res.x1, res_y1 = res.y1;
        const std::uint32_t src_data_stride = tc.data_w;
        const std::uint32_t l_width_src = static_cast<std::uint32_t>(res_x1 - res_x0);
        const std::uint32_t l_height_src = static_cast<std::uint32_t>(res_y1 - res_y0);
        const std::uint32_t l_x0_dest = ceildivpow2(dst.x0, dst.factor);
        const std::uint32_t l_y0_dest = ceildivpow2(dst.y0, dst.factor);
        const std::uint32_t l_x1_dest = l_x0_dest + dst.w;
        const std::uint32_t l_y1_dest = l_y0_dest + dst.h;
        std::uint32_t l_start_x_dest, l_start_y_dest, l_width_dest, l_height_dest;
        std::int32_t l_offset_x0_src, l_offset_y0_src, l_offset_x1_src, l_offset_y1_src;
        if (l_x0_dest < static_cast<std::uint32_t>(res_x0)) {
            l_start_x_dest = static_cast<std::uint32_t>(res_x0) - l_x0_dest;
            l_offset_x0_src = 0;
            if (l_x1_dest >= static_cast<std::uint32_t>(res_x1)) {
                l_width_dest = l_width_src;
                l_offset_x1_src = 0;
            } else {
                l_width_dest = l_x1_dest - static_cast<std::uint32_t>(res_x0);
                l_offset_x1_src = static_cast<std::int32_t>(l_width_src - l_width_dest);
            }
        } else {
            l_start_x_dest = 0U;
            l_offset_x0_src = static_cast<std::int32_t>(l_x0_dest) - res_x0;
            if (l_x1_dest >= static_cast<std::uint32_t>(res_x1)) {
                l_width_dest = l_width_src - static_cast<std::uint32_t>(l_offset_x0_src);
                l_offset_x1_src = 0;
            } else {
                l_width_dest = dst.w;
                l_offset_x1_src = res_x1 - static_cast<std::int32_t>(l_x1_dest);
            }
        }
        if (l_y0_dest < static_cast<std::uint32_t>(res_y0)) {
            l_start_y_dest = static_cast<std::uint32_t>(res_y0) - l_y0_dest;
            l_offset_y0_src = 0;
            if (l_y1_dest >= static_cast<std::uint32_t>(res_y1)) {
                l_height_dest = l_height_src;
                l_offset_y1_src = 0;
            } else {
                l_height_dest = l_y1_dest - static_cast<std::uint32_t>(res_y0);
                l_offset_y1_src = static_cast<std::int32_t>(l_height_src - l_height_dest);
            }
        } else {
            l_start_y_dest = 0U;
            l_offset_y0_src = static_cast<std::int32_t>(l_y0_dest) - res_y0;
            if (l_y1_dest >= static_cast<std::uint32_t>(res_y1)) {
                l_height_dest = l_height_src - static_cast<std::uint32_t>(l_offset_y0_src);
                l_offset_y1_src = 0;
            } else {
                l_height_dest = dst.h;
                l_offset_y1_src = res_y1 - static_cast<std::int32_t>(l_y1_dest);
            }
        }
        if ((l_offset_x0_src < 0) || (l_offset_y0_src < 0) || (l_offset_x1_src < 0) || (l_offset_y1_src < 0)) {
            return NJ2K_E_DATA;
        }
        if (static_cast<std::int32_t>(l_width_dest) < 0 || static_cast<std::int32_t>(l_height_dest) < 0) {
            return NJ2K_E_DATA;
        }
        const std::size_t l_start_offset_src =
            static_cast<std::size_t>(l_offset_x0_src) + static_cast<std::size_t>(l_offset_y0_src) * src_data_stride;
        const std::int32_t* src = tc.data + l_start_offset_src;
        const bool covers = l_start_x_dest == 0 && l_start_y_dest == 0 && l_width_dest == dst.w && l_height_dest == dst.h;
        if (sink.image_planes) {
            if (!dst.data) {
                const std::size_t w = dst.w, h = dst.h;
                if ((h == 0U) || (w > (SIZE_MAX / h)) || w * h > SIZE_MAX / sizeof(std::int32_t)) {
                    return NJ2K_E_DATA;
                }
                dst.data = static_cast<std::int32_t*>(plane_alloc(w * h * sizeof(std::int32_t)));
                if (!dst.data) {
                    msg.emit(Messages::Error, "Not enough memory for the image\n");
                    return NJ2K_E_NO_MEMORY;
                }
                if (dst.w != l_width_dest || dst.h != l_height_dest) {
                    std::int32_t* plane = dst.data;
                    by_bands(dst.h, std::uint64_t(w) * h * sizeof(std::int32_t),
                             [&](std::uint32_t j0, std::uint32_t j1) noexcept {
                                 std::memset(plane + std::size_t(j0) * w, 0,
                                             std::size_t(j1 - j0) * w * sizeof(std::int32_t));
                             });
                }
            }
        }
        out.cc = CopyComp{src, src_data_stride, l_start_x_dest, l_start_y_dest, l_width_dest, l_height_dest,
                          t.finish_of(compno), compno, true};
        out.skip = false;
        out.clear = !sink.image_planes && !dst.written && !covers;
        return NJ2K_OK;
    }

    // prepare_component for the single tile that is the image, sample for sample (whole_fits), its
    // decoded region the whole buffer (hand_over_whole checks it): the component whole
    nj2k_status_t prepare_whole(std::uint32_t compno, const Tile& t, std::uint32_t resno_decoded, Prepared& out) noexcept
    {
        const TileComp& tc = t.comps[compno];
        OutComp& dst = comps[compno];
        dst.resno_decoded = resno_decoded;
        out.skip = (d.selected_mask && !d.selected_mask[compno]) || !tc.data;
        out.clear = false;
        if (!out.skip) {
            out.cc = CopyComp{tc.data, tc.data_w, 0, 0, dst.w, dst.h, t.finish_of(compno), compno, true};
        }
        return NJ2K_OK;
    }

    // the image plane of component c (nj2k_decode's output) as a plane of the caller's
    nj2k_plane_t image_plane(std::uint32_t c) const noexcept
    {
        return nj2k_plane_t{comps[c].data, static_cast<std::int64_t>(comps[c].w) * 4, 4, c};
    }

    // Writes k prepared components (one group of output.h's write_group: the same region, the colour
    // transformed three first when mct is set) finished into the image or the caller's planes.
    // write_planes skips the caller's plane that a tile decoded into (pl.data == src, direct_output);
    // this path has no such skip, deliberately: a direct-output tile never comes here (decode_tile
    // finishes it in place and hand_over_whole copies it to the other planes with write_planes), so
    // every source here is the decoder's own buffer and every plane of the component is written.
    void write_prepared(const Prepared* p, std::uint32_t k, Mct mct) noexcept
    {
        CopyComp cc[4];
        nj2k_plane_t own[4];
        for (std::uint32_t i = 0; i < k; ++i) {
            cc[i] = p[i].cc;
            if (sink.image_planes) {
                own[i] = image_plane(p[i].cc.component);
            } else if (p[i].clear) {
                clear_planes(p[i].cc.component);
            }
        }
        const std::uint32_t type = sink.image_planes ? NJ2K_SAMPLE_I32 : sink.output->sample_type;
        const nj2k_plane_t* planes = sink.image_planes ? own : sink.output->planes;
        const std::uint32_t numplanes = sink.image_planes ? k : sink.output->numplanes;
        if (tile_exec) {
            write_group_jobs(cc, k, mct, type, planes, numplanes, *tile_exec);
        } else {
            write_group(cc, k, mct, type, planes, numplanes);
        }
        for (std::uint32_t i = 0; i < k; ++i) {
            comps[p[i].cc.component].written = true;
        }
    }

    // Two prepared components that one pass can write together (output.h's write_group): the same
    // region of the image; for the colour transform, the same source layout too, so the samples it
    // pairs are those of the same place in the three buffers (OpenJPEG's flat l_samples)
    static bool same_region(const CopyComp& a, const CopyComp& b, bool layout) noexcept
    {
        if (a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h) {
            return false;
        }
        return !layout || a.src_stride == b.src_stride;
    }

    // opj_j2k_update_image_data for every component of a tile that is not the image: finished on
    // the way (the colour transform, the DC shift) into the image or the caller's planes, up to
    // four components of the same region together. When the transform's three do not pair their
    // samples by place, the tile is finished in place first, as OpenJPEG does, and copied.
    nj2k_status_t write_tile(Tile& t, const std::uint32_t* resno, bool whole = false) noexcept
    {
        // the single tile whose buffer is the image (hand_over_whole): each component whole, no clipping
        auto prepare = [&](std::uint32_t c, Prepared& out) {
            return whole ? prepare_whole(c, t, resno[c], out) : prepare_component(c, t, resno[c], out);
        };
        Prepared p[4];
        std::uint32_t n = 0;  // prepared, not written yet
        Mct mct = Mct::kNone;
        std::uint32_t c = 0;
        if (t.mct != Mct::kNone) {
            nj2k_status_t st = NJ2K_OK;
            for (; c < 3; ++c) {
                st = prepare(c, p[c]);
                if (st != NJ2K_OK || p[c].skip) {
                    break;
                }
            }
            const bool paired = c == 3 && same_region(p[0].cc, p[1].cc, true) && same_region(p[0].cc, p[2].cc, true) &&
                                p[1].cc.src - t.comps[1].data == p[0].cc.src - t.comps[0].data &&
                                p[2].cc.src - t.comps[2].data == p[0].cc.src - t.comps[0].data;
            if (!paired) {
                finish_tile_in_place(t, resno);
                for (std::uint32_t k = 0; k < c; ++k) {
                    if (!p[k].skip) {
                        p[k].cc.finish = false;
                        write_prepared(&p[k], 1, Mct::kNone);
                    }
                }
                if (st != NJ2K_OK) {
                    return st;
                }
                return copy_tile(t, resno, c);
            }
            n = 3;
            mct = t.mct;
        }
        for (; c < mh.numcomps; ++c) {
            Prepared q;
            const nj2k_status_t st = prepare(c, q);
            if (st != NJ2K_OK) {
                if (n) {
                    write_prepared(p, n, mct);
                }
                return st;
            }
            if (q.skip) {
                continue;
            }
            if (n == 4 || (n && !same_region(p[0].cc, q.cc, false))) {
                write_prepared(p, n, mct);
                n = 0;
                mct = Mct::kNone;
            }
            p[n++] = q;
        }
        if (n) {
            write_prepared(p, n, mct);
        }
        return NJ2K_OK;
    }

    // The components from 'from' on, copied as they are (finished in place already)
    nj2k_status_t copy_tile(Tile& t, const std::uint32_t* resno, std::uint32_t from) noexcept
    {
        for (std::uint32_t c = from; c < mh.numcomps; ++c) {
            Prepared q;
            if (const nj2k_status_t st = prepare_component(c, t, resno[c], q); st != NJ2K_OK) {
                return st;
            }
            if (!q.skip) {
                q.cc.finish = false;
                write_prepared(&q, 1, Mct::kNone);
            }
        }
        return NJ2K_OK;
    }

    // The tile's samples finished where they are, as OpenJPEG does it: the colour transform over
    // the three whole buffers, then each component's DC shift over its decoded region
    void finish_tile_in_place(Tile& t, const std::uint32_t* resno) noexcept
    {
        auto view = [&](std::uint32_t c) {
            const TileComp& tc = t.comps[c];
            const Resolution& r = tc.res[resno[c]];
            const bool active = !(d.selected_mask && !d.selected_mask[c]) && tc.data;
            return InPlaceComp{tc.data, tc.data_w, tc.data_h, static_cast<std::uint32_t>(r.x1 - r.x0),
                               static_cast<std::uint32_t>(r.y1 - r.y0), t.finish_of(c), active};
        };
        std::uint32_t c = 0;
        if (t.mct != Mct::kNone) {
            InPlaceComp three[3] = {view(0), view(1), view(2)};
            if (tile_exec) {
                finish_in_place_mct_jobs(three, t.mct, *tile_exec);
            } else {
                finish_in_place_mct(three, t.mct);
            }
            c = 3;
        }
        for (; c < mh.numcomps; ++c) {
            const InPlaceComp one = view(c);
            if (one.active) {
                if (tile_exec) {
                    finish_in_place_one_jobs(one, *tile_exec);
                } else {
                    finish_in_place_one(one);
                }
            }
        }
        t.mct = Mct::kNone;
    }

    // the caller's planes of component compno cleared (its first tile does not cover it)
    void clear_planes(std::uint32_t compno) noexcept
    {
        const nj2k_output_t& o = *sink.output;
        const OutComp& dst = comps[compno];
        for (std::uint32_t k = 0; k < o.numplanes; ++k) {
            const nj2k_plane_t& pl = o.planes[k];
            if (pl.component != compno) {
                continue;
            }
            auto* base = static_cast<std::uint8_t*>(pl.data);
            // a row at once when its samples are packed, else each sample with a store of its
            // own size (a memset call per sample took a second for a 16,384-pixel image)
            const std::size_t ss = o.sample_type == NJ2K_SAMPLE_U8 ? 1 : o.sample_type == NJ2K_SAMPLE_U16 ? 2 : 4;
            by_bands(dst.h, std::uint64_t(dst.w) * dst.h * ss, [&](std::uint32_t j0, std::uint32_t j1) noexcept {
                for (std::uint32_t j = j0; j < j1; ++j) {
                    std::uint8_t* row = base + static_cast<std::int64_t>(j) * pl.row_stride;
                    if (pl.sample_stride == ss) {
                        std::memset(row, 0, std::size_t(dst.w) * ss);
                    } else if (ss == 1) {
                        for (std::uint32_t i = 0; i < dst.w; ++i) {
                            row[std::size_t(i) * pl.sample_stride] = 0;
                        }
                    } else if (ss == 2) {
                        const std::uint16_t zero = 0;
                        for (std::uint32_t i = 0; i < dst.w; ++i) {
                            std::memcpy(row + std::size_t(i) * pl.sample_stride, &zero, sizeof zero);
                        }
                    } else {
                        const std::uint32_t zero = 0;
                        for (std::uint32_t i = 0; i < dst.w; ++i) {
                            std::memcpy(row + std::size_t(i) * pl.sample_stride, &zero, sizeof zero);
                        }
                    }
                }
            });
        }
    }

    // rows(j0, j1) over [0, h) of an image plane of the given bytes: in bands through the executor
    // when the codec has one and the plane has jobs.clear_bytes or more (the clears of planes a first
    // tile does not cover: a 16,384-pixel image's are a gigabyte a component), else at once
    template <class F>
    void by_bands(std::uint32_t h, std::uint64_t bytes, F&& rows) noexcept
    {
        std::uint32_t parts = 1;
        if (have_jobs && bytes >= jobs.clear_bytes) {
            parts = h < jobs.width ? h : jobs.width;
        }
        if (parts < 2) {
            rows(0u, h);
            return;
        }
        auto band = [&](std::uint32_t i) noexcept { rows(part_begin(h, parts, i), part_begin(h, parts, i + 1)); };
        jobs.run(parts, band);
    }

    // the caller's planes of component compno but the one the tile decoded into (direct_output):
    // the whole component, copied from that plane
    void write_planes(std::uint32_t compno, const std::int32_t* src, std::size_t src_stride,
                      std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) noexcept
    {
        const nj2k_output_t& o = *sink.output;
        for (std::uint32_t k = 0; k < o.numplanes; ++k) {
            const nj2k_plane_t& pl = o.planes[k];
            if (pl.component != compno || pl.data == static_cast<const void*>(src)) {
                continue;  // not this component's, or the plane the tile decoded into (direct_output)
            }
            auto* base = static_cast<std::uint8_t*>(pl.data);
            for (std::uint32_t j = 0; j < h; ++j) {
                const std::int32_t* s = src + std::size_t(j) * src_stride;
                std::uint8_t* row = base + static_cast<std::int64_t>(y + j) * pl.row_stride + std::size_t(x) * pl.sample_stride;
                switch (o.sample_type) {
                case NJ2K_SAMPLE_U8:
                    if (pl.sample_stride == 1) {
                        for (std::uint32_t i = 0; i < w; ++i) {
                            row[i] = static_cast<std::uint8_t>(s[i]);
                        }
                    } else {
                        for (std::uint32_t i = 0; i < w; ++i) {
                            row[std::size_t(i) * pl.sample_stride] = static_cast<std::uint8_t>(s[i]);
                        }
                    }
                    break;
                case NJ2K_SAMPLE_U16:
                    for (std::uint32_t i = 0; i < w; ++i) {
                        const std::uint16_t v = static_cast<std::uint16_t>(s[i]);
                        std::memcpy(row + std::size_t(i) * pl.sample_stride, &v, sizeof v);
                    }
                    break;
                default:
                    if (pl.sample_stride == 4) {
                        std::memcpy(row, s, std::size_t(w) * sizeof(std::int32_t));
                    } else {
                        for (std::uint32_t i = 0; i < w; ++i) {
                            std::memcpy(row + std::size_t(i) * pl.sample_stride, s + i, sizeof(std::int32_t));
                        }
                    }
                    break;
                }
            }
        }
    }

    // The decoder limits' working memory for a tile whose own coding (a tile-part COD or COC)
    // changes it: the decode's check when it started took the main header's coding, so this tile's
    // need is checked with its own parameters before anything of it is allocated.
    nj2k_status_t check_tile_memory(std::uint32_t tile, const TileOverrides* ov, const TileComponentParams* params) noexcept
    {
        if (!d.limits.max_memory || !ov || (!ov->has_cod && ov->coc.size == 0)) {
            return NJ2K_OK;
        }
        if (image_need == UINT64_MAX) {
            image_need = image_working_memory(mh, d.factor, d.selected_mask);
        }
        const std::uint64_t part = tile_working_memory(mh, tile, params, d.factor, d.selected_mask);
        const std::uint64_t need = part > UINT64_MAX - image_need ? UINT64_MAX : image_need + part;
        if (need > d.limits.max_memory) {
            msg.emit(Messages::Error,
                     "Tile %u needs %llu bytes of working memory with its own coding, above the decoder's limit of "
                     "%llu\n",
                     tile, static_cast<unsigned long long>(need), static_cast<unsigned long long>(d.limits.max_memory));
            return NJ2K_E_LIMIT;
        }
        return NJ2K_OK;
    }

    // opj_j2k_read_tile_header's tile set-up and opj_j2k_decode_tile, then the image update.
    // whole_image: the single-tile path of opj_j2k_decode_tiles (the tile is the image).
    nj2k_status_t decode_tile(bool whole_image) noexcept
    {
        const std::uint32_t tile = current_tile;
        TileWalk* tw = find_walk(tile);
        const WorkArena::Mark mark = d.work.mark();
        // the tile's parameters, with precedence applied (W7) and step counts checked (W8)
        const TileOverrides* ov = mh.overrides(tile);
        auto* params = d.work.raw_array<TileComponentParams>(mh.numcomps);
        if (!params) {
            return nomem();
        }
        if (const nj2k_status_t st = resolve_tile(mh, ov, msg, params); st != NJ2K_OK) {
            return st;
        }
        if (const nj2k_status_t st = check_tile_memory(tile, ov, params); st != NJ2K_OK) {
            return st;
        }
        TileInput in{};
        in.mh = &mh;
        in.tile = tile;
        in.params = params;
        in.reduce = d.factor;
        in.layers = d.layers;
        in.strict = d.strict;
        in.selected = d.selected_mask;
        in.ppm = mh.has_ppm ? &ppm : nullptr;
        if (const nj2k_status_t st = merge_ppt(*tw, ov, in.has_ppt, in.ppt); st != NJ2K_OK) {
            return st;
        }
        if (!tile_bytes(*tw, in.data, in.size)) {
            return nomem();
        }
        in.cut = tw->cut;
        Tile t;
        if (const nj2k_status_t st = t.init(in, d.work, msg); st != NJ2K_OK) {
            msg.emit(Messages::Error, "Cannot decode tile, memory error\n");
            return st;
        }
        // opj_j2k_decode_tile: the tile needs data (DP-3: or packed packet headers)
        if (!decodable(tile, tw)) {
            return NJ2K_E_DATA;
        }
        auto* resno = d.work.raw_array<std::uint32_t>(mh.numcomps);
        if (!resno) {
            return nomem();
        }
        // data that ends inside a packet makes no truncation of its own: when the bytes were cut,
        // the tile-part's length or the markers after it say so; when they were not, the packets
        // claim more than their tile-part holds (damage, decoded as OpenJPEG does, with a warning)
        nj2k_status_t st = t.read_packets(d.work, msg, resno);
        // the samples: the image's planes when the tile is the image, else the decoder's buffer
        if (st == NJ2K_OK) {
            st = attach_samples(t, whole_image);
        }
        TileJobs tj;
        tile_exec = nullptr;
        if (st == NJ2K_OK && threaded_tile(t, tj)) {
            tile_exec = &jobs;
        } else if (st == NJ2K_OK) {
            if (!d.t1.ready(d.allocator) || !d.t1.reserve(d.allocator, t.largest_cblk_bytes()) ||
                !d.dwt.ensure(d.allocator, t.dwt_scratch_bytes_needed())) {
                st = nomem();
            }
        }
        if (st == NJ2K_OK) {
            st = t.decode_samples(d.t1, d.dwt.p, resno, msg, tile_exec ? &tj : nullptr);
        }
        // a tile that is the image's planes is finished where it is; any other goes into the image
        // finished on the way (write_tile)
        if (st == NJ2K_OK && whole_image && (sink.image_planes || direct_tile)) {
            finish_tile_in_place(t, resno);
        }
        if (st != NJ2K_OK) {
            state |= kErr;
            msg.emit(Messages::Error, "Failed to decode.\n");
            free_whole_planes(t, whole_image);
            return st;
        }
        can_decode = false;
        state &= ~kData;
        // the marker after the tile
        if (!(bytes_left() == 0 && state == kNeoc) && state != kEoc) {
            std::uint8_t h[2] = {};
            if (read(h, 2) != 2) {
                msg.emit(d.strict ? Messages::Error : Messages::Warning, "Stream too short\n");
                truncated = true;
                if (d.strict) {
                    free_whole_planes(t, whole_image);
                    return NJ2K_E_NEED_DATA;
                }
            } else {
                const std::uint32_t m = be16(h);
                if (m == mk::EOC) {
                    current_tile = 0;
                    state = kEoc;
                } else if (m != mk::SOT) {
                    if (bytes_left() == 0) {
                        state = kNeoc;
                        truncated = true;
                        msg.emit(Messages::Warning, "Stream does not end with EOC\n");
                    } else {
                        msg.emit(Messages::Error, "Stream too short, expected SOT\n");
                        free_whole_planes(t, whole_image);
                        return NJ2K_E_DATA;
                    }
                }
            }
        }
        // the image
        if (const nj2k_status_t u = whole_image ? hand_over_whole(t, resno) : write_tile(t, resno); u != NJ2K_OK) {
            return u;
        }
        // opj_j2k_tcp_data_destroy (a 1 x 1 grid keeps it, but is not decoded again)
        tw->seen = false;
        tw->has_data = false;
        tw->cut = false;
        tw->nranges = 0;
        d.work.rewind(mark);
        return NJ2K_OK;
    }

    // whether the single tile can go straight to the image (its decoded resolution is the image's
    // component, sample for sample)
    bool whole_fits(const Tile& t) const noexcept
    {
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            if (t.comps[c].data_w != comps[c].w || t.comps[c].data_h != comps[c].h) {
                return false;
            }
        }
        return true;
    }

    // The memory decode of a single-tile image (whole_fits) into int32 planes laid out as the tile's
    // own buffer (4-byte samples, aligned, rows of exactly the component's width, top-down): the tile
    // decodes straight into the caller's planes, with no buffer of its own and no copy. Every
    // component decoded needs such a plane (the first one of the component; any other plane of it
    // is then written from it), or none is used.
    bool direct_output(Tile& t) noexcept
    {
        const nj2k_output_t& o = *sink.output;
        if (o.sample_type != NJ2K_SAMPLE_I32) {
            return false;
        }
        for (int assign = 0; assign < 2; ++assign) {
            for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
                if (d.selected_mask && !d.selected_mask[c]) {
                    continue;
                }
                const nj2k_plane_t* direct = nullptr;
                for (std::uint32_t k = 0; k < o.numplanes && !direct; ++k) {
                    const nj2k_plane_t& pl = o.planes[k];
                    if (pl.component == c && pl.sample_stride == sizeof(std::int32_t) &&
                        pl.row_stride == static_cast<std::int64_t>(t.comps[c].data_w) * 4 &&
                        (reinterpret_cast<std::uintptr_t>(pl.data) & 3) == 0) {
                        direct = &pl;
                    }
                }
                if (!direct) {
                    return false;
                }
                if (assign) {
                    t.comps[c].data = t.samples_bytes(c) ? static_cast<std::int32_t*>(direct->data) : nullptr;
                }
            }
        }
        return true;
    }

    nj2k_status_t attach_samples(Tile& t, bool& whole_image) noexcept
    {
        direct_tile = false;
        if (whole_image && !whole_fits(t)) {
            // the tile's decoded level is not the image's components, sample for sample (OpenJPEG
            // would hand over a buffer of another size): it is copied as any tile is, clipped to
            // the image, into the image planes or the caller's
            whole_image = false;
        }
        if (whole_image && !sink.image_planes && direct_output(t)) {
            direct_tile = true;
            return NJ2K_OK;
        }
        if (whole_image && sink.image_planes) {
            for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
                if (d.selected_mask && !d.selected_mask[c]) {
                    continue;
                }
                const std::size_t n = t.samples_bytes(c);
                if (n) {
                    t.comps[c].data = static_cast<std::int32_t*>(plane_alloc(n));
                    if (!t.comps[c].data) {
                        msg.emit(Messages::Error, "Size of tile data exceeds system limits\n");
                        free_whole_planes(t, true);
                        return NJ2K_E_NO_MEMORY;
                    }
                }
            }
            return NJ2K_OK;
        }
        std::size_t total = 0;
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            const std::size_t n = (t.samples_bytes(c) + 63) & ~std::size_t(63);
            if (n > SIZE_MAX - total) {
                return nomem();
            }
            total += n;
        }
        // the samples and the structures this decode holds in its working arena share the limit
        if (d.limits.max_memory &&
            (total > d.limits.max_memory || d.work.high_water() > d.limits.max_memory - total)) {
            msg.emit(Messages::Error,
                     "The tile's samples need %zu bytes, above what the decoder's memory limit leaves (%llu bytes "
                     "held for the tile's structures)\n",
                     total, static_cast<unsigned long long>(d.work.high_water()));
            return NJ2K_E_LIMIT;
        }
        if (!d.planes.ensure(d.allocator, total ? total : 64)) {
            return nomem();
        }
        std::size_t at = 0;
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            const std::size_t n = t.samples_bytes(c);
            t.comps[c].data = n ? reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(d.planes.p) + at) : nullptr;
            at += (n + 63) & ~std::size_t(63);
        }
        return NJ2K_OK;
    }

    void free_whole_planes(Tile& t, bool whole_image) noexcept
    {
        if (!whole_image || !sink.image_planes) {
            return;
        }
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            plane_free(t.comps[c].data);
            t.comps[c].data = nullptr;
        }
    }

    // "Transfer TCD data to output image data" (the single-tile path). The image's planes and the
    // caller's direct planes hold the tile finished in place (decode_tile); from the decoder's own
    // buffer, the tile is finished on the way when every decoded region is its whole buffer, as
    // write_tile copies it, else finished in place first and the whole buffer copied, as OpenJPEG
    // hands it over
    nj2k_status_t hand_over_whole(Tile& t, const std::uint32_t* resno) noexcept
    {
        if (!sink.image_planes && !direct_tile) {
            // The viewer's case first, with none of write_tile's grouping: every component decoded,
            // at most four, each decoded region its whole buffer and all of one size (whole_fits:
            // the image's), so they form one write_group pass, the colour transform's three paired
            // by place, as write_tile would group them. A few-pixel decode is mostly this fixed cost.
            if (!d.selected_mask && mh.numcomps <= 4) {
                CopyComp cc[4];
                const TileComp& t0 = t.comps[0];
                bool one = true;
                for (std::uint32_t c = 0; c < mh.numcomps && one; ++c) {
                    const TileComp& tc = t.comps[c];
                    const Resolution& r = tc.res[resno[c]];
                    one = tc.data && std::uint32_t(r.x1 - r.x0) == tc.data_w && std::uint32_t(r.y1 - r.y0) == tc.data_h &&
                          tc.data_w == t0.data_w && tc.data_h == t0.data_h;
                    cc[c] = CopyComp{tc.data, tc.data_w, 0, 0, comps[c].w, comps[c].h, t.finish_of(c), c, true};
                }
                if (one) {
                    if (tile_exec) {
                        write_group_jobs(cc, mh.numcomps, t.mct, sink.output->sample_type, sink.output->planes,
                                         sink.output->numplanes, *tile_exec);
                    } else {
                        write_group(cc, mh.numcomps, t.mct, sink.output->sample_type, sink.output->planes,
                                    sink.output->numplanes);
                    }
                    for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
                        comps[c].resno_decoded = resno[c];
                        comps[c].written = true;
                    }
                    return NJ2K_OK;
                }
            }
            bool whole = true;
            for (std::uint32_t c = 0; c < mh.numcomps && whole; ++c) {
                if (d.selected_mask && !d.selected_mask[c]) {
                    continue;
                }
                const TileComp& tc = t.comps[c];
                const Resolution& r = tc.res[resno[c]];
                whole = !tc.data || (std::uint32_t(r.x1 - r.x0) == tc.data_w && std::uint32_t(r.y1 - r.y0) == tc.data_h);
            }
            if (whole) {
                return write_tile(t, resno, true);
            }
            finish_tile_in_place(t, resno);
        }
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            OutComp& dst = comps[c];
            dst.resno_decoded = resno[c];
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            if (sink.image_planes) {
                plane_free(dst.data);
                dst.data = t.comps[c].data;
                t.comps[c].data = nullptr;
                dst.written = dst.data != nullptr;
            } else if (t.comps[c].data) {
                // the tile's buffer is the component, sample for sample (attach_samples: whole_fits)
                write_planes(c, t.comps[c].data, t.comps[c].data_w, 0, 0, dst.w, dst.h);
                dst.written = true;
            }
        }
        return NJ2K_OK;
    }

    // opj_j2k_decode_tiles
    nj2k_status_t run() noexcept
    {
        bool go_on = true;
        const bool single = mh.tw == 1 && mh.th == 1 && mh.tx0 == 0 && mh.ty0 == 0 && mh.x0 == 0 && mh.y0 == 0 &&
                            mh.x1 == mh.tdx && mh.y1 == mh.tdy;
        if (single) {
            if (const nj2k_status_t st = read_tile_header(go_on); st != NJ2K_OK) {
                return st;
            }
            if (!go_on) {
                msg.emit(Messages::Error, "Failed to decode tile 1/1\n");
                return NJ2K_E_NEED_DATA;
            }
            if (const nj2k_status_t st = decode_tile(true); st != NJ2K_OK) {
                msg.emit(Messages::Error, "Failed to decode tile 1/1\n");
                return st;
            }
            return NJ2K_OK;
        }
        std::uint32_t nr_tiles = 0;
        for (;;) {
            if (const nj2k_status_t st = read_tile_header(go_on); st != NJ2K_OK) {
                return st;
            }
            if (!go_on) {
                break;
            }
            const std::uint32_t tile = current_tile;
            if (const nj2k_status_t st = decode_tile(false); st != NJ2K_OK) {
                msg.emit(Messages::Error, "Failed to decode tile %u/%u\n", tile + 1, mh.tile_count());
                return st;
            }
            msg.emit(Messages::Info, "Tile %u/%u has been decoded.\n", tile + 1, mh.tile_count());
            if (bytes_left() == 0 && state == kNeoc) {
                break;
            }
            if (++nr_tiles == mh.tile_count()) {
                break;
            }
        }
        // opj_j2k_are_all_used_components_decoded
        bool all = true;
        for (std::uint32_t c = 0; c < mh.numcomps; ++c) {
            if (d.selected_mask && !d.selected_mask[c]) {
                continue;
            }
            if (!comps[c].written) {
                msg.emit(Messages::Warning, "Failed to decode component %u\n", c);
                all = false;
            }
        }
        if (!all) {
            msg.emit(Messages::Error, "Failed to decode all used components\n");
            // when the bytes ran out first, nothing was found damaged: more bytes bring the tiles (as
            // the single-tile path reports)
            return truncated || (state & kNeoc) ? NJ2K_E_NEED_DATA : NJ2K_E_DATA;
        }
        return NJ2K_OK;
    }
};

}  // namespace

nj2k_status_t decode_codestream(Decoder& d, const CodestreamBytes& bytes, OutComp* comps, const OutSink& sink,
                                bool& truncated) noexcept
{
    MainHeader& mh = *d.header;
    // the tile-part headers' markers are this decode's (the main header's model is kept)
    mh.tiles = TileDirectory{};
    d.tile_arena.reset(d.retain);
    d.tile_arena.set_budget(d.limits.max_memory);
    d.work.set_budget(d.limits.max_memory);
    Walk w(d, bytes, comps, sink);
    w.settings.limits = d.limits;
    w.settings.dump = d.dump;
    w.pos = bytes.start;
    w.correction_checked = d.tnsot_check_off;
    if (mh.has_ppm) {
        w.ppm = PackedCursor{mh.ppm_data, static_cast<std::uint32_t>(mh.ppm_size)};
    }
    const nj2k_status_t st = w.run();
    truncated = w.truncated || (st == NJ2K_OK && w.state != kEoc);
    mh.tiles = TileDirectory{};
    return st;
}

}  // namespace nj2k::detail
