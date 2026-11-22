// The study's MQ encoder for the fused passes (tools/make_fused.py), one switch a candidate step:
//   STUDY_LOCALS      the coder's words and context states copied into a local for each pass
//   STUDY_CLZ         the renormalisation by count-leading-zeros (every shift at once)
//   STUDY_IDX         a context's state as a byte index into a packed 4-byte table
//   STUDY_BRANCHLESS  the MPS/LPS interval choice by select (needs STUDY_IDX to pay; see mq_variants.cpp)
// src/t1.cpp's mq_shifts, for a in [1, 0xffff] (0 when bit 15 is set): lzcnt in the AVX2 build, bsr in
// the scalar one (MSVC's std::countl_zero would pick lzcnt at run time, which the scalar build refuses)
inline OPJ_UINT32 mq_shifts(OPJ_UINT32 a) noexcept
{
#if defined(__AVX2__)
    return _lzcnt_u32(a) - 16U;
#else
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - static_cast<OPJ_UINT32>(msb);
#endif
}

#if STUDY_IDX
// the MQ states as byte indices (state * 2 + mps): qe, next on MPS, next on LPS
struct Packed {
    std::uint16_t qe;
    std::uint8_t nmps, nlps;
};
struct PackedTable {
    Packed t[94];
    constexpr PackedTable() noexcept : t{}
    {
        for (OPJ_UINT32 i = 0; i < 94; ++i) {
            t[i].qe = static_cast<std::uint16_t>(kMqcStates[i].qeval);
            t[i].nmps = static_cast<std::uint8_t>(kMqcStates[i].nmps - kMqcStates);
            t[i].nlps = static_cast<std::uint8_t>(kMqcStates[i].nlps - kMqcStates);
        }
    }
};
constexpr PackedTable kPacked;
#endif

struct MqEnc {
    OPJ_UINT32 c, a, ct, cx;
    std::uint8_t* bp;
    std::uint8_t* start;
    std::uint8_t* limit;  // the last byte that may be written
    bool overflow;
    const std::uint8_t* lut_ctxno_zc_orient;
#if STUDY_IDX
    std::uint8_t ctxs[19];
#else
    const MqState* ctxs[19];
#endif

    void setcurctx(OPJ_UINT32 ctxno) noexcept { cx = ctxno; }
    void resetstates() noexcept
    {
        for (OPJ_UINT32 i = 0; i < 19; i++) {
#if STUDY_IDX
            ctxs[i] = 0;
#else
            ctxs[i] = kMqcStates;
#endif
        }
    }
    void setstate(OPJ_UINT32 ctxno, OPJ_UINT32 msb, OPJ_INT32 prob) noexcept
    {
#if STUDY_IDX
        ctxs[ctxno] = static_cast<std::uint8_t>(msb + static_cast<OPJ_UINT32>(prob << 1));
#else
        ctxs[ctxno] = &kMqcStates[msb + static_cast<OPJ_UINT32>(prob << 1)];
#endif
    }
    void init_enc(std::uint8_t* buf, std::uint8_t* last) noexcept
    {
        cx = 0;
        a = 0x8000;
        c = 0;
        bp = buf - 1;
        ct = 12;
        start = buf;
        limit = last;
        overflow = false;
    }
    OPJ_UINT32 numbytes() const noexcept { return static_cast<OPJ_UINT32>(bp - start); }

    void byteout() noexcept
    {
        if (bp >= limit) {
            // OpenJPEG would write past the buffer here (OD-4); neo-j2k stops the code-block
            overflow = true;
            bp = limit - 1;
        }
        if (*bp == 0xff) {
            bp++;
            *bp = static_cast<std::uint8_t>(c >> 20);
            c &= 0xfffff;
            ct = 7;
        } else if ((c & 0x8000000) == 0) {
            bp++;
            *bp = static_cast<std::uint8_t>(c >> 19);
            c &= 0x7ffff;
            ct = 8;
        } else {
            (*bp)++;
            if (*bp == 0xff) {
                c &= 0x7ffffff;
                bp++;
                *bp = static_cast<std::uint8_t>(c >> 20);
                c &= 0xfffff;
                ct = 7;
            } else {
                bp++;
                *bp = static_cast<std::uint8_t>(c >> 19);
                c &= 0x7ffff;
                ct = 8;
            }
        }
    }

    __forceinline void renorme() noexcept
    {
#if STUDY_CLZ
        // every shift at once; the byte-outs where mqc.c's one-bit loop has them
        OPJ_UINT32 k = mq_shifts(a);
        a <<= k;
        while (k >= ct) {
            c <<= ct;
            k -= ct;
            byteout();
        }
        c <<= k;
        ct -= k;
#else
        do {
            a <<= 1;
            c <<= 1;
            ct--;
            if (ct == 0) {
                byteout();
            }
        } while ((a & 0x8000) == 0);
#endif
    }

    __forceinline void encode(OPJ_UINT32 d) noexcept
    {
#if STUDY_IDX
        const OPJ_UINT32 i = ctxs[cx];
        const Packed e = kPacked.t[i];
        const OPJ_UINT32 q = e.qe;
        const OPJ_UINT32 lps = (i ^ d) & 1U;
#else
        const MqState* st = ctxs[cx];
        const OPJ_UINT32 q = st->qeval;
        const OPJ_UINT32 lps = st->mps ^ d;
#endif
#if STUDY_BRANCHLESS
        // the encoder knows its bit: with a' = a - qe and x = (a' < qe), the MPS takes qe as its interval
        // when x (the conditional exchange) and adds qe to c otherwise, the LPS the opposite; an MPS whose
        // a' keeps bit 15 needs no renormalisation and keeps its state
        const OPJ_UINT32 an = a - q;
        const OPJ_UINT32 swap = static_cast<OPJ_UINT32>(an < q) ^ lps;
        a = swap ? q : an;
        c += swap ? 0U : q;
#if STUDY_IDX
        const OPJ_UINT32 keep = (lps | (an & 0x8000U)) == 0x8000U ? 1U : 0U;
        const OPJ_UINT32 next = lps ? e.nlps : e.nmps;
        ctxs[cx] = static_cast<std::uint8_t>(keep ? i : next);
#else
        const bool keep = !lps && (an & 0x8000);
        const MqState* next = lps ? st->nlps : st->nmps;
        ctxs[cx] = keep ? st : next;
#endif
#if STUDY_CLZ
        renorme();  // a shift of 0 when bit 15 is kept
#else
        if ((a & 0x8000) == 0) {
            renorme();
        }
#endif
#else
        a -= q;
        if (!lps) {
            if (a & 0x8000) {
                c += q;
                return;
            }
            if (a < q) {
                a = q;
            } else {
                c += q;
            }
#if STUDY_IDX
            ctxs[cx] = e.nmps;
#else
            ctxs[cx] = st->nmps;
#endif
        } else {
            if (a < q) {
                c += q;
            } else {
                a = q;
            }
#if STUDY_IDX
            ctxs[cx] = e.nlps;
#else
            ctxs[cx] = st->nlps;
#endif
        }
        renorme();
#endif
    }

    void flush() noexcept
    {
        const OPJ_UINT32 tempc = c + a;
        c |= 0xffff;
        if (c >= tempc) {
            c -= 0x8000;
        }
        c <<= ct;
        byteout();
        c <<= ct;
        byteout();
        // it is forbidden that a coding pass ends with 0xff
        if (*bp != 0xff) {
            bp++;
        }
    }
};
using Mq = MqEnc;

