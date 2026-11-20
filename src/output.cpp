// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The fused end of a tile's decode (output.h): the colour transform, the DC shift and the copy into
// the image in one pass. The per-sample operations are mct.cpp's row kernels (OpenJPEG's, with its
// notice there), called in OpenJPEG's order; this file adds the row loop and the stores.
#include "output.h"

#include <cstring>

#include <neoj2k/neoj2k.h>

#include "mct.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nj2k::detail {

namespace {

// a row is finished and stored this many samples at a time (stack buffers of 4 x 512 bytes)
constexpr std::size_t kChunk = 128;

}  // namespace

namespace kernels {

// mct.cpp's row kernels (the one copy of OpenJPEG's per-sample operations), chosen per call
void mct_rows_scalar(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                     std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept
{
    if (mct == Mct::kRct) {
        rct_scalar(a, b, c, oa, ob, oc, n);
    } else {
        ict_scalar(a, b, c, oa, ob, oc, n);
    }
}

void finish_row_scalar(std::int32_t* p, std::size_t n, const Finish& f) noexcept
{
    if (f.reversible) {
        dc_shift_53_scalar(p, n, f.shift, f.lo, f.hi);
    } else {
        dc_shift_97_scalar(p, n, f.shift, f.lo, f.hi);
    }
}

// The low bits of each sample (opj_j2k_update_image_data's copy, then the caller's conversion)
void store_scalar(const std::int32_t* row, std::size_t n, std::uint32_t sample_type, std::uint8_t* dst,
                  std::uint32_t sample_stride) noexcept
{
    switch (sample_type) {
    case NJ2K_SAMPLE_U8:
        for (std::size_t i = 0; i < n; ++i) {
            dst[i * sample_stride] = static_cast<std::uint8_t>(row[i]);
        }
        break;
    case NJ2K_SAMPLE_U16:
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint16_t v = static_cast<std::uint16_t>(row[i]);
            std::memcpy(dst + i * sample_stride, &v, sizeof v);
        }
        break;
    default:
        if (sample_stride == sizeof(std::int32_t)) {
            if (n) {  // an empty row may come with null pointers, which memcpy may not take
                std::memcpy(dst, row, n * sizeof(std::int32_t));
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                std::memcpy(dst + i * sample_stride, row + i, sizeof(std::int32_t));
            }
        }
        break;
    }
}

// k rows interleaved into 8-bit pixels of k bytes
void store_u8_packed_scalar(const std::int32_t* const* rows, std::uint32_t k, std::size_t n, std::uint8_t* dst) noexcept
{
    for (std::size_t i = 0; i < n; ++i) {
        for (std::uint32_t c = 0; c < k; ++c) {
            dst[i * k + c] = static_cast<std::uint8_t>(rows[c][i]);
        }
    }
}

#if defined(__AVX2__)
void mct_rows_avx2(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                   std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept
{
    if (mct == Mct::kRct) {
        rct_avx2(a, b, c, oa, ob, oc, n);
    } else {
        ict_avx2(a, b, c, oa, ob, oc, n);
    }
}

void finish_row_avx2(std::int32_t* p, std::size_t n, const Finish& f) noexcept
{
    if (f.reversible) {
        dc_shift_53_avx2(p, n, f.shift, f.lo, f.hi);
    } else {
        dc_shift_97_avx2(p, n, f.shift, f.lo, f.hi);
    }
}

void store_avx2(const std::int32_t* row, std::size_t n, std::uint32_t sample_type, std::uint8_t* dst,
                std::uint32_t sample_stride) noexcept
{
    std::size_t i = 0;
    if (sample_type == NJ2K_SAMPLE_U8 && sample_stride == 1) {
        // the low byte of each lane, gathered into the low 8 bytes
        const __m256i pick = _mm256_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 4, 8, 12,
                                              -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m256i join = _mm256_setr_epi32(0, 4, 0, 0, 0, 0, 0, 0);
        for (; i + 8 <= n; i += 8) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
            v = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(v, pick), join);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + i), _mm256_castsi256_si128(v));
        }
        store_scalar(row + i, n - i, sample_type, dst + i, 1);
        return;
    }
    if (sample_type == NJ2K_SAMPLE_U16 && sample_stride == 2) {
        const __m256i pick = _mm256_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 4, 5, 8,
                                              9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1);
        for (; i + 8 <= n; i += 8) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
            v = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(v, pick), 0x08);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 2), _mm256_castsi256_si128(v));
        }
        store_scalar(row + i, n - i, sample_type, dst + i * 2, 2);
        return;
    }
    store_scalar(row, n, sample_type, dst, sample_stride);
}

