// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// EBCOT-NG study, encoder: src/enc_t1.cpp's Tier-1 (OpenJPEG 2.5.4's passes, the viewer's modes) rewritten
// on the stage-2 MQ coder of doc/enc_t1_study.md (clz renormalisation, byte-index states, MPS/LPS by
// select), the coder held in a local for each pass. Included by enc_v.cpp inside namespace nj2k::ENG_NS with
// the switches below (0 or 1, default 0). With all switches 0 this is the control (E0): the library's
// operations in the library's order; every other switch changes one thing.
//   E_MODEL     the MQ coder replaced by a sink (the modelling alone; bytes and rates meaningless)
//   E_NO_NMSE   no distortion sums (ablation; bytes unchanged, distortions not)
//   E_NO_CTX    every context a constant (ablation; control flow unchanged, bytes not)
//   E_DUP_UPD   the neighbours' flag update done twice (idempotent; its cost measured by the difference)
//   E_COUNT     the tallies of common.h
//   E_PASSCLK   TSC cycles a pass type into g_pass_cycles, the stage clock into g_stage_cycles
//   E_MAGREG    refinement pass: the three magnitude contexts' states held in registers (state words)
//   E_REF_MASK  refinement pass: the refined samples of a column as a mask, iterated by tzcnt
//   E_SPP_MASK  significance pass: the eligible samples of a column as a mask, recomputed after each new
//               significant sample
//   E_CLN_MASK  cleanup pass outside run mode: the samples to code of a column as a mask (fixed for the column)
//   E_SIMD_PRE  sign-magnitude conversion and the block maximum with AVX2
//   E_BRFREE    the flag update's north/south parts branch-free when the row is a run-time value (masks)
//   E_SPLIT     EBCOT-NG: a pass generates its (context, bit) pairs without data-dependent branches into a
//               buffer, then the MQ coder codes the pass's pairs in one loop; a bit mask of the passes split
//               (1 significance, 2 refinement, 4 cleanup; 7 all), the others fused as above
//   E_SPP_SKIP  split significance pass: a column skipped when no row is eligible at its start
//   E_PRE_BRFREE sign-magnitude conversion and maximum branch-free in scalar code (no intrinsics)
#ifndef E_MODEL
#define E_MODEL 0
#endif
#ifndef E_NO_NMSE
#define E_NO_NMSE 0
#endif
#ifndef E_NO_CTX
#define E_NO_CTX 0
#endif
#ifndef E_DUP_UPD
#define E_DUP_UPD 0
#endif
#ifndef E_COUNT
#define E_COUNT 0
#endif
#ifndef E_PASSCLK
#define E_PASSCLK 0
#endif
#ifndef E_MAGREG
#define E_MAGREG 0
#endif
#ifndef E_REF_MASK
#define E_REF_MASK 0
#endif
#ifndef E_SPP_MASK
#define E_SPP_MASK 0
#endif
#ifndef E_CLN_MASK
#define E_CLN_MASK 0
#endif
#ifndef E_SIMD_PRE
#define E_SIMD_PRE 0
#endif
#ifndef E_BRFREE
#define E_BRFREE 0
#endif
#ifndef E_SPLIT
#define E_SPLIT 0
#endif
#ifndef E_SPP_SKIP
#define E_SPP_SKIP 0
#endif
#ifndef E_PRE_BRFREE
#define E_PRE_BRFREE 0
#endif

#if E_COUNT
#define CNT(x) (x)
#define SITE(k, t) ebc::site((k), (t))
#else
#define CNT(x) ((void)0)
#define SITE(k, t) ((void)0)
#endif

using namespace nj2k::detail;
using ebc::u8;
using ebc::u32;
using ebc::i32;
using ebc::u64;

namespace {

constexpr u32 kCblkW = 64, kCblkH = 64;
constexpr u32 kMaxSamples = kCblkW * kCblkH;
constexpr u32 kFlagStride = kCblkW + 2;
constexpr u32 kFlagCount = kFlagStride * ((kCblkH + 3) / 4 + 2);
constexpr u32 kTermMargin = 3U * (3U * (31U - T1_NMSEDEC_FRACBITS) - 2U);
constexpr u32 kOutCapacity = 74 + kTermMargin + kMaxSamples * 4;
constexpr u32 kMaxPasses = 3 * (31 - T1_NMSEDEC_FRACBITS);
constexpr u32 kPiAll = T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3;
constexpr u32 kSigAll = T1_SIGMA_4 | T1_SIGMA_7 | T1_SIGMA_10 | T1_SIGMA_13;
constexpr u32 kPairBuf = 3 * kMaxSamples + 64;  // a pass codes at most 2.5 decisions a sample

__forceinline u32 smr_abs(i32 x) noexcept { return static_cast<u32>(x) & 0x7FFFFFFFU; }
__forceinline u32 smr_sign(i32 x) noexcept { return static_cast<u32>(x) >> 31; }

int floorlog2(i32 a) noexcept
{
    int l;
    for (l = 0; a > 1; ++l) {
        a >>= 1;
    }
    return l;
}

// the magnitude contexts' states as 64-bit words: the decoder's state word (qe, MPS, successors) and the
// state's own index in the high half, so a pass can hold them in registers and write the index back
struct ExtWords {
    u64 w[94];
};
constexpr ExtWords make_ext()
{
    ExtWords t{};
    for (int i = 0; i < 94; ++i) {
        t.w[i] = u64(ebc::kWords.w[i]) | (u64(i) << 32);
    }
    return t;
}
constexpr ExtWords kExt = make_ext();

// the stage-2 MQ encoder (doc/enc_t1_study.md section 4), bytes and pass ends exactly OpenJPEG's
struct Mq {
    u32 c, a, ct;
    u8* bp;
    u8* start;
    u8* limit;
    bool overflow;
    u32 sink;
    u8 ctxs[19];

    void resetstates() noexcept
    {
        for (u32 i = 0; i < 19; i++) {
            ctxs[i] = 0;
        }
    }
    void setstate(u32 ctxno, u32 msb, i32 prob) noexcept { ctxs[ctxno] = static_cast<u8>(msb + static_cast<u32>(prob << 1)); }
    void init_enc(u8* buf, u8* last) noexcept
    {
        a = 0x8000;
        c = 0;
        bp = buf - 1;
        ct = 12;
        start = buf;
        limit = last;
        overflow = false;
        sink = 0;
    }
    u32 numbytes() const noexcept { return static_cast<u32>(bp - start); }

