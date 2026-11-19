/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Anne Skydancer (neo-j2k)
 *
 * nj2k_cpu_supported(): whether this processor and operating system run the library as built.
 * This file is compiled for baseline x86-64 whatever the build's instruction-set floor (CI checks
 * its object for newer instructions), so a host can call it before anything else.
 *
 * NJ2K_REQUIRE_X86_64_V3 (set by the build for NJ2K_SIMD=AVX2): the x86-64-v3 level of the
 * x86-64 psABI, which -march=x86-64-v3 and /arch:AVX2 compile for: CMPXCHG16B, LAHF/SAHF, POPCNT,
 * SSE3, SSSE3, SSE4.1, SSE4.2 (v2); AVX, AVX2, BMI1, BMI2, F16C, FMA, LZCNT, MOVBE, and the OS
 * saving the YMM state (OSXSAVE with XCR0 bits 1 and 2) (v3).
 */
#include <neoj2k/neoj2k.h>

#if defined(NJ2K_REQUIRE_X86_64_V3)

#if defined(_MSC_VER)
#include <intrin.h>
static void nj2k_cpuid(unsigned int out[4], unsigned int leaf, unsigned int subleaf)
{
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    out[0] = (unsigned int)regs[0];
    out[1] = (unsigned int)regs[1];
    out[2] = (unsigned int)regs[2];
    out[3] = (unsigned int)regs[3];
}
#else
#include <cpuid.h>
static void nj2k_cpuid(unsigned int out[4], unsigned int leaf, unsigned int subleaf)
{
    __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
}
#endif

/* XCR0 through the instruction itself: the _xgetbv intrinsic needs the XSAVE target feature under
   Clang, which this baseline file must not enable. */
static unsigned long long nj2k_xcr0(void)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return _xgetbv(0);
#else
    unsigned int lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((unsigned long long)hi << 32) | lo;
#endif
}

static int nj2k_bit(unsigned int reg, unsigned int bit)
{
    return (int)((reg >> bit) & 1u);
}

NJ2K_API int NJ2K_CALLCONV nj2k_cpu_supported(void)
{
    unsigned int r[4], max_leaf, max_ext;
    unsigned int ecx1, ebx7, ecx_ext;

    nj2k_cpuid(r, 0, 0);
    max_leaf = r[0];
    if (max_leaf < 7) {
        return 0;
    }
    nj2k_cpuid(r, 1, 0);
    ecx1 = r[2];
    nj2k_cpuid(r, 7, 0);
    ebx7 = r[1];
    nj2k_cpuid(r, 0x80000000u, 0);
    max_ext = r[0];
    if (max_ext < 0x80000001u) {
        return 0;
    }
    nj2k_cpuid(r, 0x80000001u, 0);
    ecx_ext = r[2];

    /* x86-64-v2 */
    if (!nj2k_bit(ecx1, 0) || !nj2k_bit(ecx1, 9) || !nj2k_bit(ecx1, 13) || !nj2k_bit(ecx1, 19) ||
            !nj2k_bit(ecx1, 20) || !nj2k_bit(ecx1, 23) || !nj2k_bit(ecx_ext, 0)) {
        return 0;
    }
    /* x86-64-v3: FMA, MOVBE, OSXSAVE, AVX, F16C; AVX2, BMI1, BMI2; LZCNT */
    if (!nj2k_bit(ecx1, 12) || !nj2k_bit(ecx1, 22) || !nj2k_bit(ecx1, 27) || !nj2k_bit(ecx1, 28) ||
            !nj2k_bit(ecx1, 29) || !nj2k_bit(ebx7, 3) || !nj2k_bit(ebx7, 5) || !nj2k_bit(ebx7, 8) ||
            !nj2k_bit(ecx_ext, 5)) {
        return 0;
    }
    /* the operating system saves the SSE and AVX register state */
    if ((nj2k_xcr0() & 0x6u) != 0x6u) {
        return 0;
    }
    return 1;
}

#else

/* The scalar build needs nothing beyond baseline x86-64, which every x86-64 processor has. */
NJ2K_API int NJ2K_CALLCONV nj2k_cpu_supported(void)
{
    return 1;
}

#endif
