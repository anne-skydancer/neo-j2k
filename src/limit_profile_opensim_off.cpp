// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Builds without the OpenSim profile module (NJ2K_PROFILE_OPENSIM off): the profile is unknown.
#include "limit_profiles.h"

namespace nj2k::detail {

bool opensim_texture_limits(nj2k_decoder_limits_t*) noexcept
{
    return false;
}

}  // namespace nj2k::detail