    void byteout() noexcept
    {
        if (bp >= limit) {
            overflow = true;
            bp = limit - 1;
        }
        if (*bp == 0xff) {
            bp++;
            *bp = static_cast<u8>(c >> 20);
            c &= 0xfffff;
            ct = 7;
        } else if ((c & 0x8000000) == 0) {
            bp++;
            *bp = static_cast<u8>(c >> 19);
            c &= 0x7ffff;
            ct = 8;
        } else {
            (*bp)++;
            if (*bp == 0xff) {
                c &= 0x7ffffff;
                bp++;
                *bp = static_cast<u8>(c >> 20);
                c &= 0xfffff;
                ct = 7;
            } else {
                bp++;
                *bp = static_cast<u8>(c >> 19);
                c &= 0x7ffff;
                ct = 8;
            }
        }
    }
    __forceinline void renorme() noexcept
    {
        u32 k = ebc::mq_shifts(a);
        a <<= k;
        while (k >= ct) {
            c <<= ct;
            k -= ct;
            byteout();
        }
        c <<= k;
        ct -= k;
    }
    // one decision in context cx
    __forceinline void encode(u32 cx, u32 d) noexcept
    {
        CNT(++ebc::g_counts.dec_ctx[cx]);
#if E_MODEL
        sink += (cx << 1) ^ d;
#else
        const u32 i = ctxs[cx];
        const ebc::Packed e = ebc::kPacked.t[i];
        const u32 q = e.qe;
        const u32 lps = (i ^ d) & 1U;
        const u32 an = a - q;
        const u32 swap = static_cast<u32>(an < q) ^ lps;
        a = swap ? q : an;
        c += swap ? 0U : q;
        const u32 keep = (lps | (an & 0x8000U)) == 0x8000U ? 1U : 0U;
        const u32 next = lps ? e.nlps : e.nmps;
        ctxs[cx] = static_cast<u8>(keep ? i : next);
        renorme();
#endif
    }
    // one decision on a state held in a register (E_MAGREG): the same function on the word form
    __forceinline u64 encode_w(u64 r, u32 d) noexcept
    {
#if E_MODEL
        sink += static_cast<u32>(r) ^ d;
        return r;
#else
        const u32 w = static_cast<u32>(r);
        const u32 q = w & 0xFFFFU;
        const u32 lps = ((w >> 16) ^ d) & 1U;
        const u32 an = a - q;
        const u32 swap = static_cast<u32>(an < q) ^ lps;
        a = swap ? q : an;
        c += swap ? 0U : q;
        const u32 keep = (lps | (an & 0x8000U)) == 0x8000U ? 1U : 0U;
        const u32 n = lps ? (w >> 24) : ((w >> 17) & 0x7FU);
        const u64 nr = kExt.w[n];
        renorme();
        return keep ? r : nr;
#endif
    }
    void flush() noexcept
    {
        const u32 tempc = c + a;
        c |= 0xffff;
        if (c >= tempc) {
            c -= 0x8000;
        }
        c <<= ct;
        byteout();
        c <<= ct;
        byteout();
        if (*bp != 0xff) {
            bp++;
        }
    }
};

}  // namespace

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct EncT1 {
    Mq mqc;
    u32 w, h;
    const u8* lutzc;
    alignas(64) i32 data[kMaxSamples];
    alignas(64) u32 flags[kFlagCount];
    EncPass passes[kMaxPasses];
    u8 out[1 + kOutCapacity];
#if E_SPLIT
    alignas(64) u8 pairs[kPairBuf];
#endif
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

__forceinline u32 zc_ctx(const EncT1* t1, u32 f) noexcept
{
    CNT(++ebc::g_counts.lut_loads);
#if E_NO_CTX
    (void)t1;
    (void)f;
    return 0;
#else
    return t1->lutzc[f & T1_SIGMA_NEIGHBOURS];
#endif
}

__forceinline u32 sc_index(u32 fX, u32 pfX, u32 nfX, u32 ci) noexcept
{
    u32 lu = (fX >> (ci * 3U)) & (T1_SIGMA_1 | T1_SIGMA_3 | T1_SIGMA_5 | T1_SIGMA_7);
    lu |= (pfX >> (T1_CHI_THIS_I + (ci * 3U))) & (1U << 0);
    lu |= (nfX >> (T1_CHI_THIS_I - 2U + (ci * 3U))) & (1U << 2);
    if (ci == 0U) {
        lu |= (fX >> (T1_CHI_0_I - 4U)) & (1U << 4);
    } else {
        lu |= (fX >> (T1_CHI_1_I - 4U + ((ci - 1U) * 3U))) & (1U << 4);
    }
    lu |= (fX >> (T1_CHI_2_I - 6U + (ci * 3U))) & (1U << 6);
    return lu;
}

__forceinline u32 mag_ctx(u32 f) noexcept
{
#if E_NO_CTX
    (void)f;
    return T1_CTXNO_MAG;
#else
    const u32 tmp = (f & T1_SIGMA_NEIGHBOURS) ? T1_CTXNO_MAG + 1 : T1_CTXNO_MAG;
    return (f & T1_MU_0) ? T1_CTXNO_MAG + 2 : tmp;
#endif
}

__forceinline i32 nmse_sig(u32 x, u32 bitpos) noexcept
{
    CNT(++ebc::g_counts.nmse_loads);
    if (bitpos > 0) {
        return kNmsedecSig[(x >> bitpos) & ((1 << T1_NMSEDEC_BITS) - 1)];
    }
    return kNmsedecSig0[x & ((1 << T1_NMSEDEC_BITS) - 1)];
}
__forceinline i32 nmse_ref(u32 x, u32 bitpos) noexcept
{
    CNT(++ebc::g_counts.nmse_loads);
    if (bitpos > 0) {
        return kNmsedecRef[(x >> bitpos) & ((1 << T1_NMSEDEC_BITS) - 1)];
    }
    return kNmsedecRef0[x & ((1 << T1_NMSEDEC_BITS) - 1)];
}
#if E_NO_NMSE
#define NMSE_ADD(acc, val) ((void)0)
#else
#define NMSE_ADD(acc, val) ((acc) += (val))
#endif

// opj_t1_update_flags with vsc = 0, ci known at compile time in the unrolled steps
__forceinline void update_flags_once(u32* flagsp, u32 ci, u32 s, u32 stride) noexcept
{
    CNT(ebc::g_counts.upd_rmw += (ci == 0 || ci == 3) ? 6 : 3);
    flagsp[-1] |= T1_SIGMA_5 << (3U * ci);
    *flagsp |= ((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci);
    flagsp[1] |= T1_SIGMA_3 << (3U * ci);
#if E_BRFREE
    {
        // both border rows' updates always written, masked to nothing unless the row is 0 or 3
        const u32 mn = 0U - static_cast<u32>(ci == 0U), ms = 0U - static_cast<u32>(ci == 3U);
        u32* north = flagsp - stride;
        *north |= ((s << T1_CHI_5_I) | T1_SIGMA_16) & mn;
        north[-1] |= T1_SIGMA_17 & mn;
        north[1] |= T1_SIGMA_15 & mn;
        u32* south = flagsp + stride;
        *south |= ((s << T1_CHI_0_I) | T1_SIGMA_1) & ms;
        south[-1] |= T1_SIGMA_2 & ms;
        south[1] |= T1_SIGMA_0 & ms;
    }
#else
    if (ci == 0U) {
        u32* north = flagsp - stride;
        *north |= (s << T1_CHI_5_I) | T1_SIGMA_16;
        north[-1] |= T1_SIGMA_17;
        north[1] |= T1_SIGMA_15;
    }
    if (ci == 3U) {
        u32* south = flagsp + stride;
        *south |= (s << T1_CHI_0_I) | T1_SIGMA_1;
        south[-1] |= T1_SIGMA_2;
        south[1] |= T1_SIGMA_0;
    }
#endif
}
__forceinline void update_flags(u32* flagsp, u32 ci, u32 s, u32 stride) noexcept
{
    CNT(++ebc::g_counts.sig_events);
    update_flags_once(flagsp, ci, s, stride);
#if E_DUP_UPD
    _ReadWriteBarrier();
    update_flags_once(flagsp, ci, s, stride);
    _ReadWriteBarrier();
#endif
}

// the samples of a column word that the significance pass codes: insignificant, unvisited, with a
// significant neighbour; bit 3 ci for row ci (the 3x3 window ORed by shifts; the centre is excluded by the
// insignificance test)
__forceinline u32 spp_elig(u32 f) noexcept
{
    const u32 g = f | (f >> 1) | (f >> 2);
    const u32 h = g | (g >> 3) | (g >> 6);
    return h & ~(f >> 4) & ~(f >> 21) & 0x249U;
}

// ---- significance pass ----
__forceinline void sig_step(EncT1* t1, Mq& m, u32* flagsp, const i32* datap, i32 bpno, i32 one, i32& nmsedec,
                            u32 ci) noexcept
{
    const u32 flags = *flagsp;
    CNT(++ebc::g_counts.tested[0]);
    const bool elig = (flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U &&
                      (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U;
    SITE(ebc::kSppElig, elig);
    if (elig) {
        const u32 ctxt1 = zc_ctx(t1, flags >> (ci * 3U));
        CNT(++ebc::g_counts.data_loads);
        u32 v = (smr_abs(*datap) & static_cast<u32>(one)) ? 1 : 0;
        m.encode(ctxt1, v);
        CNT(++ebc::g_counts.dec[0]);
        SITE(ebc::kSppBit, v);
        if (v) {
            const u32 lu = sc_index(*flagsp, flagsp[-1], flagsp[1], ci);
            CNT(ebc::g_counts.lut_loads += 2);
            const u32 ctxt2 = E_NO_CTX ? T1_CTXNO_SC : kLutCtxnoSc[lu];
            v = smr_sign(*datap);
            NMSE_ADD(nmsedec, nmse_sig(smr_abs(*datap), static_cast<u32>(bpno)));
            const u32 spb = E_NO_CTX ? 0 : kLutSpb[lu];
            m.encode(ctxt2, v ^ spb);
            CNT((++ebc::g_counts.dec[0], ++ebc::g_counts.sc[0]));
            update_flags(flagsp, ci, v, t1->w + 2);
        }
        *flagsp |= T1_PI_THIS << (ci * 3U);
    }
}

void enc_sigpass(EncT1* t1, Mq& m, i32 bpno, i32& nmsedec) noexcept
{
    u32 i, k;
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    const i32* datap = t1->data;
    nmsedec = 0;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        const u32 w = t1->w;
        for (i = 0; i < w; ++i, ++f, datap += 4) {
            CNT(++ebc::g_counts.cols[0]);
            SITE(ebc::kSppCol, *f != 0U);
            if (*f == 0U) {
                continue;
            }
            CNT(++ebc::g_counts.cols_work[0]);
#if E_SPP_MASK
            u32 e = spp_elig(*f);
            while (e) {
                const u32 b = ebc::ctz32(e);
                const u32 ci = (b * 11U) >> 5;
                CNT(++ebc::g_counts.tested[0]);
                const u32 ctxt1 = zc_ctx(t1, *f >> b);
                u32 v = (smr_abs(datap[ci]) & static_cast<u32>(one)) ? 1 : 0;
                m.encode(ctxt1, v);
                CNT(++ebc::g_counts.dec[0]);
                if (v) {
                    const u32 lu = sc_index(*f, f[-1], f[1], ci);
                    const u32 ctxt2 = kLutCtxnoSc[lu];
                    v = smr_sign(datap[ci]);
                    NMSE_ADD(nmsedec, nmse_sig(smr_abs(datap[ci]), static_cast<u32>(bpno)));
                    m.encode(ctxt2, v ^ kLutSpb[lu]);
                    CNT((++ebc::g_counts.dec[0], ++ebc::g_counts.sc[0]));
                    update_flags(f, ci, v, stride);
                    *f |= T1_PI_THIS << b;
                    e = spp_elig(*f) & ~((2U << b) - 1U);
                } else {
                    *f |= T1_PI_THIS << b;
                    e &= e - 1U;
                }
            }
#else
            sig_step(t1, m, f, &datap[0], bpno, one, nmsedec, 0);
            sig_step(t1, m, f, &datap[1], bpno, one, nmsedec, 1);
            sig_step(t1, m, f, &datap[2], bpno, one, nmsedec, 2);
            sig_step(t1, m, f, &datap[3], bpno, one, nmsedec, 3);
#endif
        }
    }
    if (k < t1->h) {
        for (i = 0; i < t1->w; ++i, ++f) {
            if (*f == 0U) {
                datap += (t1->h - k);
                continue;
            }
            for (u32 j = k; j < t1->h; ++j, ++datap) {
                sig_step(t1, m, f, &datap[0], bpno, one, nmsedec, j - k);
            }
        }
    }
}

// ---- refinement pass ----
__forceinline void ref_step(Mq& m, u32 flags, u32& flagsUpdated, const i32* datap, i32 bpno, i32 one, i32& nmsedec,
                            u32 ci) noexcept
{
    CNT(++ebc::g_counts.tested[1]);
    const bool elig = (flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == (T1_SIGMA_THIS << (ci * 3U));
    SITE(ebc::kRefElig, elig);
    if (elig) {
        const u32 ctxt = mag_ctx(flags >> (ci * 3U));
        CNT(++ebc::g_counts.data_loads);
        const u32 abs_data = smr_abs(*datap);
        NMSE_ADD(nmsedec, nmse_ref(abs_data, static_cast<u32>(bpno)));
        const u32 v = (static_cast<i32>(abs_data) & one) ? 1 : 0;
        m.encode(ctxt, v);
        CNT(++ebc::g_counts.dec[1]);
        flagsUpdated |= T1_MU_THIS << (ci * 3U);
    }
}

void enc_refpass(EncT1* t1, Mq& m, i32 bpno, i32& nmsedec) noexcept
{
    u32 i, k;
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    const i32* datap = t1->data;
    nmsedec = 0;
#if E_MAGREG
    u64 r14 = kExt.w[m.ctxs[14]], r15 = kExt.w[m.ctxs[15]], r16 = kExt.w[m.ctxs[16]];
#endif
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        for (i = 0; i < t1->w; ++i, f++, datap += 4) {
            const u32 flags = *f;
            CNT(++ebc::g_counts.cols[1]);
#if E_REF_MASK
            u32 mk = flags & ~(flags >> 17) & 0x2490U;
            SITE(ebc::kRefCol, mk != 0);
            if (mk == 0) {
                continue;
            }
            CNT(++ebc::g_counts.cols_work[1]);
            *f = flags | (mk << 16);
            do {
                const u32 b = ebc::ctz32(mk);
                const u32 s = b - 4U;
                const u32 ci = (s * 11U) >> 5;
                mk &= mk - 1U;
                CNT(++ebc::g_counts.tested[1]);
                const u32 ctxt = mag_ctx(flags >> s);
                const u32 abs_data = smr_abs(datap[ci]);
                NMSE_ADD(nmsedec, nmse_ref(abs_data, static_cast<u32>(bpno)));
                const u32 v = (static_cast<i32>(abs_data) & one) ? 1 : 0;
                CNT(++ebc::g_counts.dec[1]);
#if E_MAGREG
                const u64 r = ctxt == 16 ? r16 : ctxt == 15 ? r15 : r14;
                const u64 nr = m.encode_w(r, v);
                r14 = ctxt == 14 ? nr : r14;
                r15 = ctxt == 15 ? nr : r15;
                r16 = ctxt == 16 ? nr : r16;
#else
                m.encode(ctxt, v);
#endif
            } while (mk);
#else
            u32 flagsUpdated = flags;
            const bool work = (flags & kSigAll) != 0 && (flags & kPiAll) != kPiAll;
            SITE(ebc::kRefCol, work);
            if ((flags & kSigAll) == 0) {
                continue;
            }
            if ((flags & kPiAll) == kPiAll) {
                continue;
            }
            CNT(++ebc::g_counts.cols_work[1]);
#if E_MAGREG
#define REF_STEP_R(ci)                                                                                           \
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << ((ci) * 3U))) == (T1_SIGMA_THIS << ((ci) * 3U))) {            \
        const u32 ctxt = mag_ctx(flags >> ((ci) * 3U));                                                          \
        const u32 abs_data = smr_abs(datap[ci]);                                                                 \
        NMSE_ADD(nmsedec, nmse_ref(abs_data, static_cast<u32>(bpno)));                                           \
        const u32 v = (static_cast<i32>(abs_data) & one) ? 1 : 0;                                                \
        const u64 r = ctxt == 16 ? r16 : ctxt == 15 ? r15 : r14;                                                 \
        const u64 nr = m.encode_w(r, v);                                                                         \
        r14 = ctxt == 14 ? nr : r14;                                                                             \
        r15 = ctxt == 15 ? nr : r15;                                                                             \
        r16 = ctxt == 16 ? nr : r16;                                                                             \
        flagsUpdated |= T1_MU_THIS << ((ci) * 3U);                                                               \
    }
            REF_STEP_R(0)
            REF_STEP_R(1)
            REF_STEP_R(2)
            REF_STEP_R(3)
#undef REF_STEP_R
#else
            ref_step(m, flags, flagsUpdated, &datap[0], bpno, one, nmsedec, 0);
            ref_step(m, flags, flagsUpdated, &datap[1], bpno, one, nmsedec, 1);
            ref_step(m, flags, flagsUpdated, &datap[2], bpno, one, nmsedec, 2);
            ref_step(m, flags, flagsUpdated, &datap[3], bpno, one, nmsedec, 3);
#endif
            *f = flagsUpdated;
#endif
        }
    }
#if E_MAGREG
    m.ctxs[14] = static_cast<u8>(r14 >> 32);
    m.ctxs[15] = static_cast<u8>(r15 >> 32);
    m.ctxs[16] = static_cast<u8>(r16 >> 32);
#endif
    if (k < t1->h) {
        const u32 remaining_lines = t1->h - k;
        for (i = 0; i < t1->w; ++i, ++f) {
            if ((*f & kSigAll) == 0) {
                datap += remaining_lines;
                continue;
            }
            for (u32 j = 0; j < remaining_lines; j++, datap++) {
                ref_step(m, *f, *f, &datap[0], bpno, one, nmsedec, j);
            }
        }
    }
}

