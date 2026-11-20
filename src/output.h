// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The end of a tile's decode after the inverse wavelet, fused (stage 2): the colour transform
// (OpenJPEG 2.5.4's opj_mct_decode, opj_mct_decode_real), the DC level shift with its rounding and
// clamping (opj_tcd_dc_level_shift_decode) and the copy into the image (opj_j2k_update_image_data,
// or the caller's planes of nj2k_decode_memory, interleaved, flipped, 8, 16 or 32 bits a sample) in
// one pass over the tile's samples: each sample sees the operations of mct.h in the same order, so
// the output is the unfused one's (and OpenJPEG's), sample for sample. Rows go through a small
// stack buffer, so the pass needs no memory of its own.
#pragma once

#include <cstddef>
#include <cstdint>

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

enum class Mct : std::uint8_t { kNone, kRct, kIct };

// A component's DC shift: shift added, the result clamped to [lo, hi]; float samples (9/7,
// !reversible) rounded to nearest, ties to even, first (mct.h dc_shift_53, dc_shift_97).
struct Finish {
    std::int32_t shift, lo, hi;
    bool reversible;
};

// In place, as OpenJPEG does it on the tile's buffers: the colour transform over every sample of
// the three buffers (buf_w x buf_h each, as flat arrays: opj_tcd_mct_decode's l_samples of the first),
// then each component's DC shift over its decoded region (w x h, top left, rows of buf_w).
struct InPlaceComp {
    std::int32_t* data;
    std::uint32_t buf_w, buf_h, w, h;
    Finish f;
    bool active;
};
void finish_in_place_mct(const InPlaceComp* three, Mct mct) noexcept;
void finish_in_place_one(const InPlaceComp& comp) noexcept;

// A component's region to copy: w x h samples from src (rows of src_stride) to (x, y) of the
// component's planes, finished on the way (finish) or copied as they are.
struct CopyComp {
    const std::int32_t* src;
    std::size_t src_stride;
    std::uint32_t x, y, w, h;
    Finish f;
    std::uint32_t component;
    bool finish;
};

// k (1 to 4) components of regions of the same size and place, written into every plane of each
// (nj2k_plane_t: sample (x, y) at data + y * row_stride + x * sample_stride) as the sample type
// says (NJ2K_SAMPLE_I32, U8 or U16: the low bits kept). With mct, comps[0..2] are components 0-2,
// their samples paired by place, and take the colour transform first. Components whose 8-bit
// planes interleave them in one buffer (the viewer's raw image: one plane each, k bytes a pixel)
// are written a pixel at a time.
void write_group(const CopyComp* comps, std::uint32_t k, Mct mct, std::uint32_t sample_type, const nj2k_plane_t* planes,
                 std::uint32_t numplanes) noexcept;

// Stage 5: the same passes with the rows cut into bands for the host's executor (parallel.h), every
// sample taking the same operations; a region of fewer than jobs.output_samples samples a component
// runs on the calling thread.
struct Jobs;
void write_group_jobs(const CopyComp* comps, std::uint32_t k, Mct mct, std::uint32_t sample_type,
                      const nj2k_plane_t* planes, std::uint32_t numplanes, const Jobs& jobs) noexcept;
void finish_in_place_mct_jobs(const InPlaceComp* three, Mct mct, const Jobs& jobs) noexcept;
void finish_in_place_one_jobs(const InPlaceComp& comp, const Jobs& jobs) noexcept;

namespace kernels {
// The row kernels, scalar references and AVX2 versions, for the kernel tests: the colour transform
// out of place and the DC shift in place (each picks mct.cpp's row kernel), the
// stores (stride in bytes between samples; packed: k components interleaved, k = 1 to 4).
void mct_rows_scalar(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                     std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept;
void finish_row_scalar(std::int32_t* p, std::size_t n, const Finish& f) noexcept;
void store_u8_packed_scalar(const std::int32_t* const* rows, std::uint32_t k, std::size_t n, std::uint8_t* dst) noexcept;
void store_scalar(const std::int32_t* row, std::size_t n, std::uint32_t sample_type, std::uint8_t* dst,
                  std::uint32_t sample_stride) noexcept;
#if defined(__AVX2__)
void mct_rows_avx2(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                   std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept;
void finish_row_avx2(std::int32_t* p, std::size_t n, const Finish& f) noexcept;
void store_u8_packed_avx2(const std::int32_t* const* rows, std::uint32_t k, std::size_t n, std::uint8_t* dst) noexcept;
void store_avx2(const std::int32_t* row, std::size_t n, std::uint32_t sample_type, std::uint8_t* dst,
                std::uint32_t sample_stride) noexcept;
// The fused 8-bit pass of the viewer's image: k (1 to 4) components interleaved into k-byte pixels,
// h rows of n read straight from their sources (src, each row src_stride samples after the last),
// the colour transform (mct: the first three), the DC shift (f) and the store in registers, sixteen
// pixels at a time, into rows dst_stride bytes apart, for components whose finished samples lie in
// [0, 255] (u8_fusable). fused_u8_function gives the instance for a group; the instance returns the
// pixels it wrote of each row: n rounded down to 16. Every component must be 5/3 or every one 9/7,
// with the colour transform that goes with them; otherwise there is no instance (nullptr).
bool u8_fusable(const Finish& f) noexcept;
using FusedU8Fn = std::size_t (*)(const std::int32_t* const* src, const std::size_t* src_stride, const Finish* f,
                                  std::size_t n, std::uint32_t h, std::uint8_t* dst, std::int64_t dst_stride) noexcept;
FusedU8Fn fused_u8_function(Mct mct, const Finish* f, std::uint32_t k) noexcept;
#endif
}  // namespace kernels

}  // namespace nj2k::detail
