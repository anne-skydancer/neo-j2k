// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The fused end of a tile's decode (output.h): the colour transform, the DC shift and the copy into
// the image in one pass. The per-sample operations are those of mct.cpp, in the same order.
#include "output.h"

#include <cstring>

#include <neoj2k/neoj2k.h>

#include "mct.h"

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nj2k::detail {

namespace {

inline float as_float(std::int32_t v) noexcept
{
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

inline std::int32_t as_int(float f) noexcept
{
    std::int32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

inline std::int32_t wrap_add(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

inline std::int32_t wrap_sub(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// a row is finished and stored this many samples at a time (stack buffers of 4 x 512 bytes)
constexpr std::size_t kChunk = 128;

}  // namespace

namespace kernels {

// mct.cpp's rct_scalar and ict_scalar, out of place (an output may be its input)
void mct_rows_scalar(Mct mct, const std::int32_t* a, const std::int32_t* b, const std::int32_t* c, std::int32_t* oa,
                     std::int32_t* ob, std::int32_t* oc, std::size_t n) noexcept
{
    if (mct == Mct::kRct) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::int32_t y = a[i], u = b[i], v = c[i];
            const std::int32_t g = wrap_sub(y, wrap_add(u, v) >> 2);
            oa[i] = wrap_add(v, g);
            ob[i] = g;
            oc[i] = wrap_add(u, g);
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            const float y = as_float(a[i]), u = as_float(b[i]), v = as_float(c[i]);
            const float r = y + (v * 1.402f);
            const float g = y - (u * 0.34413f) - (v * (0.71414f));
            const float bl = y + (u * 1.772f);
            oa[i] = as_int(r);
            ob[i] = as_int(g);
            oc[i] = as_int(bl);
        }
    }
}

// mct.cpp's dc_shift_53 and dc_shift_97 on one row
void finish_row_scalar(std::int32_t* p, std::size_t n, const Finish& f) noexcept
{
    if (f.reversible) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::int64_t v = std::int64_t(p[i]) + f.shift;
            p[i] = v < f.lo ? f.lo : (v > f.hi ? f.hi : static_cast<std::int32_t>(v));
        }
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
            std::memcpy(dst, row, n * sizeof(std::int32_t));
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
    std::size_t i = 0;
    if (mct == Mct::kRct) {
        for (; i + 8 <= n; i += 8) {
            const __m256i y = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
            const __m256i u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(c + i));
            const __m256i g = _mm256_sub_epi32(y, _mm256_srai_epi32(_mm256_add_epi32(u, v), 2));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(oa + i), _mm256_add_epi32(v, g));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(ob + i), g);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(oc + i), _mm256_add_epi32(u, g));
        }
    } else {
        const __m256 vrv = _mm256_set1_ps(1.402f), vgu = _mm256_set1_ps(0.34413f), vgv = _mm256_set1_ps(0.71414f),
                     vbu = _mm256_set1_ps(1.772f);
        for (; i + 8 <= n; i += 8) {
            const __m256 y = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i)));
            const __m256 u = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i)));
            const __m256 v = _mm256_castsi256_ps(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(c + i)));
            const __m256 r = _mm256_add_ps(y, _mm256_mul_ps(v, vrv));
            const __m256 g = _mm256_sub_ps(_mm256_sub_ps(y, _mm256_mul_ps(u, vgu)), _mm256_mul_ps(v, vgv));
            const __m256 bl = _mm256_add_ps(y, _mm256_mul_ps(u, vbu));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(oa + i), _mm256_castps_si256(r));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(ob + i), _mm256_castps_si256(g));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(oc + i), _mm256_castps_si256(bl));
        }
    }
    mct_rows_scalar(mct, a + i, b + i, c + i, oa + i, ob + i, oc + i, n - i);
}

// The 5/3 shift as a clamp to [lo - shift, hi - shift] then the add: the same result as the 64-bit
// add and clamp, in int32 lanes (OpenJPEG's shifts keep both bounds in range: prec <= 31)
void finish_row_avx2(std::int32_t* p, std::size_t n, const Finish& f) noexcept
{
    if (!f.reversible) {
        dc_shift_97_avx2(p, n, f.shift, f.lo, f.hi);
        return;
    }
    const std::int64_t lo = std::int64_t(f.lo) - f.shift, hi = std::int64_t(f.hi) - f.shift;
    std::size_t i = 0;
    if (lo >= INT32_MIN && hi <= INT32_MAX) {
        const __m256i vlo = _mm256_set1_epi32(static_cast<std::int32_t>(lo));
        const __m256i vhi = _mm256_set1_epi32(static_cast<std::int32_t>(hi));
        const __m256i vs = _mm256_set1_epi32(f.shift);
        for (; i + 8 <= n; i += 8) {
            __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
            v = _mm256_add_epi32(_mm256_min_epi32(_mm256_max_epi32(v, vlo), vhi), vs);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(p + i), v);
        }
    }
    finish_row_scalar(p + i, n - i, f);
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
    alignas(32) std::int32_t tmp[4][kChunk];
    const std::int32_t* rows[4] = {tmp[0], tmp[1], tmp[2], tmp[3]};
    for (std::uint32_t j = 0; j < c0.h; ++j) {
        for (std::uint32_t x = 0; x < c0.w; x += static_cast<std::uint32_t>(kChunk)) {
            const std::size_t n = c0.w - x < kChunk ? c0.w - x : kChunk;
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
            const std::int64_t y = static_cast<std::int64_t>(c0.y) + j;
            const std::size_t xs = std::size_t(c0.x) + x;
            if (packed) {
                store_packed(rows, k, n, static_cast<std::uint8_t*>(only[0]->data) + y * only[0]->row_stride + xs * k);
                continue;
            }
            for (c = 0; c < k; ++c) {
                for (std::uint32_t d = 0; d < numplanes; ++d) {
                    const nj2k_plane_t& pl = planes[d];
                    if (pl.component == comps[c].component) {
                        store(tmp[c], n, type, static_cast<std::uint8_t*>(pl.data) + y * pl.row_stride + xs * pl.sample_stride,
                              pl.sample_stride);
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