// ---- cleanup pass ----
__forceinline void cln_step(EncT1* t1, Mq& m, u32* flagsp, const i32* l_datap, i32 bpno, i32 one, i32& nmsedec,
                            u32 agg, u32 runlen, u32 lim) noexcept
{
    const u32 check = kSigAll | kPiAll;
    if ((*flagsp & check) == check) {
        if (runlen == 0) {
            *flagsp &= ~kPiAll;
        } else if (runlen == 1) {
            *flagsp &= ~(T1_PI_1 | T1_PI_2 | T1_PI_3);
        } else if (runlen == 2) {
            *flagsp &= ~(T1_PI_2 | T1_PI_3);
        } else if (runlen == 3) {
            *flagsp &= ~(T1_PI_3);
        }
        return;
    }
    for (u32 ci = runlen; ci < lim; ++ci) {
        bool partial = false;
        if ((agg != 0) && (ci == runlen)) {
            partial = true;
        } else {
            CNT(++ebc::g_counts.tested[2]);
            const bool elig = !(*flagsp & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)));
            SITE(ebc::kClnElig, elig);
            if (elig) {
                const u32 ctxt1 = zc_ctx(t1, *flagsp >> (ci * 3U));
                CNT(++ebc::g_counts.data_loads);
                const u32 v = (smr_abs(*l_datap) & static_cast<u32>(one)) ? 1 : 0;
                m.encode(ctxt1, v);
                CNT(++ebc::g_counts.dec[2]);
                SITE(ebc::kClnBit, v);
                if (v) {
                    partial = true;
                }
            }
        }
        if (partial) {
            const u32 lu = sc_index(*flagsp, flagsp[-1], flagsp[1], ci);
            CNT(ebc::g_counts.lut_loads += 2);
            NMSE_ADD(nmsedec, nmse_sig(smr_abs(*l_datap), static_cast<u32>(bpno)));
            const u32 ctxt2 = E_NO_CTX ? T1_CTXNO_SC : kLutCtxnoSc[lu];
            const u32 v = smr_sign(*l_datap);
            const u32 spb = E_NO_CTX ? 0 : kLutSpb[lu];
            m.encode(ctxt2, v ^ spb);
            CNT((++ebc::g_counts.dec[2], ++ebc::g_counts.sc[2]));
            update_flags(flagsp, ci, v, t1->w + 2U);
        }
        *flagsp &= ~(T1_PI_THIS << (3U * ci));
        l_datap++;
    }
}

