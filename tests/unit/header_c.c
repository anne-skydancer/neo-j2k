/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Anne Skydancer (neo-j2k)
 *
 * The public C headers compile as strict C (C99 with GCC and Clang, C11 with MSVC, which has no
 * C99 mode) with every warning as an error. Nothing here runs.
 */
#include <neoj2k/neoj2k.h>
#include <neoj2k/neoj2k_openjpeg.h>

int nj2k_header_check_c(void);

int nj2k_header_check_c(void)
{
    nj2k_dparameters_t parameters;
    opj_image_t *image = NULL;
    nj2k_set_default_decoder_parameters(&parameters);
    parameters.cp_reduce = 1;
    if (opj_has_thread_support() == OPJ_TRUE) {
        return 1;
    }
    opj_image_destroy(image);
    return (int)parameters.cp_reduce + nj2k_cpu_supported() + (NJ2K_ENGINE_STRING_MAX > 0);
}