void store_u8_packed_avx2(const std::int32_t* const* rows, std::uint32_t k, std::size_t n, std::uint8_t* dst) noexcept
{
    const __m256i ff = _mm256_set1_epi32(0xFF);
    std::size_t i = 0;
    switch (k) {
    case 1:
        store_avx2(rows[0], n, NJ2K_SAMPLE_U8, dst, 1);
        return;
    case 2: {
        const __m256i pick = _mm256_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 4, 5, 8,
                                              9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1);
        for (; i + 8 <= n; i += 8) {
            const __m256i a = _mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[0] + i)), ff);
            const __m256i b = _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[1] + i)), 8);
            __m256i w = _mm256_or_si256(a, b);
            w = _mm256_permute4x64_epi64(_mm256_shuffle_epi8(w, pick), 0x08);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 2), _mm256_castsi256_si128(w));
        }
        break;
    }
    case 3: {
        // each lane's four 3-byte pixels gathered into its low 12 bytes; the two 16-byte stores
        // overlap, and the second writes 4 bytes into the next two pixels, which the next
        // iteration (or the tail) writes again: the loop keeps two pixels beyond its eight
        const __m256i pick = _mm256_setr_epi8(0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, -1, -1, -1, -1, 0, 1, 2, 4, 5, 6,
                                              8, 9, 10, 12, 13, 14, -1, -1, -1, -1);
        const __m256i ffff = _mm256_set1_epi32(0xFFFF);
        for (; i + 10 <= n; i += 8) {
            const __m256i a = _mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[0] + i)), ff);
            const __m256i b = _mm256_and_si256(
                _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[1] + i)), 8), ffff);
            const __m256i c = _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[2] + i)), 16);
            __m256i w = _mm256_or_si256(_mm256_or_si256(a, b), c);
            w = _mm256_shuffle_epi8(w, pick);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 3), _mm256_castsi256_si128(w));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i * 3 + 12), _mm256_extracti128_si256(w, 1));
        }
        break;
    }
    default: {
        const __m256i ffff = _mm256_set1_epi32(0xFFFF);
        const __m256i ffffff = _mm256_set1_epi32(0xFFFFFF);
        for (; i + 8 <= n; i += 8) {
            const __m256i a = _mm256_and_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[0] + i)), ff);
            const __m256i b = _mm256_and_si256(
                _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[1] + i)), 8), ffff);
            const __m256i c = _mm256_and_si256(
                _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[2] + i)), 16), ffffff);
            const __m256i d = _mm256_slli_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(rows[3] + i)), 24);
            const __m256i w = _mm256_or_si256(_mm256_or_si256(a, b), _mm256_or_si256(c, d));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * 4), w);
        }
        break;
    }
    }
    const std::int32_t* rest[4] = {rows[0] + i, rows[1] + i, k > 2 ? rows[2] + i : nullptr, k > 3 ? rows[3] + i : nullptr};
    store_u8_packed_scalar(rest, k, n - i, dst + i * k);
}

#endif

}  // namespace kernels