void enc_clnpass(EncT1* t1, Mq& m, i32 bpno, i32& nmsedec) noexcept
{
    u32 i, k;
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const i32* datap = t1->data;
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    nmsedec = 0;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        for (i = 0; i < t1->w; ++i, f++) {
            u32 agg, runlen;
            agg = !*f;
            CNT(++ebc::g_counts.cols[2]);
            SITE(ebc::kClnAgg, agg);
            if (agg) {
                for (runlen = 0; runlen < 4; ++runlen, ++datap) {
                    CNT(++ebc::g_counts.data_loads);
                    if (smr_abs(*datap) & static_cast<u32>(one)) {
                        break;
                    }
                }
                m.encode(T1_CTXNO_AGG, runlen != 4);
                CNT((++ebc::g_counts.agg, ++ebc::g_counts.dec[2]));
                SITE(ebc::kClnAggBit, runlen != 4);
                if (runlen == 4) {
                    continue;
                }
                CNT(++ebc::g_counts.cols_work[2]);
                m.encode(T1_CTXNO_UNI, runlen >> 1);
                m.encode(T1_CTXNO_UNI, runlen & 1);
                CNT((ebc::g_counts.uni += 2, ebc::g_counts.dec[2] += 2));
            } else {
                runlen = 0;
                CNT(++ebc::g_counts.cols_work[2]);
#if E_CLN_MASK
                const u32 fl = *f;
                const u32 check = kSigAll | kPiAll;
                if ((fl & check) == check) {
                    *f = fl & ~kPiAll;
                } else {
                    u32 cand = ~((fl >> 4) | (fl >> 21)) & 0x249U;
                    while (cand) {
                        const u32 b = ebc::ctz32(cand);
                        const u32 ci = (b * 11U) >> 5;
                        cand &= cand - 1U;
                        CNT(++ebc::g_counts.tested[2]);
                        const u32 ctxt1 = zc_ctx(t1, *f >> b);
                        const u32 v = (smr_abs(datap[ci]) & static_cast<u32>(one)) ? 1 : 0;
                        m.encode(ctxt1, v);
                        CNT(++ebc::g_counts.dec[2]);
                        if (v) {
                            const u32 lu = sc_index(*f, f[-1], f[1], ci);
                            NMSE_ADD(nmsedec, nmse_sig(smr_abs(datap[ci]), static_cast<u32>(bpno)));
                            m.encode(kLutCtxnoSc[lu], smr_sign(datap[ci]) ^ kLutSpb[lu]);
                            CNT((++ebc::g_counts.dec[2], ++ebc::g_counts.sc[2]));
                            update_flags(f, ci, smr_sign(datap[ci]), stride);
                        }
                    }
                    *f &= ~kPiAll;
                }
                datap += 4;
                continue;
#endif
            }
            cln_step(t1, m, f, datap, bpno, one, nmsedec, agg, runlen, 4U);
            datap += 4 - runlen;
        }
    }
    if (k < t1->h) {
        for (i = 0; i < t1->w; ++i, f++) {
            cln_step(t1, m, f, datap, bpno, one, nmsedec, 0, 0, t1->h - k);
            datap += t1->h - k;
        }
    }
}

