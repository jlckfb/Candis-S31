/*
 * Candis-S31 simulator - heap_caps shim.
 *
 * Host malloc backs every capability; the size queries return fixed
 * ESP32-S31-like figures (32 MB PSRAM, ~500 KB internal) so the sysinfo
 * page shows realistic numbers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_DMA      (1u << 10)
#define MALLOC_CAP_SPIRAM   (1u << 11)
#define MALLOC_CAP_INTERNAL (1u << 12)

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        return 30u * 1024u * 1024u;
    }
    return 88u * 1024u;
}

static inline size_t heap_caps_get_total_size(uint32_t caps)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        return 32u * 1024u * 1024u;
    }
    return 503u * 1024u;
}

#ifdef __cplusplus
}
#endif
