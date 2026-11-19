// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The OpenSim profile module (owner decision D10): the Second Life texture profile with 16,384 px a
// side at the decoded level, the largest side the viewer's decode guard admits (llimagej2coj.cpp),
// and the same components, layers, tiles and memory budget, so a large OpenSim texture decodes at
// the reduced levels that fit 256 MiB. Built unless NJ2K_PROFILE_OPENSIM is off (a Second Life
// build of the viewer leaves it out); without it, limit_profile_opensim_off.cpp says the profile
// is unknown.
#include "limit_profiles.h"

namespace nj2k::detail {

bool opensim_texture_limits(nj2k_decoder_limits_t* out) noexcept
{
    *out = kSecondLifeTexture;
    out->max_decoded_side = 16384;
    return true;
}

}  // namespace nj2k::detail