#if E_SPLIT
// ---- EBCOT-NG encoder: each pass generates its ordered (context, bit) pairs without data-dependent
// branches, then the MQ coder codes them in one loop. A pair is a byte: context << 1 | bit; a pair is
// always written and the count advanced by its "coded" bit. The state is OpenJPEG's column words, updated
// at once where later decisions read it (the column's own word in a register), the neighbours' words from
// accumulators at the column's end (no sample of the column reads what it writes there).

struct Gen {
    u8* buf;
    u32 n;
    i32 nmse;
};

__forceinline void put(Gen& g, u32 cx, u32 d, u32 en) noexcept
{
    g.buf[g.n] = static_cast<u8>((cx << 1) | d);
    g.n += en;
}

struct Acc {
    u32 w, e, n0, n1, n2, s0, s1, s2;
};

// one row of a column, ci known at compile time: the zero-coding pair if `e`, then the sign pair if the
// sample becomes significant, the flags of the column word (f) at once and the neighbours' into `a`
template <u32 ci>
__forceinline void sig_row(const EncT1* t1, Gen& g, Acc& a, u32& f, const u32* fp, const i32* dp, u32 e, u32 one,
                           const std::int16_t* nt, u32 nsh) noexcept
{
    const u32 fs = f >> (3U * ci);
    const u32 x = smr_abs(dp[ci]);
    const u32 v = (x & one) ? 1U : 0U;
    put(g, t1->lutzc[fs & T1_SIGMA_NEIGHBOURS], v, e);
    const u32 ev = e & v;
    const u32 M = 0U - ev;
    const u32 lu = sc_index(f, fp[-1], fp[1], ci);
    const u32 s = smr_sign(dp[ci]);
    put(g, kLutCtxnoSc[lu], s ^ kLutSpb[lu], ev);
    NMSE_ADD(g.nmse, static_cast<i32>(nt[(x >> nsh) & 127U]) & static_cast<i32>(M));
    a.w |= (T1_SIGMA_5 << (3U * ci)) & M;
    a.e |= (T1_SIGMA_3 << (3U * ci)) & M;
    f |= (((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci)) & M;
    if constexpr (ci == 0) {
        a.n0 |= T1_SIGMA_17 & M;
        a.n1 |= ((s << T1_CHI_5_I) | T1_SIGMA_16) & M;
        a.n2 |= T1_SIGMA_15 & M;
    }
    if constexpr (ci == 3) {
        a.s0 |= T1_SIGMA_2 & M;
        a.s1 |= ((s << T1_CHI_0_I) | T1_SIGMA_1) & M;
        a.s2 |= T1_SIGMA_0 & M;
    }
}

__forceinline void flush_acc(u32* fp, u32 stride, const Acc& a) noexcept
{
    fp[-1] |= a.w;
    fp[1] |= a.e;
    u32* north = fp - stride;
    north[-1] |= a.n0;
    north[0] |= a.n1;
    north[1] |= a.n2;
    u32* south = fp + stride;
    south[-1] |= a.s0;
    south[0] |= a.s1;
    south[1] |= a.s2;
}

template <u32 ci>
__forceinline u32 spp_e(u32 f) noexcept
{
    const u32 fs = f >> (3U * ci);
    return static_cast<u32>((fs & T1_SIGMA_NEIGHBOURS) != 0) & static_cast<u32>((fs & (T1_SIGMA_THIS | T1_PI_THIS)) == 0);
}

// the library's steps on the pair buffer, for the partial last stripe and the run-mode columns that break
void put_sig_step(EncT1* t1, Gen& g, u32* flagsp, const i32* datap, i32 bpno, i32 one, u32 ci) noexcept
{
    const u32 flags = *flagsp;
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U && (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) {
        u32 v = (smr_abs(*datap) & static_cast<u32>(one)) ? 1 : 0;
        put(g, t1->lutzc[(flags >> (ci * 3U)) & T1_SIGMA_NEIGHBOURS], v, 1);
        if (v) {
            const u32 lu = sc_index(*flagsp, flagsp[-1], flagsp[1], ci);
            v = smr_sign(*datap);
            NMSE_ADD(g.nmse, nmse_sig(smr_abs(*datap), static_cast<u32>(bpno)));
            put(g, kLutCtxnoSc[lu], v ^ kLutSpb[lu], 1);
            update_flags(flagsp, ci, v, t1->w + 2);
        }
        *flagsp |= T1_PI_THIS << (ci * 3U);
    }
}
void put_cln_step(EncT1* t1, Gen& g, u32* flagsp, const i32* l_datap, i32 bpno, i32 one, u32 agg, u32 runlen,
                  u32 lim) noexcept
{
    const u32 check = kSigAll | kPiAll;
    if ((*flagsp & check) == check) {
        if (runlen == 0) {
            *flagsp &= ~kPiAll;
        } else if (runlen == 1) {
            *flagsp &= ~(T1_PI_1 | T1_PI_2 | T1_PI_3);
        } else if (runlen == 2) {
            *flagsp &= ~(T1_PI_2 | T1_PI_3);
        } else if (runlen == 3) {
            *flagsp &= ~(T1_PI_3);
        }
        return;
    }
    for (u32 ci = runlen; ci < lim; ++ci) {
        bool partial = false;
        if ((agg != 0) && (ci == runlen)) {
            partial = true;
        } else if (!(*flagsp & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)))) {
            const u32 v = (smr_abs(*l_datap) & static_cast<u32>(one)) ? 1 : 0;
            put(g, t1->lutzc[(*flagsp >> (ci * 3U)) & T1_SIGMA_NEIGHBOURS], v, 1);
            partial = v != 0;
        }
        if (partial) {
            const u32 lu = sc_index(*flagsp, flagsp[-1], flagsp[1], ci);
            NMSE_ADD(g.nmse, nmse_sig(smr_abs(*l_datap), static_cast<u32>(bpno)));
            const u32 v = smr_sign(*l_datap);
            put(g, kLutCtxnoSc[lu], v ^ kLutSpb[lu], 1);
            update_flags(flagsp, ci, v, t1->w + 2U);
        }
        *flagsp &= ~(T1_PI_THIS << (3U * ci));
        l_datap++;
    }
}

