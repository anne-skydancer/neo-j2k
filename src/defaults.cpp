// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// Parameter defaults, identical to OpenJPEG 2.5.4's (openjpeg.c, opj_common.h): the unit tests
// compare both structures with the oracle's byte for byte. OpenJPEG's notice follows.
//
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

#include <cstring>

extern "C" {

NJ2K_API void NJ2K_CALLCONV nj2k_set_default_decoder_parameters(nj2k_dparameters_t* parameters)
{
    if (!parameters) {
        return;
    }
    std::memset(parameters, 0, sizeof(nj2k_dparameters_t));
    parameters->cp_layer = 0;
    parameters->cp_reduce = 0;
    parameters->decod_format = -1;
    parameters->cod_format = -1;
    parameters->flags = 0;
}

NJ2K_API void NJ2K_CALLCONV nj2k_set_default_encoder_parameters(nj2k_cparameters_t* parameters)
{
    if (!parameters) {
        return;
    }
    std::memset(parameters, 0, sizeof(nj2k_cparameters_t));
    parameters->cp_cinema = NJ2K_OFF;
    parameters->rsiz = NJ2K_PROFILE_NONE;
    parameters->max_comp_size = 0;
    parameters->numresolution = 6;      // OPJ_COMP_PARAM_DEFAULT_NUMRESOLUTION
    parameters->cp_rsiz = NJ2K_STD_RSIZ;
    parameters->cblockw_init = 64;      // OPJ_COMP_PARAM_DEFAULT_CBLOCKW
    parameters->cblockh_init = 64;      // OPJ_COMP_PARAM_DEFAULT_CBLOCKH
    parameters->prog_order = NJ2K_LRCP; // OPJ_COMP_PARAM_DEFAULT_PROG_ORDER
    parameters->roi_compno = -1;
    parameters->subsampling_dx = 1;
    parameters->subsampling_dy = 1;
    parameters->tp_on = 0;
    parameters->decod_format = -1;
    parameters->cod_format = -1;
    parameters->tcp_rates[0] = 0;
    parameters->tcp_numlayers = 0;
    parameters->cp_disto_alloc = 0;
    parameters->cp_fixed_alloc = 0;
    parameters->cp_fixed_quality = 0;
    parameters->jpip_on = NJ2K_FALSE;
}

}  // extern "C"
