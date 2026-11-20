// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Tier-1 decoding of one code-block: the MQ and raw (BYPASS) decoders and the three coding passes,
// ported from OpenJPEG 2.5.4 (t1.c, mqc.c, mqc_inl.h, t1_luts.h; their notice is in t1.cpp) with
// the same operations, so the coefficients are OpenJPEG's. The interface is neo-j2k's: one
// code-block's data chunks and segments in, its coefficients in the decoder's scratch, then written
// into the tile-component's buffer with dequantisation (and the ROI shift) as OpenJPEG does.
// The code-block's bytes are always copied into the decoder's padded buffer first, so the MQ
// decoder's 0xFFFF sentinel is never written into the caller's bytes (plan P4).
#pragma once

#include <cstddef>
#include <cstdint>

#include "memory.h"
#include "messages.h"

namespace nj2k::detail {

inline constexpr std::uint32_t kCblkLazy = 0x01, kCblkReset = 0x02, kCblkTermall = 0x04, kCblkVsc = 0x08,
                               kCblkPterm = 0x10, kCblkSegsym = 0x20;

// The bytes of one contribution (a codeword segment's part from one packet).
struct Chunk {
    const std::uint8_t* data;
    std::uint32_t len;
};

// A codeword segment of a code-block (opj_tcd_seg_t, decoding side). Tier-1 reads len (the bytes
// with data) and real_num_passes (the passes with data); the others are Tier-2's state.
struct Segment {
    std::uint32_t len;              // bytes with data
    std::uint32_t numpasses;        // passes the packet headers gave it
    std::uint32_t real_num_passes;  // passes with data
    std::uint32_t maxpasses;
    std::uint32_t numnewpasses;     // of the packet being read
    std::uint32_t newlen;
};

// What Tier-1 needs of a code-block.
struct T1Block {
    std::uint32_t w, h;      // x1 - x0, y1 - y0 (at most 1024 each, 4096 samples)
    std::uint32_t numbps;    // from its packet headers (0 when never included)
    std::uint32_t orient;    // the band: 0 LL, 1 HL, 2 LH, 3 HH
    std::uint32_t roishift;
    std::uint32_t cblksty;
    const Chunk* chunks;
    std::uint32_t nchunks;
    const Segment* segs;     // the segments with data, real_num_segs of them
    std::uint32_t nsegs;
};

// The MQ decoder state (opj_mqc_t, decoding side).
struct MqState {
    std::uint32_t qeval;
    std::uint32_t mps;
    const MqState* nmps;
    const MqState* nlps;
};

struct Mqc {
    std::uint32_t c, a, ct;
    std::uint32_t end_of_byte_stream_counter;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* end;
    const MqState* ctxs[19];
    const MqState** curctx;
    const std::uint8_t* lut_ctxno_zc_orient;
    std::uint8_t backup[2];
};

// A decoder's Tier-1 scratch, kept between code-blocks and decodes.
struct T1 {
    Mqc mqc;
    std::int32_t* data = nullptr;    // w x h coefficients of the current code-block
    std::uint32_t* flags = nullptr;  // (h + 3) / 4 + 2 rows of w + 2
    std::uint32_t w = 0, h = 0;
    std::uint8_t* cblkdata = nullptr;  // the code-block's bytes and 2 bytes of padding
    std::size_t cblkdata_size = 0;

    static constexpr std::size_t kMaxData = 4096;   // samples of the largest code-block
    static constexpr std::size_t kMaxFlags = 3078;  // (1024 + 2) x (4 / 4 + 2), the largest flag area

    // The fixed buffers (data, flags), once; false when the allocator refuses.
    bool ready(const Allocator& alloc) noexcept;
    // Room for a code-block of n bytes; false when the allocator refuses.
    bool reserve(const Allocator& alloc, std::size_t n) noexcept;
    void release(const Allocator& alloc) noexcept;
    std::size_t bytes() const noexcept;
};

// Decodes the code-block into t1.data (row-major, t1.w x t1.h), as opj_t1_decode_cblk; false when
// OpenJPEG refuses it (bit-planes plus ROI shift at or above 31), after the same warning. The
// chunks' total must fit t1.reserve().
bool t1_decode_cblk(T1& t1, const T1Block& b, const Messages& msg) noexcept;

// The bit-plane check of opj_t1_decode_cblk alone, for a code-block without data (whose decode
// gives zeros): false with the warning when OpenJPEG would refuse it.
bool t1_empty_cblk_ok(const T1Block& b, const Messages& msg) noexcept;

// Writes t1.data (w x h) into a tile-component buffer at dst (row stride in samples), as
// opj_t1_clbl_decode_processor does for whole-tile decoding: the ROI shift undone, then 5/3
// coefficients halved (towards zero), 9/7 ones multiplied by 0.5 * stepsize as floats (their bits
// stored in the int32 buffer). A null src writes zeros (a code-block without data).
void t1_store(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::uint32_t roishift, bool reversible,
              float stepsize, std::int32_t* dst, std::size_t stride) noexcept;

#if defined(NJ2K_T1_TRACE)
// The trace of the MQ prototype bench (t1.cpp): each MQ segment (its bytes, which the decoder ends
// with its 0xFF 0xFF sentinel, the 19 contexts' state indices, whether contexts reset between
// passes) and each decision (its context, its symbol). Never built into the library.
struct T1Trace {
    void* user;
    void (*segment)(void* user, const std::uint8_t* data, std::uint32_t len, const std::uint8_t* states, bool reset);
    void (*decision)(void* user, std::uint32_t ctx, std::uint32_t d);
};
extern T1Trace g_t1_trace;
// OpenJPEG's 94 MQ states, in order (the state indices above)
const MqState* t1_trace_states() noexcept;
#endif

namespace kernels {
// Scalar references of the store kernels, compiled in every build; the library uses the AVX2 ones
// when built for x86-64-v3. The kernel tests compare both bit for bit.
void store53_scalar(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::int32_t* dst,
                    std::size_t stride) noexcept;
void store97_scalar(const std::int32_t* src, std::uint32_t w, std::uint32_t h, float half_step, std::int32_t* dst,
                    std::size_t stride) noexcept;
#if defined(__AVX2__)
void store53_avx2(const std::int32_t* src, std::uint32_t w, std::uint32_t h, std::int32_t* dst,
                  std::size_t stride) noexcept;
void store97_avx2(const std::int32_t* src, std::uint32_t w, std::uint32_t h, float half_step, std::int32_t* dst,
                  std::size_t stride) noexcept;
#endif
}  // namespace kernels

}  // namespace nj2k::detail