void gen_sigpass(EncT1* t1, Gen& g, i32 bpno) noexcept
{
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const u32 uone = static_cast<u32>(one);
    const std::int16_t* nt = bpno > 0 ? kNmsedecSig : kNmsedecSig0;
    const u32 nsh = bpno > 0 ? static_cast<u32>(bpno) : 0U;
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    const i32* datap = t1->data;
    u32 k;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        const u32 w = t1->w;
        for (u32 i = 0; i < w; ++i, ++f, datap += 4) {
            u32 fl = *f;
#if E_SPP_SKIP
            // no row eligible at the column's start: none can become so (only an eligible row's new
            // significance makes a later row of the column eligible)
            if (spp_elig(fl) == 0U) {
                continue;
            }
#else
            if (fl == 0U) {
                continue;
            }
#endif
            Acc a{};
            u32 e;
            e = spp_e<0>(fl);
            sig_row<0>(t1, g, a, fl, f, datap, e, uone, nt, nsh);
            fl |= (T1_PI_THIS << 0) & (0U - e);
            e = spp_e<1>(fl);
            sig_row<1>(t1, g, a, fl, f, datap, e, uone, nt, nsh);
            fl |= (T1_PI_THIS << 3) & (0U - e);
            e = spp_e<2>(fl);
            sig_row<2>(t1, g, a, fl, f, datap, e, uone, nt, nsh);
            fl |= (T1_PI_THIS << 6) & (0U - e);
            e = spp_e<3>(fl);
            sig_row<3>(t1, g, a, fl, f, datap, e, uone, nt, nsh);
            fl |= (T1_PI_THIS << 9) & (0U - e);
            *f = fl;
            flush_acc(f, stride, a);
        }
    }
    if (k < t1->h) {
        for (u32 i = 0; i < t1->w; ++i, ++f) {
            if (*f == 0U) {
                datap += (t1->h - k);
                continue;
            }
            for (u32 j = k; j < t1->h; ++j, ++datap) {
                put_sig_step(t1, g, f, datap, bpno, one, j - k);
            }
        }
    }
}

template <u32 ci>
__forceinline void ref_row(Gen& g, u32 f, u32 m, const i32* dp, u32 one, const std::int16_t* nt, u32 nsh) noexcept
{
    const u32 e = (m >> (4U + 3U * ci)) & 1U;
    const u32 x = smr_abs(dp[ci]);
    put(g, mag_ctx(f >> (3U * ci)), (x & one) ? 1U : 0U, e);
    NMSE_ADD(g.nmse, static_cast<i32>(nt[(x >> nsh) & 127U]) & static_cast<i32>(0U - e));
}

void gen_refpass(EncT1* t1, Gen& g, i32 bpno) noexcept
{
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const u32 uone = static_cast<u32>(one);
    const std::int16_t* nt = bpno > 0 ? kNmsedecRef : kNmsedecRef0;
    const u32 nsh = bpno > 0 ? static_cast<u32>(bpno) : 0U;
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    const i32* datap = t1->data;
    u32 k;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        for (u32 i = 0; i < t1->w; ++i, f++, datap += 4) {
            const u32 fl = *f;
            const u32 m = fl & ~(fl >> 17) & 0x2490U;
            if (m == 0) {
                continue;
            }
            ref_row<0>(g, fl, m, datap, uone, nt, nsh);
            ref_row<1>(g, fl, m, datap, uone, nt, nsh);
            ref_row<2>(g, fl, m, datap, uone, nt, nsh);
            ref_row<3>(g, fl, m, datap, uone, nt, nsh);
            *f = fl | (m << 16);
        }
    }
    if (k < t1->h) {
        const u32 remaining_lines = t1->h - k;
        for (u32 i = 0; i < t1->w; ++i, ++f) {
            if ((*f & kSigAll) == 0) {
                datap += remaining_lines;
                continue;
            }
            for (u32 j = 0; j < remaining_lines; j++, datap++) {
                if ((*f & ((T1_SIGMA_THIS | T1_PI_THIS) << (j * 3U))) == (T1_SIGMA_THIS << (j * 3U))) {
                    const u32 abs_data = smr_abs(*datap);
                    NMSE_ADD(g.nmse, nmse_ref(abs_data, static_cast<u32>(bpno)));
                    put(g, mag_ctx(*f >> (j * 3U)), (static_cast<i32>(abs_data) & one) ? 1 : 0, 1);
                    *f |= T1_MU_THIS << (j * 3U);
                }
            }
        }
    }
}

template <u32 ci>
__forceinline void cln_row(const EncT1* t1, Gen& g, Acc& a, u32& f, const u32* fp, const i32* dp, u32 cand, u32 one,
                           const std::int16_t* nt, u32 nsh) noexcept
{
    sig_row<ci>(t1, g, a, f, fp, dp, (cand >> (3U * ci)) & 1U, one, nt, nsh);
}

