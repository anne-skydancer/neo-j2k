// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// EBCOT-NG study, decoder: src/t1.cpp's Tier-1 decoder (OpenJPEG 2.5.4's passes with every Part 1 mode
// switch: BYPASS raw passes, RESET, RESTART/TERMALL segments, VSC, SEGSYM) rewritten on MQ-NG stages 1+2
// (doc/mqng_study.md: state words in the context slots, the decision by selects with the renormalisation
// always run, the cleanup's AGG MPS runs taken at once), the coder's registers in locals for each pass.
// Included by dec_v.cpp inside namespace ebc::DNG_NS with the switches below (0 or 1). With all of them 0
// this is the control (D0): the library's operations in the library's order on the stage 1+2 coder.
//   D_MODEL     the decisions' bits read from the recording (the passes alone, no MQ decoder, no AGG runs)
//   D_REC       every decision's bit appended to the recording (for D_MODEL)
//   D_NOAGG     no AGG runs (stage 1 only)
//   D_NO_CTX    every context a constant (ablation; with D_MODEL the control flow is unchanged)
//   D_NO_STORE  no coefficient writes (ablation, with D_MODEL)
//   D_DUP_UPD   the flag update done twice (idempotent; cost by difference)
//   D_COUNT     tallies (common.h)
//   D_PASSCLK   TSC cycles a pass type into g_pass_cycles (3: raw passes)
//   D_MAGREG    refinement pass: the three magnitude contexts' state words held in registers
//   D_REF_MASK  refinement pass: the refined samples of a column as a mask (they cannot change in the pass)
//   D_SPP_MASK  significance pass: eligible samples as a mask, recomputed after each new significant sample
//   D_CLN_MASK  cleanup outside run mode: the samples to decode as a mask (fixed for the column)
//   D_BRFREE    the flag update's north/south parts branch-free when the row is a run-time value
//   D_REF_LIST  refinement pass: the refined samples listed first without data-dependent branches (their
//               coefficient index and context), then decoded in one loop (overrides D_REF_MASK, D_MAGREG)
#ifndef D_MODEL
#define D_MODEL 0
#endif
#ifndef D_REC
#define D_REC 0
#endif
#ifndef D_NOAGG
#define D_NOAGG 0
#endif
#ifndef D_NO_CTX
#define D_NO_CTX 0
#endif
#ifndef D_NO_STORE
#define D_NO_STORE 0
#endif
#ifndef D_DUP_UPD
#define D_DUP_UPD 0
#endif
#ifndef D_COUNT
#define D_COUNT 0
#endif
#ifndef D_PASSCLK
#define D_PASSCLK 0
#endif
#ifndef D_MAGREG
#define D_MAGREG 0
#endif
#ifndef D_REF_MASK
#define D_REF_MASK 0
#endif
#ifndef D_SPP_MASK
#define D_SPP_MASK 0
#endif
#ifndef D_CLN_MASK
#define D_CLN_MASK 0
#endif
#ifndef D_BRFREE
#define D_BRFREE 0
#endif
#ifndef D_REF_LIST
#define D_REF_LIST 0
#endif

#if D_COUNT
#define CNT(x) (x)
#define SITE(k, t) ebc::site((k), (t))
#else
#define CNT(x) ((void)0)
#define SITE(k, t) ((void)0)
#endif
#if D_NO_STORE
#define STORE(lhs, rhs) ((void)0)
#else
#define STORE(lhs, rhs) ((lhs) = (rhs))
#endif

using nj2k::detail::T1Block;
using nj2k::detail::kLutCtxnoZc;
using nj2k::detail::kLutCtxnoSc;
using nj2k::detail::kLutSpb;

constexpr u32 kExtra = 2;
constexpr u32 kPiAll = T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3;

struct Dt1 {
    u32 a, c, ct;
    const u8* bp;
    u8* end;
    u8 backup[2];
    u32 st[19];
    const u8* lutzc;
    i32* data;
    u32* flags;
    u32 w, h;
    u8* cblkdata;
    std::size_t cblkdata_size;
};

struct Reg {
    u32 a, c, ct;
    const u8* bp;
};

