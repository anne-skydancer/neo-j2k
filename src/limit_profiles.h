// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Decoder limit profiles (owner decision D10): the Second Life texture profile is built in, the
// OpenSim profile is a module of its own (limit_profile_opensim.cpp, CMake option NJ2K_PROFILE_OPENSIM).
// The Second Life numbers are those of the OpenJPEG fork's stage 1a hotfix, so both decoders refuse
// the same codestreams. Why these numbers (plan audit A1 and B2: derived from the viewer's size
// rules and from the per-tile cost, not from the texture cache):
// - 2,048 px a side at the decoded level: the viewer decodes prim textures at most 2048 x 2048
//   (llviewertexture.cpp), and asks for no larger level;
// - 8 components: the viewer's MAX_IMAGE_COMPONENTS (llimage.h); its cache holds at most 5;
// - 64 layers: the cache holds at most 8, the viewer's uploads at most 6;
// - 1,024 tiles: SL textures are one tile, and 1,024 keeps tiles x components under 16,384, the
//   bound that keeps per-tile cost to tens of MB (fork audit S2), while local tiled files (8192 x
//   8192 in 512 x 512 tiles: 256 tiles) still decode;
// - 256 MiB of working memory: the viewer's own cap on a raw image (llimage.cpp), and what a 2048 x
//   2048 5-component bake needs with room to spare (80 MiB of int32 samples).
#pragma once

#include <neoj2k/neoj2k.h>

namespace nj2k::detail {

inline constexpr nj2k_decoder_limits_t kNoLimits = {0, 0, 0, 0, 0};
inline constexpr nj2k_decoder_limits_t kSecondLifeTexture = {1024, 8, 64, 2048, 256ull << 20};

// The OpenSim module (limit_profile_opensim.cpp): false when the build leaves it out.
bool opensim_texture_limits(nj2k_decoder_limits_t* out) noexcept;

}  // namespace nj2k::detail
