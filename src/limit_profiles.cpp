// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
#include "limit_profiles.h"

extern "C" {

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_limits_init(nj2k_decoder_limits_t* p_limits, NJ2K_UINT32 profile)
{
    if (!p_limits) {
        return NJ2K_FALSE;
    }
    switch (profile) {
    case NJ2K_DECODER_LIMITS_NONE:
        *p_limits = nj2k::detail::kNoLimits;
        return NJ2K_TRUE;
    case NJ2K_DECODER_LIMITS_SL_TEXTURE:
        *p_limits = nj2k::detail::kSecondLifeTexture;
        return NJ2K_TRUE;
    case NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE:
        if (nj2k::detail::opensim_texture_limits(p_limits)) {
            return NJ2K_TRUE;
        }
        break;
    default:
        break;
    }
    *p_limits = nj2k::detail::kNoLimits;
    return NJ2K_FALSE;
}

}  // extern "C"