__forceinline void bytein(Reg& r) noexcept
{
    const u32 l_c = r.bp[1];
    if (r.bp[0] == 0xff) {
        if (l_c > 0x8f) {
            r.c += 0xff00;
            r.ct = 8;
        } else {
            r.bp++;
            r.c += l_c << 9;
            r.ct = 7;
        }
    } else {
        r.bp++;
        r.c += l_c << 8;
        r.ct = 8;
    }
}

__declspec(noinline) void renorm_slow(Reg& r) noexcept
{
    u32 shifts = mq_shifts(r.a);
    do {
        if (r.ct == 0) {
            bytein(r);
        }
        const u32 k = shifts < r.ct ? shifts : r.ct;
        r.a <<= k;
        r.c <<= k;
        r.ct -= k;
        shifts -= k;
    } while (shifts != 0);
}

// MQ-NG stage 1's decision on a state word w; the slot's next word in nw
__forceinline u32 decide_w(Reg& r, u32 w, u32& nw) noexcept
{
    const u32 qe = w & 0xFFFFU;
    r.a -= qe;
    const u32 lpsi = (r.c >> 16) < qe;
    const u32 flip = lpsi ^ static_cast<u32>(r.a < qe);
    const u32 lm = 0U - lpsi;
    const u32 d = ((w >> 16) & 1U) ^ flip;
    const u32 n = flip ? (w >> 24) : ((w >> 17) & 0x7FU);
    const u32 nx = kWords.w[n];
    nw = (lpsi | static_cast<u32>(r.a < 0x8000U)) ? nx : w;
    r.c -= (qe << 16) & ~lm;
    r.a = (r.a & ~lm) | (qe & lm);
    const u32 s = mq_shifts(r.a);
    if (s <= r.ct) {
        r.a <<= s;
        r.c <<= s;
        r.ct -= s;
    } else {
        renorm_slow(r);
    }
    return d;
}

__forceinline u32 decide(Reg& r, u32* slot) noexcept
{
#if D_MODEL
    (void)r;
    (void)slot;
    return *g_bits++;
#else
    u32 nw;
    const u32 d = decide_w(r, *slot, nw);
    *slot = nw;
#if D_REC
    if (g_rec.n < g_rec.cap) {
        g_rec.bits[g_rec.n++] = static_cast<u8>(d);
    }
#endif
    return d;
#endif
}

__forceinline u32 raw_decode(Dt1& t) noexcept
{
#if D_MODEL
    (void)t;
    return *g_bits++;
#else
    if (t.ct == 0) {
        if (t.c == 0xff) {
            if (*t.bp > 0x8f) {
                t.c = 0xff;
                t.ct = 8;
            } else {
                t.c = *t.bp;
                t.bp++;
                t.ct = 7;
            }
        } else {
            t.c = *t.bp;
            t.bp++;
            t.ct = 8;
        }
    }
    t.ct--;
    const u32 d = (t.c >> t.ct) & 0x01U;
#if D_REC
    if (g_rec.n < g_rec.cap) {
        g_rec.bits[g_rec.n++] = static_cast<u8>(d);
    }
#endif
    return d;
#endif
}

__forceinline Reg load(const Dt1& t) noexcept { return Reg{t.a, t.c, t.ct, t.bp}; }
__forceinline void save(Dt1& t, const Reg& r) noexcept
{
    t.a = r.a;
    t.c = r.c;
    t.ct = r.ct;
    t.bp = r.bp;
}

__forceinline u32 zc_ctx(const Dt1& t, u32 f) noexcept
{
    CNT(++g_counts.lut_loads);
#if D_NO_CTX
    (void)t;
    (void)f;
    return 0;
#else
    return t.lutzc[f & T1_SIGMA_NEIGHBOURS];
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
#if D_NO_CTX
    (void)f;
    return T1_CTXNO_MAG;
#else
    const u32 tmp = (f & T1_SIGMA_NEIGHBOURS) ? T1_CTXNO_MAG + 1 : T1_CTXNO_MAG;
    return (f & T1_MU_0) ? T1_CTXNO_MAG + 2 : tmp;
#endif
}