void gen_clnpass(EncT1* t1, Gen& g, i32 bpno) noexcept
{
    const i32 one = 1 << (bpno + T1_NMSEDEC_FRACBITS);
    const u32 uone = static_cast<u32>(one);
    const std::int16_t* nt = bpno > 0 ? kNmsedecSig : kNmsedecSig0;
    const u32 nsh = bpno > 0 ? static_cast<u32>(bpno) : 0U;
    const i32* datap = t1->data;
    const u32 stride = t1->w + 2;
    u32* f = &t1->flags[stride + 1];
    u32 k;
    for (k = 0; k < (t1->h & ~3U); k += 4, f += 2) {
        for (u32 i = 0; i < t1->w; ++i, f++, datap += 4) {
            u32 fl = *f;
            if (fl == 0) {
                const u32 m4 = ((smr_abs(datap[0]) & uone) ? 1U : 0U) | ((smr_abs(datap[1]) & uone) ? 2U : 0U) |
                               ((smr_abs(datap[2]) & uone) ? 4U : 0U) | ((smr_abs(datap[3]) & uone) ? 8U : 0U);
                if (m4 == 0) {
                    put(g, T1_CTXNO_AGG, 0, 1);
                    continue;
                }
                const u32 runlen = ebc::ctz32(m4);
                put(g, T1_CTXNO_AGG, 1, 1);
                put(g, T1_CTXNO_UNI, runlen >> 1, 1);
                put(g, T1_CTXNO_UNI, runlen & 1, 1);
                put_cln_step(t1, g, f, datap + runlen, bpno, one, 1, runlen, 4U);
                continue;
            }
            const u32 cand = ~((fl >> 4) | (fl >> 21)) & 0x249U;
            Acc a{};
            cln_row<0>(t1, g, a, fl, f, datap, cand, uone, nt, nsh);
            cln_row<1>(t1, g, a, fl, f, datap, cand, uone, nt, nsh);
            cln_row<2>(t1, g, a, fl, f, datap, cand, uone, nt, nsh);
            cln_row<3>(t1, g, a, fl, f, datap, cand, uone, nt, nsh);
            *f = fl & ~kPiAll;
            flush_acc(f, stride, a);
        }
    }
    if (k < t1->h) {
        for (u32 i = 0; i < t1->w; ++i, f++) {
            put_cln_step(t1, g, f, datap, bpno, one, 0, 0, t1->h - k);
            datap += t1->h - k;
        }
    }
}

// the pass's pairs through the stage-2 coder
__forceinline void code_pairs(Mq& m, const u8* buf, u32 n) noexcept
{
    for (u32 i = 0; i < n; ++i) {
        const u32 p = buf[i];
        m.encode(p >> 1, p & 1U);
    }
}
#endif

double getwmsedec(i32 nmsedec, u32 compno, u32 level, u32 orient, i32 bpno, bool reversible, double stepsize,
                  const double* mct_norms, u32 mct_numcomps) noexcept
{
    double w1 = 1, w2, wmsedec;
    if (mct_norms && (compno < mct_numcomps)) {
        w1 = mct_norms[compno];
    }
    if (reversible) {
        w2 = enc_dwt_norm(level, orient);
    } else {
        const i32 log2_gain = (orient == 0) ? 0 : (orient == 3) ? 2 : 1;
        w2 = enc_dwt_norm_real(level, orient);
        stepsize /= (1 << log2_gain);
    }
    wmsedec = w1 * w2 * stepsize * (1 << bpno);
    wmsedec *= wmsedec * nmsedec / 8192.0;
    return wmsedec;
}

#if E_PASSCLK
#define CLK() __rdtsc()
#else
#define CLK() 0ULL
#endif

}  // namespace

EncT1* enc_t1_create(const Allocator& alloc) noexcept
{
    void* mem = alloc.allocate(sizeof(EncT1), 64);
    if (!mem) {
        return nullptr;
    }
    std::memset(mem, 0, sizeof(EncT1));
    return static_cast<EncT1*>(mem);
}
void enc_t1_destroy(EncT1* t1, const Allocator& alloc) noexcept
{
    if (t1) {
        alloc.release(t1, sizeof(EncT1));
    }
}
std::int32_t* enc_t1_data(EncT1* t1) noexcept { return t1->data; }
std::size_t enc_t1_size() noexcept { return sizeof(EncT1); }

