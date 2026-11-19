// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The parts of the API that later stages implement. Each fails cleanly today (nullptr or
// NJ2K_FALSE, no side effects), so the export list and the ABI checks cover the whole API from
// stage 0. The stage that replaces each group is named; the plan is in doc/stages.md.
#include <neoj2k/neoj2k.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" {

// ---- streams (stage 1b) ----------------------------------------------------------------------

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_default_create(NJ2K_BOOL)
{
    return nullptr;
}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create(NJ2K_SIZE_T, NJ2K_BOOL)
{
    return nullptr;
}

NJ2K_API void NJ2K_CALLCONV nj2k_stream_destroy(nj2k_stream_t*) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_read_function(nj2k_stream_t*, nj2k_stream_read_fn) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_write_function(nj2k_stream_t*, nj2k_stream_write_fn) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_skip_function(nj2k_stream_t*, nj2k_stream_skip_fn) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_seek_function(nj2k_stream_t*, nj2k_stream_seek_fn) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data(nj2k_stream_t*, void*, nj2k_stream_free_user_data_fn) {}
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data_length(nj2k_stream_t*, NJ2K_UINT64) {}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_default_file_stream(const char*, NJ2K_BOOL)
{
    return nullptr;
}

NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_file_stream(const char*, NJ2K_SIZE_T, NJ2K_BOOL)
{
    return nullptr;
}

// ---- codec and messages (stage 1b) -------------------------------------------------------------

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_info_handler(nj2k_codec_t*, nj2k_msg_callback, void*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_warning_handler(nj2k_codec_t*, nj2k_msg_callback, void*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_error_handler(nj2k_codec_t*, nj2k_msg_callback, void*)
{
    return NJ2K_FALSE;
}

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_codec(nj2k_codec_t*) {}

// The library starts no threads of its own (plan T2): a host executor runs large work instead.
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_codec_set_threads(nj2k_codec_t*, int)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_has_thread_support(void)
{
    return NJ2K_FALSE;
}

// The operating system's count of logical processors: on Windows those of every processor group
// (OpenJPEG's GetSystemInfo counts the calling thread's group only, which differs above 64 logical
// processors), elsewhere the online ones, as OpenJPEG. No standard library here: <thread> brings
// MSVC's STL a writable global into the object.
NJ2K_API int NJ2K_CALLCONV nj2k_get_num_cpus(void)
{
#if defined(_WIN32)
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? static_cast<int>(n) : 1;
}

// ---- decoding (stages 1b and 1c) ---------------------------------------------------------------

NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_decompress(NJ2K_CODEC_FORMAT)
{
    return nullptr;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_decoder(nj2k_codec_t*, nj2k_dparameters_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_strict_mode(nj2k_codec_t*, NJ2K_BOOL)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_header(nj2k_stream_t*, nj2k_codec_t*, nj2k_image_t** p_image)
{
    if (p_image) {
        *p_image = nullptr;
    }
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_components(nj2k_codec_t*, NJ2K_UINT32, const NJ2K_UINT32*, NJ2K_BOOL)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decode_area(nj2k_codec_t*, nj2k_image_t*, NJ2K_INT32, NJ2K_INT32, NJ2K_INT32,
                                                     NJ2K_INT32)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode(nj2k_codec_t*, nj2k_stream_t*, nj2k_image_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_decompress(nj2k_codec_t*, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_decoded_tile(nj2k_codec_t*, nj2k_stream_t*, nj2k_image_t*, NJ2K_UINT32)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_resolution_factor(nj2k_codec_t*, NJ2K_UINT32)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_tile_header(nj2k_codec_t*, nj2k_stream_t*, NJ2K_UINT32*, NJ2K_UINT32*,
                                                      NJ2K_INT32*, NJ2K_INT32*, NJ2K_INT32*, NJ2K_INT32*,
                                                      NJ2K_UINT32*, NJ2K_BOOL*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode_tile_data(nj2k_codec_t*, NJ2K_UINT32, NJ2K_BYTE*, NJ2K_UINT32, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API nj2k_codestream_info_v2_t* NJ2K_CALLCONV nj2k_get_cstr_info(nj2k_codec_t*)
{
    return nullptr;
}

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_cstr_info(nj2k_codestream_info_v2_t** cstr_info)
{
    if (cstr_info) {
        *cstr_info = nullptr;
    }
}

NJ2K_API nj2k_codestream_index_t* NJ2K_CALLCONV nj2k_get_cstr_index(nj2k_codec_t*)
{
    return nullptr;
}

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_cstr_index(nj2k_codestream_index_t** p_cstr_index)
{
    if (p_cstr_index) {
        *p_cstr_index = nullptr;
    }
}

NJ2K_API void NJ2K_CALLCONV nj2k_dump_codec(nj2k_codec_t*, NJ2K_INT32, FILE*) {}

// OpenJPEG declares these two and never defines them (its DLL lacks them). neo-j2k defines them:
// there is no JP2 metadata or index object, so the answer is always nullptr.
NJ2K_API nj2k_jp2_metadata_t* NJ2K_CALLCONV nj2k_get_jp2_metadata(nj2k_codec_t*)
{
    return nullptr;
}

NJ2K_API nj2k_jp2_index_t* NJ2K_CALLCONV nj2k_get_jp2_index(nj2k_codec_t*)
{
    return nullptr;
}

// ---- encoding (stage 6a) ------------------------------------------------------------------------

NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_compress(NJ2K_CODEC_FORMAT)
{
    return nullptr;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_encoder(nj2k_codec_t*, nj2k_cparameters_t*, nj2k_image_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encoder_set_extra_options(nj2k_codec_t*, const char* const*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_start_compress(nj2k_codec_t*, nj2k_image_t*, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encode(nj2k_codec_t*, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_compress(nj2k_codec_t*, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_write_tile(nj2k_codec_t*, NJ2K_UINT32, NJ2K_BYTE*, NJ2K_UINT32, nj2k_stream_t*)
{
    return NJ2K_FALSE;
}

// Custom multiple-component transforms are Part 2, which neo-j2k does not implement (plan D7).
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_MCT(nj2k_cparameters_t*, NJ2K_FLOAT32*, NJ2K_INT32*, NJ2K_UINT32)
{
    return NJ2K_FALSE;
}

}  // extern "C"
