/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Anne Skydancer (neo-j2k)
 *
 * The bench's allocation counter for the oracle: OpenJPEG's own allocation entry points
 * (opj_malloc.c, unchanged and included below) renamed, and counting wrappers in their place.
 * Built only into the counting oracle module (nj2k_oracle_counting), never into the oracle the
 * tests compare against. Every OpenJPEG heap call goes through these entry points.
 */
#define opj_malloc opj_malloc_uncounted
#define opj_calloc opj_calloc_uncounted
#define opj_realloc opj_realloc_uncounted
#define opj_free opj_free_uncounted
#define opj_aligned_malloc opj_aligned_malloc_uncounted
#define opj_aligned_realloc opj_aligned_realloc_uncounted
#define opj_aligned_32_malloc opj_aligned_32_malloc_uncounted
#define opj_aligned_32_realloc opj_aligned_32_realloc_uncounted
#define opj_aligned_free opj_aligned_free_uncounted
#include "opj_malloc.c"
#undef opj_malloc
#undef opj_calloc
#undef opj_realloc
#undef opj_free
#undef opj_aligned_malloc
#undef opj_aligned_realloc
#undef opj_aligned_32_malloc
#undef opj_aligned_32_realloc
#undef opj_aligned_free

#if defined(_WIN32)
#define NJ2K_COUNT_API __declspec(dllexport)
#else
#define NJ2K_COUNT_API __attribute__((visibility("default")))
#endif

/* allocations (malloc, calloc and the aligned forms), reallocations, frees, bytes requested */
static unsigned long long g_nj2k_counts[4];

void *opj_malloc(size_t size)
{
    ++g_nj2k_counts[0];
    g_nj2k_counts[3] += size;
    return opj_malloc_uncounted(size);
}
void *opj_calloc(size_t num, size_t size)
{
    ++g_nj2k_counts[0];
    g_nj2k_counts[3] += num * size;
    return opj_calloc_uncounted(num, size);
}
void *opj_aligned_malloc(size_t size)
{
    ++g_nj2k_counts[0];
    g_nj2k_counts[3] += size;
    return opj_aligned_malloc_uncounted(size);
}
void *opj_aligned_32_malloc(size_t size)
{
    ++g_nj2k_counts[0];
    g_nj2k_counts[3] += size;
    return opj_aligned_32_malloc_uncounted(size);
}
void *opj_realloc(void *ptr, size_t size)
{
    ++g_nj2k_counts[1];
    g_nj2k_counts[3] += size;
    return opj_realloc_uncounted(ptr, size);
}
void *opj_aligned_realloc(void *ptr, size_t size)
{
    ++g_nj2k_counts[1];
    g_nj2k_counts[3] += size;
    return opj_aligned_realloc_uncounted(ptr, size);
}
void *opj_aligned_32_realloc(void *ptr, size_t size)
{
    ++g_nj2k_counts[1];
    g_nj2k_counts[3] += size;
    return opj_aligned_32_realloc_uncounted(ptr, size);
}
void opj_free(void *ptr)
{
    if (ptr) {
        ++g_nj2k_counts[2];
    }
    opj_free_uncounted(ptr);
}
void opj_aligned_free(void *ptr)
{
    if (ptr) {
        ++g_nj2k_counts[2];
    }
    opj_aligned_free_uncounted(ptr);
}

NJ2K_COUNT_API void nj2k_oracle_alloc_counts(unsigned long long out[4])
{
    int i;
    for (i = 0; i < 4; ++i) {
        out[i] = g_nj2k_counts[i];
    }
}
NJ2K_COUNT_API void nj2k_oracle_alloc_reset(void)
{
    int i;
    for (i = 0; i < 4; ++i) {
        g_nj2k_counts[i] = 0;
    }
}