bool enc_t1_encode(EncT1* t1, EncCblk& cblk, std::uint32_t orient, std::uint32_t compno, std::uint32_t level,
                   bool reversible, double stepsize, const double* mct_norms, std::uint32_t mct_numcomps) noexcept
{
    [[maybe_unused]] const u64 t_begin = CLK();
    double cumwmsedec = 0.0;
    u32 passno;
    i32 bpno;
    u32 passtype;
    i32 nmsedec = 0;
    i32 max;
    t1->w = static_cast<u32>(cblk.x1 - cblk.x0);
    t1->h = static_cast<u32>(cblk.y1 - cblk.y0);
    {
        const u32 flags_stride = t1->w + 2U;
        const u32 flags_height = (t1->h + 3U) / 4U;
        const u32 flagssize = (flags_height + 2U) * flags_stride;
        std::memset(t1->flags, 0, flagssize * sizeof(u32));
        u32* p = &t1->flags[0];
        for (u32 x = 0; x < flags_stride; ++x) {
            *p++ = kPiAll;
        }
        p = &t1->flags[((flags_height + 1) * flags_stride)];
        for (u32 x = 0; x < flags_stride; ++x) {
            *p++ = kPiAll;
        }
        if (t1->h % 4) {
            u32 v = 0;
            p = &t1->flags[((flags_height)*flags_stride)];
            if (t1->h % 4 == 1) {
                v |= T1_PI_1 | T1_PI_2 | T1_PI_3;
            } else if (t1->h % 4 == 2) {
                v |= T1_PI_2 | T1_PI_3;
            } else if (t1->h % 4 == 3) {
                v |= T1_PI_3;
            }
            for (u32 x = 0; x < flags_stride; ++x) {
                *p++ = v;
            }
        }
    }
    t1->lutzc = kLutCtxnoZc + (orient << 9);

    const u32 n = t1->w * t1->h;
#if E_SIMD_PRE
    {
        u32 i = 0;
        __m256i vmax = _mm256_setzero_si256();
        const __m256i vmin1 = _mm256_set1_epi32(INT32_MIN + 1);
        const __m256i vsign = _mm256_set1_epi32(INT32_MIN);
        for (; i + 8 <= n; i += 8) {
            const __m256i x = _mm256_load_si256(reinterpret_cast<const __m256i*>(t1->data + i));
            const __m256i a = _mm256_abs_epi32(_mm256_max_epi32(x, vmin1));
            vmax = _mm256_max_epi32(vmax, a);
            _mm256_store_si256(reinterpret_cast<__m256i*>(t1->data + i), _mm256_or_si256(a, _mm256_and_si256(x, vsign)));
        }
        __m128i m4 = _mm_max_epi32(_mm256_castsi256_si128(vmax), _mm256_extracti128_si256(vmax, 1));
        m4 = _mm_max_epi32(m4, _mm_shuffle_epi32(m4, 0x4E));
        m4 = _mm_max_epi32(m4, _mm_shuffle_epi32(m4, 0xB1));
        max = _mm_cvtsi128_si32(m4);
        for (; i < n; ++i) {
            i32 tmp = t1->data[i];
            if (tmp < 0) {
                if (tmp == INT32_MIN) {
                    tmp = INT32_MIN + 1;
                }
                max = max > -tmp ? max : -tmp;
                const u32 tu = static_cast<u32>(-tmp) | 0x80000000U;
                std::memcpy(&t1->data[i], &tu, 4);
            } else {
                max = max > tmp ? max : tmp;
            }
        }
    }
#elif E_PRE_BRFREE
    {
        u32 mx = 0;
        for (u32 i = 0; i < n; ++i) {
            const u32 x = static_cast<u32>(t1->data[i]);
            const u32 neg = 0U - (x >> 31);
            const u32 y = x == 0x80000000U ? 0x80000001U : x;  // INT32_MIN as INT32_MIN + 1
            const u32 a = (y ^ neg) - neg;
            mx = a > mx ? a : mx;
            t1->data[i] = static_cast<i32>(a | (x & 0x80000000U));
        }
        max = static_cast<i32>(mx);
    }
#else
    max = 0;
    for (u32 i = 0; i < n; ++i) {
        i32 tmp = t1->data[i];
        if (tmp < 0) {
            if (tmp == INT32_MIN) {
                tmp = INT32_MIN + 1;
            }
            max = max > -tmp ? max : -tmp;
            const u32 tu = static_cast<u32>(-tmp) | 0x80000000U;
            std::memcpy(&t1->data[i], &tu, 4);
        } else {
            max = max > tmp ? max : tmp;
        }
    }
#endif
    cblk.numbps = max ? static_cast<u32>((floorlog2(max) + 1) - T1_NMSEDEC_FRACBITS) : 0;
    [[maybe_unused]] const u64 t_pre = CLK();
#if E_PASSCLK
    ebc::g_stage_cycles[0] += t_pre - t_begin;
#endif
    CNT((++ebc::g_counts.blocks, ebc::g_counts.samples += n, ebc::g_counts.bitplanes += cblk.numbps));
    if (cblk.numbps == 0) {
        cblk.totalpasses = 0;
        return true;
    }
    bpno = static_cast<i32>(cblk.numbps - 1);
    passtype = 2;
    t1->out[0] = 0;
    Mq& mq = t1->mqc;
    mq.resetstates();
    mq.setstate(T1_CTXNO_UNI, 0, 46);
    mq.setstate(T1_CTXNO_AGG, 0, 3);
    mq.setstate(T1_CTXNO_ZC, 0, 4);
    mq.init_enc(t1->out + 1, t1->out + kOutCapacity);

    for (passno = 0; bpno >= 0; ++passno) {
        EncPass* pass = &t1->passes[passno];
        [[maybe_unused]] const u64 p0 = CLK();
        {
            Mq m = mq;
#if E_SPLIT
            if ((E_SPLIT >> passtype) & 1) {
                Gen g{t1->pairs, 0, 0};
                switch (passtype) {
                case 0:
                    gen_sigpass(t1, g, bpno);
                    break;
                case 1:
                    gen_refpass(t1, g, bpno);
                    break;
                case 2:
                    gen_clnpass(t1, g, bpno);
                    break;
                }
                nmsedec = g.nmse;
#if E_PASSCLK
                ebc::g_pass_cycles[3] += CLK() - p0;
#endif
                code_pairs(m, g.buf, g.n);
            } else
#endif
            {
                switch (passtype) {
                case 0:
                    enc_sigpass(t1, m, bpno, nmsedec);
                    break;
                case 1:
                    enc_refpass(t1, m, bpno, nmsedec);
                    break;
                case 2:
                    enc_clnpass(t1, m, bpno, nmsedec);
                    break;
                }
            }
            mq = m;
        }
#if E_PASSCLK
        ebc::g_pass_cycles[passtype] += CLK() - p0;
#endif
        CNT(++ebc::g_counts.passes[passtype]);
#if E_MODEL
        ebc::g_sink += mq.sink;
#endif
        const double tempwmsedec =
            getwmsedec(nmsedec, compno, level, orient, bpno, reversible, stepsize, mct_norms, mct_numcomps);
        cumwmsedec += tempwmsedec;
        pass->distortiondec = cumwmsedec;
        if (passtype == 2 && bpno == 0) {
#if !E_MODEL
            mq.flush();
#endif
            pass->term = 1;
            pass->rate = mq.numbytes();
        } else {
            pass->term = 0;
            pass->rate = mq.numbytes() + 3;
        }
        if (++passtype == 3) {
            passtype = 0;
            bpno--;
        }
    }
    [[maybe_unused]] const u64 t_passes = CLK();
#if E_MODEL
    // the sink wrote no bytes: no rates to finish
    cblk.totalpasses = 0;
    return true;
#endif
    if (mq.overflow) {
        return false;
    }
    cblk.totalpasses = passno;
    if (cblk.totalpasses) {
        u32 last_pass_rate = mq.numbytes();
        for (passno = cblk.totalpasses; passno > 0;) {
            EncPass* pass = &t1->passes[--passno];
            if (pass->rate > last_pass_rate) {
                pass->rate = last_pass_rate;
            } else {
                last_pass_rate = pass->rate;
            }
        }
    }
    for (passno = 0; passno < cblk.totalpasses; passno++) {
        EncPass* pass = &t1->passes[passno];
        if (t1->out[1 + pass->rate - 1] == 0xFF) {
            pass->rate--;
        }
        pass->len = pass->rate - (passno == 0 ? 0 : t1->passes[passno - 1].rate);
    }
#if E_PASSCLK
    const u64 t_end = CLK();
    ebc::g_stage_cycles[1] += t_passes - t_pre;
    ebc::g_stage_cycles[2] += t_end - t_passes;
#endif
    return true;
}

bool enc_t1_commit(const EncT1* t1, Arena& arena, EncCblk& cblk) noexcept
{
    if (!cblk.totalpasses) {
        return true;
    }
    cblk.passes = arena.make_array<EncPass>(cblk.totalpasses);
    const u32 bytes = t1->mqc.numbytes();
    cblk.data = static_cast<u8*>(arena.allocate(bytes ? bytes : 1, 16));
    if (!cblk.passes || !cblk.data) {
        return false;
    }
    std::memcpy(cblk.passes, t1->passes, cblk.totalpasses * sizeof(EncPass));
    std::memcpy(cblk.data, t1->out + 1, bytes);
    return true;
}
