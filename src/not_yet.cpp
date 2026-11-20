// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Anne Skydancer (neo-j2k)
//
// The parts of the API that later stages implement, and the few that neo-j2k answers with a fixed
// value. Each fails cleanly (nullptr or NJ2K_FALSE, no side effects), so the export list and the ABI
// checks cover the whole API from stage 0. The stage that replaces each group is named; the plan is
// in doc/stages.md. The decoder's functions live in codec.cpp and stream.cpp.
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

// ---- threads ------------------------------------------------------------------------------------

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

// ---- codestream index and dumps -----------------------------------------------------------------

// No codestream index is built (fork audit P5: OpenJPEG builds one on every decode for this call).
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

// OpenJPEG's opj_dump prints through this; neo-j2k prints nothing.
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

// ---- encoding ----------------------------------------------------------------------------------

// The encoder is in encode.cpp (stage 6a). Writing a tile at a time belongs to the j2k-tile module.

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