// opj_t1_update_flags_macro: own word in `flags`, the neighbours' in memory
__forceinline void update_once(u32& flags, u32* flagsp, u32 ci, u32 s, u32 stride, u32 vsc) noexcept
{
    CNT(g_counts.upd_rmw += ((ci == 0 && !vsc) || ci == 3) ? 6 : 3);
    flagsp[-1] |= T1_SIGMA_5 << (3U * ci);
    flags |= ((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci);
    flagsp[1] |= T1_SIGMA_3 << (3U * ci);
#if D_BRFREE
    {
        const u32 mn = 0U - static_cast<u32>(ci == 0U && !vsc), ms = 0U - static_cast<u32>(ci == 3U);
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
    if (ci == 0U && !vsc) {
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
__forceinline void update_flags(u32& flags, u32* flagsp, u32 ci, u32 s, u32 stride, u32 vsc) noexcept
{
    CNT(++g_counts.sig_events);
    update_once(flags, flagsp, ci, s, stride, vsc);
#if D_DUP_UPD
    _ReadWriteBarrier();
    update_once(flags, flagsp, ci, s, stride, vsc);
    _ReadWriteBarrier();
#endif
}

__forceinline u32 spp_elig(u32 f) noexcept
{
    const u32 g = f | (f >> 1) | (f >> 2);
    const u32 h = g | (g >> 3) | (g >> 6);
    return h & ~(f >> 4) & ~(f >> 21) & 0x249U;
}

// the sign decision and the coefficient of a sample that became significant
__forceinline void sign_decode(Dt1& t, Reg& r, u32& flags, u32* flagsp, i32* data, u32 l_w, u32 ci, i32 oph, u32 stride,
                               u32 vsc) noexcept
{
    const u32 lu = sc_index(flags, flagsp[-1], flagsp[1], ci);
    CNT(g_counts.lut_loads += 2);
    const u32 ctxt2 = D_NO_CTX ? T1_CTXNO_SC : kLutCtxnoSc[lu];
    const u32 spb = D_NO_CTX ? 0 : kLutSpb[lu];
    const u32 v = decide(r, &t.st[ctxt2]) ^ spb;
    STORE(data[ci * l_w], v ? -oph : oph);
    update_flags(flags, flagsp, ci, v, stride, vsc);
}

// ---- significance pass (MQ) ----
__forceinline void sig_step(Dt1& t, Reg& r, u32& flags, u32* flagsp, i32* data, u32 l_w, u32 ci, i32 oph, u32 stride,
                            u32 vsc) noexcept
{
    CNT(++g_counts.tested[0]);
    const bool elig = (flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U &&
                      (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U;
    SITE(kSppElig, elig);
    if (elig) {
        const u32 ctxt1 = zc_ctx(t, flags >> (ci * 3U));
        const u32 v = decide(r, &t.st[ctxt1]);
        CNT(++g_counts.dec[0]);
        SITE(kSppBit, v);
        if (v) {
            sign_decode(t, r, flags, flagsp, data, l_w, ci, oph, stride, vsc);
            CNT((++g_counts.dec[0], ++g_counts.sc[0]));
        }
        flags |= T1_PI_THIS << (ci * 3U);
    }
}

void sigpass_mqc(Dt1& t, i32 bpno, u32 vsc) noexcept
{
    const i32 one = 1 << bpno, half = one >> 1, oph = one | half;
    i32* data = t.data;
    const u32 l_w = t.w, h = t.h, stride = l_w + 2;
    u32* flagsp = &t.flags[stride + 1];
    u32 k;
    Reg r = load(t);
    for (k = 0; k < (h & ~3U); k += 4, data += 3 * l_w, flagsp += 2) {
        for (u32 i = 0; i < l_w; ++i, ++data, ++flagsp) {
            u32 flags = *flagsp;
            CNT(++g_counts.cols[0]);
            SITE(kSppCol, flags != 0);
            if (flags != 0) {
                CNT(++g_counts.cols_work[0]);
#if D_SPP_MASK
                u32 e = spp_elig(flags);
                while (e) {
                    const u32 b = ctz32(e);
                    const u32 ci = (b * 11U) >> 5;
                    CNT(++g_counts.tested[0]);
                    const u32 v = decide(r, &t.st[zc_ctx(t, flags >> b)]);
                    CNT(++g_counts.dec[0]);
                    if (v) {
                        sign_decode(t, r, flags, flagsp, data, l_w, ci, oph, stride, vsc & (ci == 0 ? 1U : 0U));
                        CNT((++g_counts.dec[0], ++g_counts.sc[0]));
                        flags |= T1_PI_THIS << b;
                        e = spp_elig(flags) & ~((2U << b) - 1U);
                    } else {
                        flags |= T1_PI_THIS << b;
                        e &= e - 1U;
                    }
                }
#else
                sig_step(t, r, flags, flagsp, data, l_w, 0, oph, stride, vsc);
                sig_step(t, r, flags, flagsp, data, l_w, 1, oph, stride, 0);
                sig_step(t, r, flags, flagsp, data, l_w, 2, oph, stride, 0);
                sig_step(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
#endif
                *flagsp = flags;
            }
        }
    }
    if (k < h) {
        for (u32 i = 0; i < l_w; ++i, ++data, ++flagsp) {
            for (u32 j = 0; j < h - k; ++j) {
                u32 flags = *flagsp;
                sig_step(t, r, flags, flagsp, data + j * l_w, 0, j, oph, stride, vsc);
                *flagsp = flags;
            }
        }
    }
    save(t, r);
}

// ---- significance pass (raw) ----
__forceinline void sig_step_raw(Dt1& t, u32* flagsp, i32* datap, i32 oph, u32 vsc, u32 ci, u32 stride) noexcept
{
    u32 flags = *flagsp;
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U &&
        (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) {
        if (raw_decode(t)) {
            const u32 v = raw_decode(t);
            STORE(*datap, v ? -oph : oph);
            update_flags(flags, flagsp, ci, v, stride, vsc);
        }
        flags |= T1_PI_THIS << (ci * 3U);
        *flagsp = flags;
    }
}

void sigpass_raw(Dt1& t, i32 bpno, u32 cblksty) noexcept
{
    const i32 one = 1 << bpno, half = one >> 1, oph = one | half;
    i32* data = t.data;
    const u32 l_w = t.w, stride = l_w + 2;
    u32* flagsp = &t.flags[stride + 1];
    const u32 vsc = (cblksty & nj2k::detail::kCblkVsc) ? 1U : 0U;
    u32 k;
    for (k = 0; k < (t.h & ~3U); k += 4, flagsp += 2, data += 3 * l_w) {
        for (u32 i = 0; i < l_w; ++i, ++flagsp, ++data) {
            if (*flagsp != 0) {
                sig_step_raw(t, flagsp, data, oph, vsc, 0U, stride);
                sig_step_raw(t, flagsp, data + l_w, oph, 0, 1U, stride);
                sig_step_raw(t, flagsp, data + 2 * l_w, oph, 0, 2U, stride);
                sig_step_raw(t, flagsp, data + 3 * l_w, oph, 0, 3U, stride);
            }
        }
    }
    if (k < t.h) {
        for (u32 i = 0; i < l_w; ++i, ++flagsp, ++data) {
            for (u32 j = 0; j < t.h - k; ++j) {
                sig_step_raw(t, flagsp, data + j * l_w, oph, vsc, j, stride);
            }
        }
    }
}

// ---- refinement pass (MQ) ----
__forceinline void ref_upd(i32* x, u32 v, i32 poshalf) noexcept
{
#if D_NO_STORE
    (void)x;
    (void)v;
    (void)poshalf;
#else
    *x += (v ^ static_cast<u32>(*x < 0)) ? poshalf : -poshalf;
#endif
}

void refpass_mqc(Dt1& t, i32 bpno) noexcept
{
    const i32 one = 1 << bpno, poshalf = one >> 1;
    i32* data = t.data;
    const u32 l_w = t.w, h = t.h, stride = l_w + 2;
    u32* flagsp = &t.flags[stride + 1];
    u32 k;
    Reg r = load(t);
#if D_MAGREG && !D_MODEL
    u32 w14 = t.st[14], w15 = t.st[15], w16 = t.st[16];
#define REF_DECIDE(ctxt, v)                                                                                      \
    {                                                                                                            \
        const u32 w_ = (ctxt) == 16 ? w16 : (ctxt) == 15 ? w15 : w14;                                           \
        u32 nw_;                                                                                                 \
        v = decide_w(r, w_, nw_);                                                                                \
        w14 = (ctxt) == 14 ? nw_ : w14;                                                                          \
        w15 = (ctxt) == 15 ? nw_ : w15;                                                                          \
        w16 = (ctxt) == 16 ? nw_ : w16;                                                                          \
    }
#else
#define REF_DECIDE(ctxt, v) v = decide(r, &t.st[ctxt])
#endif
#if D_REF_LIST
    {
        // the list: coefficient index (12 bits) and context - 14 (bits 12-13); the pass changes no flag that
        // decides which samples it refines or their contexts, so the list is exact before the decisions
        static thread_local std::uint16_t list[4096];
        u32 n = 0;
        for (k = 0; k < (h & ~3U); k += 4, flagsp += 2) {
            for (u32 i = 0; i < l_w; ++i, ++flagsp) {
                const u32 flags = *flagsp;
                const u32 mk = flags & ~(flags >> 17) & 0x2490U;
                if (mk == 0) {
                    continue;
                }
                *flagsp = flags | (mk << 16);
                const u32 base = k * l_w + i;
                for (u32 ci = 0; ci < 4; ++ci) {
                    const u32 e = (mk >> (4U + 3U * ci)) & 1U;
                    list[n] = static_cast<std::uint16_t>((base + ci * l_w) | ((mag_ctx(flags >> (3U * ci)) - 14U) << 12));
                    n += e;
                }
            }
        }
        for (u32 j = 0; j < n; ++j) {
            const u32 x = list[j];
            const u32 v = decide(r, &t.st[14U + (x >> 12)]);
            ref_upd(&t.data[x & 0xFFFU], v, poshalf);
        }
        data = t.data + (h & ~3U) * l_w;
    }
    if (false)
#endif
    for (k = 0; k < (h & ~3U); k += 4, data += 3 * l_w, flagsp += 2) {
        for (u32 i = 0; i < l_w; ++i, ++data, ++flagsp) {
            u32 flags = *flagsp;
            CNT(++g_counts.cols[1]);
#if D_REF_MASK
            u32 mk = flags & ~(flags >> 17) & 0x2490U;
            SITE(kRefCol, mk != 0);
            if (mk == 0) {
                continue;
            }
            CNT(++g_counts.cols_work[1]);
            *flagsp = flags | (mk << 16);
            do {
                const u32 b = ctz32(mk);
                const u32 s = b - 4U;
                const u32 ci = (s * 11U) >> 5;
                mk &= mk - 1U;
                CNT((++g_counts.tested[1], ++g_counts.dec[1]));
                const u32 ctxt = mag_ctx(flags >> s);
                u32 v;
                REF_DECIDE(ctxt, v);
                ref_upd(&data[ci * l_w], v, poshalf);
            } while (mk);
#else
            SITE(kRefCol, flags != 0);
            if (flags != 0) {
                CNT(++g_counts.cols_work[1]);
#define REF_STEP(ci)                                                                                             \
    {                                                                                                            \
        CNT(++g_counts.tested[1]);                                                                               \
        const bool elig_ = (flags & ((T1_SIGMA_THIS | T1_PI_THIS) << ((ci) * 3U))) == (T1_SIGMA_THIS << ((ci) * 3U)); \
        SITE(kRefElig, elig_);                                                                                   \
        if (elig_) {                                                                                             \
            const u32 ctxt = mag_ctx(flags >> ((ci) * 3U));                                                      \
            u32 v;                                                                                               \
            REF_DECIDE(ctxt, v);                                                                                 \
            CNT(++g_counts.dec[1]);                                                                              \
            ref_upd(&data[(ci) * l_w], v, poshalf);                                                              \
            flags |= T1_MU_THIS << ((ci) * 3U);                                                                  \
        }                                                                                                        \
    }
                REF_STEP(0)
                REF_STEP(1)
                REF_STEP(2)
                REF_STEP(3)
#undef REF_STEP
                *flagsp = flags;
            }
#endif
        }
    }
#if D_MAGREG && !D_MODEL
    t.st[14] = w14;
    t.st[15] = w15;
    t.st[16] = w16;
#endif
#undef REF_DECIDE
    if (k < h) {
        for (u32 i = 0; i < l_w; ++i, ++data, ++flagsp) {
            for (u32 j = 0; j < h - k; ++j) {
                u32 flags = *flagsp;
                if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (j * 3U))) == (T1_SIGMA_THIS << (j * 3U))) {
                    const u32 ctxt = mag_ctx(flags >> (j * 3U));
                    const u32 v = decide(r, &t.st[ctxt]);
                    ref_upd(&data[j * l_w], v, poshalf);
                    flags |= T1_MU_THIS << (j * 3U);
                    *flagsp = flags;
                }
            }
        }
    }
    save(t, r);
}

// ---- refinement pass (raw) ----
void refpass_raw(Dt1& t, i32 bpno) noexcept
{
    const i32 one = 1 << bpno, poshalf = one >> 1;
    i32* data = t.data;
    const u32 l_w = t.w, stride = l_w + 2;
    u32* flagsp = &t.flags[stride + 1];
    u32 k;
    auto step = [&](u32* fp, i32* dp, u32 ci) {
        if ((*fp & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == (T1_SIGMA_THIS << (ci * 3U))) {
            const u32 v = raw_decode(t);
            ref_upd(dp, v, poshalf);
            *fp |= T1_MU_THIS << (ci * 3U);
        }
    };
    for (k = 0; k < (t.h & ~3U); k += 4, flagsp += 2, data += 3 * l_w) {
        for (u32 i = 0; i < l_w; ++i, ++flagsp, ++data) {
            if (*flagsp != 0) {
                step(flagsp, data, 0U);
                step(flagsp, data + l_w, 1U);
                step(flagsp, data + 2 * l_w, 2U);
                step(flagsp, data + 3 * l_w, 3U);
            }
        }
    }
    if (k < t.h) {
        for (u32 i = 0; i < l_w; ++i, ++flagsp, ++data) {
            for (u32 j = 0; j < t.h - k; ++j) {
                step(flagsp, data + j * l_w, j);
            }
        }
    }
}

// ---- cleanup pass ----
// opj_t1_dec_clnpass_step_macro: check_flags, partial known at compile time
template <bool kCheck, bool kPartial>
__forceinline void cln_step(Dt1& t, Reg& r, u32& flags, u32* flagsp, i32* data, u32 l_w, u32 ci, i32 oph, u32 stride,
                            u32 vsc, bool partial_rt = kPartial) noexcept
{
    bool go = true;
    if (kCheck) {
        CNT(++g_counts.tested[2]);
        go = !(flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)));
        SITE(kClnElig, go);
    }
    if (go) {
        if (!partial_rt) {
            const u32 v = decide(r, &t.st[zc_ctx(t, flags >> (ci * 3U))]);
            CNT(++g_counts.dec[2]);
            SITE(kClnBit, v);
            if (!v) {
                return;
            }
        }
        sign_decode(t, r, flags, flagsp, data, l_w, ci, oph, stride, vsc);
        CNT((++g_counts.dec[2], ++g_counts.sc[2]));
    }
}

void clnpass(Dt1& t, i32 bpno, u32 vsc, u32 segsym) noexcept
{
    const i32 one = 1 << bpno, half = one >> 1, oph = one | half;
    i32* data = t.data;
    const u32 l_w = t.w, h = t.h, stride = l_w + 2;
    u32* flagsp = &t.flags[stride + 1];
    u32 k;
    Reg r = load(t);
    for (k = 0; k < (h & ~3U); k += 4, data += 3 * l_w, flagsp += 2) {
        for (u32 i = 0; i < l_w; ++i, ++data, ++flagsp) {
            u32 flags = *flagsp;
            CNT(++g_counts.cols[2]);
            SITE(kClnAgg, flags == 0);
            if (flags == 0) {
                u32 v;
#if !D_NOAGG && !D_MODEL
                {
                    // MQ-NG stage 2: a run of AGG MPS decisions without renormalisation over empty columns
                    const u32 wa = t.st[T1_CTXNO_AGG];
                    const u32 qa = wa & 0xFFFFU;
                    const u32 ra = r.a - 0x8000U, rc = r.c >> 16;
                    const u32 room = ra < rc ? ra : rc;
                    if ((wa & 0x10000U) == 0 && qa <= room) {
                        u32 used = qa, n = 1;
                        const u32 left = l_w - i;
                        while (n < left && flagsp[n] == 0 && used + qa <= room) {
                            used += qa;
                            ++n;
                        }
                        r.a -= used;
                        r.c -= used << 16;
                        i += n - 1;
                        data += n - 1;
                        flagsp += n - 1;
                        continue;
                    }
                }
#endif
                v = decide(r, &t.st[T1_CTXNO_AGG]);
                CNT((++g_counts.agg, ++g_counts.dec[2]));
                SITE(kClnAggBit, v);
                if (!v) {
                    continue;
                }
                CNT(++g_counts.cols_work[2]);
                u32 runlen = decide(r, &t.st[T1_CTXNO_UNI]);
                v = decide(r, &t.st[T1_CTXNO_UNI]);
                CNT((g_counts.uni += 2, g_counts.dec[2] += 2));
                runlen = (runlen << 1) | v;
                switch (runlen) {
                case 0:
                    cln_step<false, true>(t, r, flags, flagsp, data, l_w, 0, oph, stride, vsc);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 1, oph, stride, 0);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 2, oph, stride, 0);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
                    break;
                case 1:
                    cln_step<false, true>(t, r, flags, flagsp, data, l_w, 1, oph, stride, 0);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 2, oph, stride, 0);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
                    break;
                case 2:
                    cln_step<false, true>(t, r, flags, flagsp, data, l_w, 2, oph, stride, 0);
                    cln_step<false, false>(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
                    break;
                case 3:
                    cln_step<false, true>(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
                    break;
                }
            } else {
                CNT(++g_counts.cols_work[2]);
#if D_CLN_MASK
                u32 cand = ~((flags >> 4) | (flags >> 21)) & 0x249U;
                while (cand) {
                    const u32 b = ctz32(cand);
                    const u32 ci = (b * 11U) >> 5;
                    cand &= cand - 1U;
                    CNT(++g_counts.tested[2]);
                    const u32 v = decide(r, &t.st[zc_ctx(t, flags >> b)]);
                    CNT(++g_counts.dec[2]);
                    if (v) {
                        sign_decode(t, r, flags, flagsp, data, l_w, ci, oph, stride, vsc & (ci == 0 ? 1U : 0U));
                        CNT((++g_counts.dec[2], ++g_counts.sc[2]));
                    }
                }
#else
                cln_step<true, false>(t, r, flags, flagsp, data, l_w, 0, oph, stride, vsc);
                cln_step<true, false>(t, r, flags, flagsp, data, l_w, 1, oph, stride, 0);
                cln_step<true, false>(t, r, flags, flagsp, data, l_w, 2, oph, stride, 0);
                cln_step<true, false>(t, r, flags, flagsp, data, l_w, 3, oph, stride, 0);
#endif
            }
            *flagsp = flags & ~kPiAll;
        }
    }
    if (k < h) {
        for (u32 i = 0; i < l_w; ++i, ++flagsp, ++data) {
            for (u32 j = 0; j < h - k; ++j) {
                u32 flags = *flagsp;
                cln_step<true, false>(t, r, flags, flagsp, data + j * l_w, 0, j, oph, stride, vsc);
                *flagsp = flags;
            }
            *flagsp &= ~kPiAll;
        }
    }
    if (segsym) {
        u32 v = decide(r, &t.st[T1_CTXNO_UNI]);
        v = (v << 1) | decide(r, &t.st[T1_CTXNO_UNI]);
        v = (v << 1) | decide(r, &t.st[T1_CTXNO_UNI]);
        v = (v << 1) | decide(r, &t.st[T1_CTXNO_UNI]);
        (void)v;
    }
    save(t, r);
}

void reset_states(Dt1& t) noexcept
{
    for (u32 i = 0; i < 19; ++i) {
        t.st[i] = kWords.w[0];
    }
    t.st[T1_CTXNO_UNI] = kWords.w[46 << 1];
    t.st[T1_CTXNO_AGG] = kWords.w[3 << 1];
    t.st[T1_CTXNO_ZC] = kWords.w[4 << 1];
}

void init_common(Dt1& t, u8* bp, u32 len) noexcept
{
    t.end = bp + len;
    std::memcpy(t.backup, t.end, kExtra);
    t.end[0] = 0xFF;
    t.end[1] = 0xFF;
    t.bp = bp;
}

void init_mq(Dt1& t, u8* bp, u32 len) noexcept
{
    init_common(t, bp, len);
    t.c = len == 0 ? (0xffU << 16) : (static_cast<u32>(*t.bp) << 16);
    Reg r = load(t);
    bytein(r);
    save(t, r);
    t.c <<= 7;
    t.ct -= 7;
    t.a = 0x8000;
}

void init_raw(Dt1& t, u8* bp, u32 len) noexcept
{
    init_common(t, bp, len);
    t.c = 0;
    t.ct = 0;
}

void allocate(Dt1& t, u32 w, u32 h) noexcept
{
    std::memset(t.data, 0, std::size_t(w) * h * sizeof(i32));
    const u32 fs = w + 2U, fh = (h + 3U) / 4U, size = (fh + 2U) * fs;
    std::memset(t.flags, 0, size * sizeof(u32));
    u32* p = &t.flags[0];
    for (u32 x = 0; x < fs; ++x) {
        *p++ = kPiAll;
    }
    p = &t.flags[(fh + 1) * fs];
    for (u32 x = 0; x < fs; ++x) {
        *p++ = kPiAll;
    }
    if (h % 4) {
        u32 v = 0;
        p = &t.flags[fh * fs];
        if (h % 4 == 1) {
            v |= T1_PI_1 | T1_PI_2 | T1_PI_3;
        } else if (h % 4 == 2) {
            v |= T1_PI_2 | T1_PI_3;
        } else if (h % 4 == 3) {
            v |= T1_PI_3;
        }
        for (u32 x = 0; x < fs; ++x) {
            *p++ = v;
        }
    }
    t.w = w;
    t.h = h;
}

#if D_PASSCLK
#define DCLK() __rdtsc()
#else
#define DCLK() 0ULL
#endif

bool decode_cblk(Dt1& t, const T1Block& b) noexcept
{
    using namespace nj2k::detail;
    const u32 cblksty = b.cblksty;
    t.lutzc = kLutCtxnoZc + (b.orient << 9);
    allocate(t, b.w, b.h);
    i32 bpno_plus_one = static_cast<i32>(b.roishift + b.numbps);
    if (bpno_plus_one >= 31) {
        return false;
    }
    u32 passtype = 2;
    reset_states(t);
    CNT((++g_counts.blocks, g_counts.samples += std::size_t(b.w) * b.h, g_counts.bitplanes += b.numbps));
    if (b.nchunks == 0) {
        return true;
    }
    u8* cblkdata = t.cblkdata;
    u32 cblk_len = 0;
    for (u32 i = 0; i < b.nchunks; i++) {
        if (b.chunks[i].len) {
            std::memcpy(cblkdata + cblk_len, b.chunks[i].data, b.chunks[i].len);
        }
        cblk_len += b.chunks[i].len;
    }
    cblkdata[cblk_len] = 0;
    cblkdata[cblk_len + 1] = 0;
    const u32 vsc = (cblksty & kCblkVsc) ? 1U : 0U;
    const u32 segsym = (cblksty & kCblkSegsym) ? 1U : 0U;
    u32 index = 0;
    for (u32 segno = 0; segno < b.nsegs; ++segno) {
        const nj2k::detail::Segment* seg = &b.segs[segno];
        const bool raw = (bpno_plus_one <= static_cast<i32>(b.numbps) - 4) && (passtype < 2) && (cblksty & kCblkLazy);
        if (raw) {
            init_raw(t, cblkdata + index, seg->len);
        } else {
            init_mq(t, cblkdata + index, seg->len);
        }
        index += seg->len;
        for (u32 passno = 0; (passno < seg->real_num_passes) && (bpno_plus_one >= 1); ++passno) {
            [[maybe_unused]] const u64 p0 = DCLK();
            switch (passtype) {
            case 0:
                if (raw) {
                    sigpass_raw(t, bpno_plus_one, cblksty);
                } else {
                    sigpass_mqc(t, bpno_plus_one, vsc);
                }
                break;
            case 1:
                if (raw) {
                    refpass_raw(t, bpno_plus_one);
                } else {
                    refpass_mqc(t, bpno_plus_one);
                }
                break;
            case 2:
                clnpass(t, bpno_plus_one, vsc, segsym);
                break;
            }
#if D_PASSCLK
            g_pass_cycles[raw ? 3 : passtype] += DCLK() - p0;
#endif
            CNT(++g_counts.passes[passtype]);
            if ((cblksty & kCblkReset) && !raw) {
                reset_states(t);
            }
            if (++passtype == 3) {
                passtype = 0;
                bpno_plus_one--;
            }
        }
        std::memcpy(t.end, t.backup, kExtra);
    }
    return true;
}
