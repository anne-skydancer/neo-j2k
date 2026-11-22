// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Tier-1 decoding of one code-block: the MQ and raw (BYPASS) decoders and the three coding passes,
// ported from OpenJPEG 2.5.4 (t1.c, mqc.c, mqc_inl.h, t1_luts.h; their notice is in t1.cpp), so the
// coefficients are OpenJPEG's; the MQ decision and the cleanup pass's run of AGG decisions are
// reformulated (t1_mq.h). The interface is neo-j2k's: one code-block's data chunks and segments in,
// its coefficients in the decoder's scratch, then written
// into the tile-component's buffer with dequantisation (and the ROI shift) as OpenJPEG does.
// The code-block's bytes are always copied into the decoder's padded buffer first, so the MQ
// decoder's 0xFFFF sentinel is never written into the caller's bytes (plan P4).
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

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

// The MQ coders' renormalisation shifts for a in [1, 0xffff]: until a reaches 0x8000 (15 - floor(log2 a);
// 0 when bit 15 is set), with the instructions of the build's floor: lzcnt in the AVX2 build (x86-64-v3
// has it), bsr in the scalar one (MSVC's std::countl_zero would pick lzcnt at run time, which the scalar
// build's object check refuses). The decoder (t1.cpp) and the encoder (enc_mq.h) share it.
inline std::uint32_t mq_shifts(std::uint32_t a) noexcept
{
#if defined(_MSC_VER) && !defined(__clang__)
#if defined(__AVX2__)
    return _lzcnt_u32(a) - 16U;
#else
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - static_cast<std::uint32_t>(msb);
#endif
#else
    return static_cast<std::uint32_t>(__builtin_clz(a)) - 16U;
#endif
}

// One MQ state of the standard's Table C.2 (OpenJPEG's opj_mqc_state_t), the 94 of kMqcStates (t1_tables.h): the encoder
// reads them, and the decoder's context words (t1_mq.h) are built from them at compile time.
struct MqState {
    std::uint32_t qeval;
    std::uint32_t mps;
    const MqState* nmps;
    const MqState* nlps;
};

// The 19 contexts' states, one 32-bit word each (t1_mq.h has the layout and the table of the 94 words).
struct MqContexts {
    std::uint32_t slot[19];
};

// The MQ decoder (opj_mqc_t, decoding side): the registers, the input and the contexts. curctx is the slot a pass
// decides on while it runs, kept here between the passes' loops; the macros of t1_mq.h work on locals.
struct Mqc {
    std::uint32_t c, a, ct;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* end;
    MqContexts ctx;
    std::uint32_t* curctx;
    const std::uint8_t* lut_ctxno_zc_orient;
    std::uint8_t backup[2];
};

// A decoder's Tier-1 scratch, kept between code-blocks and decodes.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // padded to a whole cache line, as intended (a part's scratch shares none with the next part's)
#endif
struct alignas(64) T1 {
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
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Decodes the code-block into t1.data (row-major, t1.w x t1.h), as opj_t1_decode_cblk; false when
// OpenJPEG refuses it (bit-planes plus ROI shift at or above 31), after the same warning. The
// chunks' total must fit t1.reserve(). The segments' lengths are meant to add up to no more than the
// chunks' total (Tier-2 makes them so); where they do not, a segment is cut to the bytes that are there.
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
