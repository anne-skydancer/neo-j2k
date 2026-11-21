// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The default floating-point environment for the library's own float work. The 9/7 wavelet, the
// irreversible colour transform and the quantiser round their results, so the samples depend on the
// rounding mode (and on flush-to-zero / denormals-are-zero) of the thread that computes them. A host's
// threads may keep another environment (game, audio and physics threads do), so every API call that
// computes, and every job the library hands to a host's executor, runs under round to nearest-even with
// FTZ and DAZ off, and puts the caller's environment back on every exit path. The output is therefore
// independent of the host's floating-point environment.
//
// Only the control bits are touched (x86-64: MXCSR rounding control, FZ and DAZ; the exception masks and
// flags stay the caller's), once per API call and once per executor job, never per sample, and the
// register is written only when it differs from the default.
#pragma once

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#else
#include <cfenv>
#endif

namespace nj2k::detail {

class DefaultFpEnv {
public:
    DefaultFpEnv() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        saved_ = _mm_getcsr();
        if ((saved_ & kControl) != 0) {
            _mm_setcsr(saved_ & ~kControl);
            changed_ = true;
        }
#else
        // Elsewhere only the rounding mode is set (fesetround); flush-to-zero has no portable control.
        saved_ = std::fegetround();
        if (saved_ != FE_TONEAREST) {
            std::fesetround(FE_TONEAREST);
            changed_ = true;
        }
#endif
    }
    ~DefaultFpEnv()
    {
        if (changed_) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            // the caller's control bits back; the flags the work raised are kept
            _mm_setcsr((_mm_getcsr() & ~kControl) | (saved_ & kControl));
#else
            std::fesetround(saved_);
#endif
        }
    }
    DefaultFpEnv(const DefaultFpEnv&) = delete;
    DefaultFpEnv& operator=(const DefaultFpEnv&) = delete;

private:
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // RC (bits 13-14), FZ (bit 15), DAZ (bit 6)
    static constexpr unsigned kControl = (3u << 13) | (1u << 15) | (1u << 6);
    unsigned saved_;
#else
    int saved_;
#endif
    bool changed_ = false;
};

}  // namespace nj2k::detail