namespace {

#if defined(__AVX2__)
#define NJ2K_K(name) kernels::name##_avx2
#else
#define NJ2K_K(name) kernels::name##_scalar
#endif

inline void mct_rows(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                     std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept
{
    NJ2K_K(mct_rows)(mct, a, b, c, oa, ob, oc, n);
}
inline void finish_row(std::int32_t* p, std::size_t n, const Finish& f) noexcept
{
    NJ2K_K(finish_row)(p, n, f);
}
inline void store(const std::int32_t* row, std::size_t n, std::uint32_t type, std::uint8_t* dst, std::uint32_t ss) noexcept
{
    NJ2K_K(store)(row, n, type, dst, ss);
}
inline void store_packed(const std::int32_t* const* rows, std::uint32_t k, std::size_t n, std::uint8_t* dst) noexcept
{
    NJ2K_K(store_u8_packed)(rows, k, n, dst);
}

#undef NJ2K_K

// Regions of at most this many pixels (the 1 x 1 floor of a reduced decode, 2 x 2) go a pixel at a
// time, every operation inline (mct.h's sample functions, the ones the scalar kernels loop over):
// the row kernels would run their scalar tails behind a call per component. From 3 x 3 on, the run
// below (one call per kernel over the region's samples, full vectors) is faster.
constexpr std::size_t kTinyPixels = 4;

void write_tiny_packed(const CopyComp* comps, std::uint32_t k, Mct mct, std::uint8_t* dst, std::int64_t row_stride) noexcept
{
    const CopyComp& c0 = comps[0];
    for (std::uint32_t j = 0; j < c0.h; ++j) {
        const std::int32_t* src[4];
        for (std::uint32_t c = 0; c < k; ++c) {
            src[c] = comps[c].src + std::size_t(j) * comps[c].src_stride;
        }
        std::uint8_t* d = dst + static_cast<std::int64_t>(j) * row_stride;
        for (std::uint32_t i = 0; i < c0.w; ++i) {
            std::int32_t s[4];
            for (std::uint32_t c = 0; c < k; ++c) {
                s[c] = src[c][i];
            }
            if (mct == Mct::kRct) {
                sample::rct(s[0], s[1], s[2]);
            } else if (mct == Mct::kIct) {
                sample::ict(s[0], s[1], s[2]);
            }
            for (std::uint32_t c = 0; c < k; ++c) {
                const CopyComp& cc = comps[c];
                std::int32_t v = s[c];
                if (cc.finish) {
                    v = cc.f.reversible ? sample::shift_53(v, cc.f.shift, cc.f.lo, cc.f.hi)
                                        : sample::shift_97(v, cc.f.shift, cc.f.lo, cc.f.hi);
                }
                d[std::size_t(i) * k + c] = static_cast<std::uint8_t>(v);
            }
        }
    }
}

}  // namespace

void write_group(const CopyComp* comps, std::uint32_t k, Mct mct, std::uint32_t type, const nj2k_plane_t* planes,
                 std::uint32_t numplanes) noexcept
{
    const CopyComp& c0 = comps[0];
    if (c0.w == 0 || c0.h == 0) {
        return;
    }
    // packed: one 8-bit plane each, the k components a pixel's consecutive bytes
    const nj2k_plane_t* only[4] = {};
    bool packed = type == NJ2K_SAMPLE_U8;
    for (std::uint32_t c = 0; c < k && packed; ++c) {
        std::uint32_t count = 0;
        for (std::uint32_t d = 0; d < numplanes; ++d) {
            if (planes[d].component == comps[c].component) {
                only[c] = &planes[d];
                ++count;
            }
        }
        packed = count == 1 && only[c]->sample_stride == k &&
                 static_cast<const std::uint8_t*>(only[c]->data) == static_cast<const std::uint8_t*>(only[0]->data) + c &&
                 only[c]->row_stride == only[0]->row_stride;
    }
    std::uint8_t* packed_base =
        packed ? static_cast<std::uint8_t*>(only[0]->data) + static_cast<std::int64_t>(c0.y) * only[0]->row_stride +
                     std::size_t(c0.x) * k
               : nullptr;
    if (packed && std::size_t(c0.w) * c0.h <= kTinyPixels) {
        write_tiny_packed(comps, k, mct, packed_base, only[0]->row_stride);
        return;
    }
    // A region of at most kChunk samples whose rows follow one another in every source (a small
    // tile's whole buffer) is finished as one run, so the row kernels see full vectors and are called
    // once, not once a row; its rows are then stored one by one.
    std::uint32_t run_rows = 1;
    if (c0.h > 1 && std::size_t(c0.w) * c0.h <= kChunk) {
        bool contiguous = true;
        for (std::uint32_t c = 0; c < k; ++c) {
            contiguous = contiguous && comps[c].src_stride == c0.w;
        }
        run_rows = contiguous ? c0.h : 1;
    }
    alignas(32) std::int32_t tmp[4][kChunk];
    for (std::uint32_t j = 0; j < c0.h; j += run_rows) {
        for (std::uint32_t x = 0; x < c0.w; x += static_cast<std::uint32_t>(kChunk)) {
            const std::size_t row_n = c0.w - x < kChunk ? c0.w - x : kChunk;
            const std::size_t n = row_n * run_rows;
            std::uint32_t c = 0;
            if (mct != Mct::kNone) {
                mct_rows(mct, comps[0].src + std::size_t(j) * comps[0].src_stride + x,
                         comps[1].src + std::size_t(j) * comps[1].src_stride + x,
                         comps[2].src + std::size_t(j) * comps[2].src_stride + x, tmp[0], tmp[1], tmp[2], n);
                c = 3;
            }
            for (; c < k; ++c) {
                std::memcpy(tmp[c], comps[c].src + std::size_t(j) * comps[c].src_stride + x, n * sizeof(std::int32_t));
            }
            for (c = 0; c < k; ++c) {
                if (comps[c].finish) {
                    finish_row(tmp[c], n, comps[c].f);
                }
            }
            const std::size_t xs = std::size_t(c0.x) + x;
            for (std::uint32_t r = 0; r < run_rows; ++r) {
                const std::size_t at = std::size_t(r) * row_n;
                if (packed) {
                    const std::int32_t* rows[4] = {tmp[0] + at, tmp[1] + at, tmp[2] + at, tmp[3] + at};
                    store_packed(rows, k, row_n,
                                 packed_base + static_cast<std::int64_t>(j + r) * only[0]->row_stride + std::size_t(x) * k);
                    continue;
                }
                const std::int64_t y = static_cast<std::int64_t>(c0.y) + j + r;
                for (c = 0; c < k; ++c) {
                    for (std::uint32_t d = 0; d < numplanes; ++d) {
                        const nj2k_plane_t& pl = planes[d];
                        if (pl.component == comps[c].component) {
                            store(tmp[c] + at, row_n, type,
                                  static_cast<std::uint8_t*>(pl.data) + y * pl.row_stride + xs * pl.sample_stride,
                                  pl.sample_stride);
                        }
                    }
                }
            }
        }
    }
}

void finish_in_place_mct(const InPlaceComp* p, Mct mct) noexcept
{
    // every sample of the three buffers takes the transform (the buffers as flat arrays), then each
    // component its DC shift; when the three have rows of the same width, a row at a time, each
    // row's shift while it is in the cache
    const std::uint32_t bw = p[0].buf_w;
    const bool rows = p[1].buf_w == bw && p[2].buf_w == bw;
    for (std::uint32_t j = 0; j < p[0].buf_h; ++j) {
        const std::size_t at = std::size_t(j) * bw;
        mct_rows(mct, p[0].data + at, p[1].data + at, p[2].data + at, p[0].data + at, p[1].data + at, p[2].data + at, bw);
        for (std::uint32_t k = 0; rows && k < 3; ++k) {
            if (p[k].active && j < p[k].h && p[k].w) {
                finish_row(p[k].data + at, p[k].w, p[k].f);
            }
        }
    }
    for (std::uint32_t k = 0; !rows && k < 3; ++k) {
        if (p[k].active) {
            finish_in_place_one(p[k]);
        }
    }
}

void finish_in_place_one(const InPlaceComp& p) noexcept
{
    if (p.w == 0) {
        return;
    }
    for (std::uint32_t j = 0; j < p.h; ++j) {
        finish_row(p.data + std::size_t(j) * p.buf_w, p.w, p.f);
    }
}

}  // namespace nj2k::detail
