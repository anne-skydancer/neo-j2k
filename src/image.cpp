// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Image objects: nj2k_image_create, nj2k_image_tile_create, nj2k_image_destroy and the sample-plane
// allocation pair. Ported from OpenJPEG 2.5.4 (image.c, openjpeg.c) with the same behaviour,
// including the failures: a component with no samples, or no components, gives no image.
// OpenJPEG's notices follow.
//
/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2008, 2011-2012, Centre National d'Etudes Spatiales (CNES), FR
 * Copyright (c) 2012, CS Systemes d'Information, France
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <neoj2k/neoj2k.h>

#include <cstdint>
#include <cstring>

#include "memory.h"

using nj2k::detail::aligned_alloc;
using nj2k::detail::aligned_free;
using nj2k::detail::heap_calloc;
using nj2k::detail::heap_free;

namespace {

void copy_component_parameters(nj2k_image_comp_t* comp, const nj2k_image_cmptparm_t* parm) noexcept
{
    comp->dx = parm->dx;
    comp->dy = parm->dy;
    comp->w = parm->w;
    comp->h = parm->h;
    comp->x0 = parm->x0;
    comp->y0 = parm->y0;
    comp->prec = parm->prec;
    comp->sgnd = parm->sgnd;
}

nj2k_image_t* new_image(NJ2K_UINT32 numcmpts, NJ2K_COLOR_SPACE clrspc) noexcept
{
    auto* image = static_cast<nj2k_image_t*>(heap_calloc(1, sizeof(nj2k_image_t)));
    if (!image) {
        return nullptr;
    }
    image->color_space = clrspc;
    image->numcomps = numcmpts;
    image->comps = static_cast<nj2k_image_comp_t*>(heap_calloc(numcmpts, sizeof(nj2k_image_comp_t)));
    if (!image->comps) {
        nj2k_image_destroy(image);
        return nullptr;
    }
    return image;
}

}  // namespace

extern "C" {

NJ2K_API void* NJ2K_CALLCONV nj2k_image_data_alloc(NJ2K_SIZE_T size)
{
    return aligned_alloc(size);
}

NJ2K_API void NJ2K_CALLCONV nj2k_image_data_free(void* ptr)
{
    aligned_free(ptr);
}

NJ2K_API nj2k_image_t* NJ2K_CALLCONV nj2k_image_create(NJ2K_UINT32 numcmpts, nj2k_image_cmptparm_t* cmptparms,
                                                       NJ2K_COLOR_SPACE clrspc)
{
    nj2k_image_t* image = new_image(numcmpts, clrspc);
    if (!image) {
        return nullptr;
    }
    for (NJ2K_UINT32 compno = 0; compno < numcmpts; ++compno) {
        nj2k_image_comp_t* comp = &image->comps[compno];
        copy_component_parameters(comp, &cmptparms[compno]);
        if (comp->h != 0 && static_cast<NJ2K_SIZE_T>(comp->w) > SIZE_MAX / comp->h / sizeof(NJ2K_INT32)) {
            nj2k_image_destroy(image);
            return nullptr;
        }
        const NJ2K_SIZE_T bytes = static_cast<NJ2K_SIZE_T>(comp->w) * comp->h * sizeof(NJ2K_INT32);
        comp->data = static_cast<NJ2K_INT32*>(aligned_alloc(bytes));
        if (!comp->data) {
            nj2k_image_destroy(image);
            return nullptr;
        }
        std::memset(comp->data, 0, bytes);
    }
    return image;
}

NJ2K_API nj2k_image_t* NJ2K_CALLCONV nj2k_image_tile_create(NJ2K_UINT32 numcmpts, nj2k_image_cmptparm_t* cmptparms,
                                                            NJ2K_COLOR_SPACE clrspc)
{
    nj2k_image_t* image = new_image(numcmpts, clrspc);
    if (!image) {
        return nullptr;
    }
    for (NJ2K_UINT32 compno = 0; compno < numcmpts; ++compno) {
        copy_component_parameters(&image->comps[compno], &cmptparms[compno]);
        image->comps[compno].data = nullptr;
    }
    return image;
}

NJ2K_API void NJ2K_CALLCONV nj2k_image_destroy(nj2k_image_t* image)
{
    if (!image) {
        return;
    }
    if (image->comps) {
        for (NJ2K_UINT32 compno = 0; compno < image->numcomps; ++compno) {
            aligned_free(image->comps[compno].data);
        }
        heap_free(image->comps);
    }
    heap_free(image->icc_profile_buf);
    heap_free(image);
}

}  // extern "C"
