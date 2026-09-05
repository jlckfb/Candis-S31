/*
 * Candis-S31 watch demo - MEMORY domain test suite (spec D.1).
 *
 * Ports the factory flash-size, PSRAM pattern, PSRAM alias probe and
 * memory-bandwidth checks. The alias probe uses the non-destructive
 * probe mode only (factory_diag.c:245-322): every probed address sits
 * inside one owned allocation and each touched cache line is saved and
 * restored, so the running system is never corrupted; the destructive
 * full mode is deliberately not ported (spec D.3).
 *
 * All functions run on the svc_test task; no LVGL calls (C.7). Large
 * buffers come from PSRAM and are freed on every exit path (C.7 memory
 * discipline).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "test_registry.h"

#define EXPECTED_FLASH_SIZE (16U * 1024U * 1024U)
#define PSRAM_TEST_SIZE     (64U * 1024U)
#define PSRAM_PATTERN_BASE  UINT32_C(0xa5a50000)

static void result_fail(test_result_t *out, const char *evidence)
{
    out->st = TEST_ST_FAIL;
    strlcpy(out->evidence, evidence, sizeof(out->evidence));
}

/* ------------------------------------------------------------------ */
/* mem.flash_size (factory_console.c:808-830)                          */
/* ------------------------------------------------------------------ */

static void run_flash_size(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    uint32_t flash_size = 0;
    const esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        result_fail(out, "flash size read failed");
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "size=%" PRIu32 " expected=%u", flash_size,
             EXPECTED_FLASH_SIZE);
    out->st = flash_size == EXPECTED_FLASH_SIZE ? TEST_ST_PASS : TEST_ST_FAIL;
}

/* ------------------------------------------------------------------ */
/* mem.psram_pattern (factory_console.c:831-894)                       */
/* ------------------------------------------------------------------ */

static void run_psram_pattern(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    if (!esp_psram_is_initialized()) {
        result_fail(out, "PSRAM is not initialized");
        return;
    }
    uint32_t *buffer = heap_caps_malloc(PSRAM_TEST_SIZE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        result_fail(out, "64 KiB allocation failed");
        return;
    }

    const size_t words = PSRAM_TEST_SIZE / sizeof(*buffer);
    for (size_t index = 0; index < words; ++index) {
        buffer[index] = PSRAM_PATTERN_BASE ^ (uint32_t)index;
    }
    size_t failed_index = words;
    for (size_t index = 0; index < words; ++index) {
        if (buffer[index] != (PSRAM_PATTERN_BASE ^ (uint32_t)index)) {
            failed_index = index;
            break;
        }
    }
    free(buffer);

    if (failed_index != words) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "mismatch_at_word=%zu", failed_index);
        out->st = TEST_ST_FAIL;
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence), "tested=%u total=%zu",
             PSRAM_TEST_SIZE, esp_psram_get_size());
    out->st = TEST_ST_PASS;
}

/* ------------------------------------------------------------------ */
/* mem.psram_alias_probe (factory_diag.c:150-322, probe mode only)     */
/*                                                                     */
/* An octal PSRAM die ignores address bits above its physical depth,   */
/* so a 16 MB part mapped as 32 MB wraps: X and X+16MB alias one cell. */
/* Every read is forced past the cache with esp_cache_msync so a stale */
/* line can never masquerade as the physical cell.                     */
/* ------------------------------------------------------------------ */

#define PSRAM_VERIFY_MAGIC_LOW  UINT32_C(0xc0ffee11)
#define PSRAM_VERIFY_MAGIC_HIGH UINT32_C(0x5afe2002)
#define PSRAM_VERIFY_MAX_LINE   128U

static size_t probe_line_size(const void *addr)
{
    const size_t line = esp_cache_get_line_size_by_addr(addr);
    return (line >= 16U && line <= PSRAM_VERIFY_MAX_LINE) ? line : 64U;
}

/* Write back then invalidate the line holding addr: the write reaches the
 * physical cell (wrapping with it, if the chip wraps) and the next read
 * misses the cache. */
static void probe_publish(const void *addr, size_t line)
{
    const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1U);
    esp_cache_msync((void *)start, line,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static void probe_reload(const void *addr, size_t line)
{
    const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1U);
    esp_cache_msync((void *)start, line, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

/* One save -> write -> observe -> restore round across a (low, high)
 * virtual pair. Both addresses must sit inside the caller's own
 * allocation: on a wrapping chip the high write then lands on a cell the
 * allocation already owns, keeping the probe non-destructive. Returns
 * true when the two addresses alias one physical cell. */
static bool probe_pair_aliases(uint32_t *low, uint32_t *high, size_t line)
{
    volatile uint32_t *const vlow = low;
    volatile uint32_t *const vhigh = high;
    const uintptr_t low_line = (uintptr_t)low & ~(uintptr_t)(line - 1U);
    const uintptr_t high_line = (uintptr_t)high & ~(uintptr_t)(line - 1U);
    uint8_t low_save[PSRAM_VERIFY_MAX_LINE];
    uint8_t high_save[PSRAM_VERIFY_MAX_LINE];

    /* Start from clean lines so both saves read the physical cells. */
    probe_publish(low, line);
    probe_publish(high, line);
    memcpy(low_save, (const void *)low_line, line);
    memcpy(high_save, (const void *)high_line, line);

    /* Direction 1: write through low, then look through high. */
    *vlow = PSRAM_VERIFY_MAGIC_LOW;
    probe_publish(low, line);
    probe_reload(high, line);
    const bool low_to_high = (*vhigh == PSRAM_VERIFY_MAGIC_LOW);
    memcpy((void *)low_line, low_save, line);
    probe_publish(low, line);

    /* Direction 2: write through high, then look through low. A high write
     * that changes the low read is the wrap signature. */
    *vhigh = PSRAM_VERIFY_MAGIC_HIGH;
    probe_publish(high, line);
    probe_reload(low, line);
    const bool high_to_low = (*vlow == PSRAM_VERIFY_MAGIC_HIGH);

    /* Restore both lines. On a wrapping chip they are the same physical
     * cell, and the two saves hold identical content (equal line offset,
     * read before any magic write), so the order is irrelevant. */
    memcpy((void *)low_line, low_save, line);
    probe_publish(low, line);
    memcpy((void *)high_line, high_save, line);
    probe_publish(high, line);

    return low_to_high || high_to_low;
}

static void run_psram_alias_probe(const test_ctx_t *ctx, test_result_t *out)
{
    (void)ctx;
    static const size_t anchors[] = {1U << 20, 4U << 20, 12U << 20};
    static const size_t deltas[] = {8U << 20, 16U << 20};
    const size_t min_window = anchors[0] + deltas[1] + PSRAM_VERIFY_MAX_LINE;

    size_t size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (size < min_window) {
        out->st = TEST_ST_WARN;
        snprintf(out->evidence, sizeof(out->evidence),
                 "inconclusive: free block %zu < %zu", size, min_window);
        return;
    }
    /* The window must be one owned allocation: every probed address then
     * belongs to this test, even after a wrap. */
    uint32_t *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
                                       MALLOC_CAP_8BIT);
    if (block == NULL) {
        size = min_window;
        block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (block == NULL) {
        out->st = TEST_ST_WARN;
        strlcpy(out->evidence, "probe window allocation failed",
                sizeof(out->evidence));
        return;
    }
    const size_t line = probe_line_size(block);

    unsigned tested16 = 0;
    unsigned aliased = 0;
    for (size_t d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
        for (size_t a = 0; a < sizeof(anchors) / sizeof(anchors[0]); ++a) {
            if (anchors[a] + deltas[d] + line > size) {
                continue;
            }
            uint32_t *low = (uint32_t *)((uint8_t *)block + anchors[a]);
            uint32_t *high = (uint32_t *)((uint8_t *)block + anchors[a] +
                                          deltas[d]);
            if (probe_pair_aliases(low, high, line)) {
                ++aliased;
            }
            if (deltas[d] == (16U << 20)) {
                ++tested16;
            }
        }
    }
    heap_caps_free(block);

    if (aliased > 0) {
        /* A wrap means the reported map aliases: capacity verdict fail. */
        snprintf(out->evidence, sizeof(out->evidence),
                 "ALIASED: %u alias pair(s), reported=%zu",
                 aliased, esp_psram_get_size());
        out->st = TEST_ST_FAIL;
    } else if (tested16 > 0) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "no alias in %u +16MB pair(s), size=%zu",
                 tested16, esp_psram_get_size());
        out->st = TEST_ST_PASS;
    } else {
        out->st = TEST_ST_WARN;
        strlcpy(out->evidence, "no +16MB pair fit the window",
                sizeof(out->evidence));
    }
}

/* ------------------------------------------------------------------ */
/* mem.bandwidth (factory_diag.c:451-568)                              */
/* Numbers are evidence only - no hard thresholds (spec F10: they move  */
/* with the flash/PSRAM clock profile).                                */
/*                                                                     */
/* Window policy: demo runtime keeps display/network/audio services    */
/* resident; they allocate continuously, so the largest PSRAM block    */
/* shrinks below the 8 MiB cap and races this task. Sizing the window  */
/* as "min(largest block, 8 MiB)" (factory_diag) is nearly the whole   */
/* block once fragmented and fails on multi-heap bookkeeping alone.    */
/* Here the window is the largest rung of a fixed ladder that places,  */
/* floored at MEM_BW_SPAN_MIN (far above the S3 data cache, so numbers */
/* stay representative; the floor bounds allocation policy only, spec  */
/* F10 performance numbers stay evidence-only). Real pressure below    */
/* the floor still FAILs.                                              */
/* ------------------------------------------------------------------ */

#define MEM_BW_CHUNK_MAX    (64U * 1024U)
#define MEM_BW_CHUNK_MIN    (16U * 1024U)
#define MEM_BW_SPAN_MAX     (8U * 1024U * 1024U)
#define MEM_BW_SPAN_MIN     (256U * 1024U)
#define MEM_BW_ALLOC_TRIES  3
#define MEM_BW_FLASH_BYTES  (4U * 1024U * 1024U)
#define MEM_BW_FLASH_OFFSET (1024U * 1024U)

static double bw_mbps(size_t bytes, int64_t elapsed_us)
{
    return elapsed_us > 0 ? (double)bytes / (double)elapsed_us : 0.0;
}

/* Fixed-size ladder allocator: takes the largest rung that places,
 * retrying each rung MEM_BW_ALLOC_TRIES times ~10 ms apart to absorb
 * transient allocations from resident services. Deterministic given
 * the heap state; NULL only when even the smallest rung cannot be
 * placed (real memory pressure). */
static void *bw_ladder_alloc(const size_t *rungs, size_t n_rungs,
                             uint32_t caps, size_t *size_out)
{
    for (size_t r = 0; r < n_rungs; ++r) {
        for (int tries = 0; tries < MEM_BW_ALLOC_TRIES; ++tries) {
            void *p = heap_caps_malloc(rungs[r], caps);
            if (p != NULL) {
                *size_out = rungs[r];
                return p;
            }
            if (tries + 1 < MEM_BW_ALLOC_TRIES) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    return NULL;
}

static void run_mem_bandwidth(const test_ctx_t *ctx, test_result_t *out)
{
    if (!esp_psram_is_initialized()) {
        result_fail(out, "PSRAM not initialized this boot");
        return;
    }

    static const size_t win_rungs[] = {
        MEM_BW_SPAN_MAX, 4U * 1024U * 1024U, 2U * 1024U * 1024U,
        1024U * 1024U, 512U * 1024U, MEM_BW_SPAN_MIN,
    };
    static const size_t chunk_rungs[] = {
        MEM_BW_CHUNK_MAX, 32U * 1024U, MEM_BW_CHUNK_MIN,
    };

    /* Window (PSRAM) first, then the flash-read staging chunk; the
     * staging-failure path frees the window it already holds. */
    size_t span = 0;
    size_t chunk = 0;
    uint32_t *psram = bw_ladder_alloc(win_rungs,
                                      sizeof(win_rungs) / sizeof(win_rungs[0]),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      &span);
    uint8_t *iram = NULL;
    if (psram != NULL) {
        iram = bw_ladder_alloc(chunk_rungs,
                               sizeof(chunk_rungs) / sizeof(chunk_rungs[0]),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                               &chunk);
    }
    if (psram == NULL) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "window alloc failed largest=%u min=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                            MALLOC_CAP_8BIT),
                 (unsigned)MEM_BW_SPAN_MIN);
        out->st = TEST_ST_FAIL;
        return;
    }
    if (iram == NULL) {
        heap_caps_free(psram);
        snprintf(out->evidence, sizeof(out->evidence),
                 "staging alloc failed internal=%u",
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        out->st = TEST_ST_FAIL;
        return;
    }

    const size_t words = span / sizeof(*psram);

    ctx->progress(ctx, 5, "psram write");
    int64_t t0 = esp_timer_get_time();
    for (size_t w = 0; w < words; ++w) {
        psram[w] = (uint32_t)w ^ PSRAM_PATTERN_BASE;
    }
    const int64_t write_us = esp_timer_get_time() - t0;

    ctx->progress(ctx, 35, "psram read");
    volatile uint32_t sink = 0;
    size_t bad_off = 0;
    bool verified = true;
    t0 = esp_timer_get_time();
    for (size_t w = 0; w < words; ++w) {
        const uint32_t v = ((const volatile uint32_t *)psram)[w];
        sink += v;
        if (v != ((uint32_t)w ^ PSRAM_PATTERN_BASE)) {
            bad_off = w * sizeof(*psram);
            verified = false;
            break;
        }
    }
    const int64_t read_us = esp_timer_get_time() - t0;
    if (!verified) {
        heap_caps_free(psram);
        heap_caps_free(iram);
        snprintf(out->evidence, sizeof(out->evidence),
                 "psram verify fail @0x%05zx", bad_off);
        out->st = TEST_ST_FAIL;
        return;
    }

    ctx->progress(ctx, 60, "psram copy");
    const size_t half = span / 2U;
    t0 = esp_timer_get_time();
    memcpy((uint8_t *)psram + half, psram, half);
    const int64_t copy_us = esp_timer_get_time() - t0;

    ctx->progress(ctx, 80, "flash read");
    size_t flash_done = 0;
    uint32_t flash_sink = 0;
    t0 = esp_timer_get_time();
    esp_err_t flash_error = ESP_OK;
    while (flash_done < MEM_BW_FLASH_BYTES &&
           !ctx->cancel_requested(ctx)) {
        flash_error = esp_flash_read(esp_flash_default_chip, iram,
                                     MEM_BW_FLASH_OFFSET + flash_done,
                                     chunk);
        if (flash_error != ESP_OK) {
            break;
        }
        flash_sink += ((const uint32_t *)iram)[0] +
                      ((const uint32_t *)iram)[chunk / 4U - 1U];
        flash_done += chunk;
        /* esp_flash_read() keeps this task runnable for several seconds at
         * the S31 preview target's measured ~1 MB/s. Block one tick per
         * chunk so IDLE0 can service the task watchdog during the benchmark. */
        vTaskDelay(1);
    }
    const int64_t flash_us = esp_timer_get_time() - t0;

    heap_caps_free(psram);
    heap_caps_free(iram);
    (void)sink;
    (void)flash_sink;

    if (ctx->cancel_requested(ctx)) {
        return; /* NOT_RUN -> runner records aborted */
    }
    if (flash_error != ESP_OK) {
        snprintf(out->evidence, sizeof(out->evidence),
                 "flash read failed after %zu bytes", flash_done);
        out->st = TEST_ST_FAIL;
        return;
    }
    snprintf(out->evidence, sizeof(out->evidence),
             "win=%uK W %.0f R %.0f C %.0f F %.0f MB/s",
             (unsigned)(span / 1024U),
             bw_mbps(span, write_us), bw_mbps(span, read_us),
             bw_mbps(half, copy_us), bw_mbps(flash_done, flash_us));
    out->st = TEST_ST_PASS;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void test_memory_register(void)
{
    static const test_case_t s_cases[] = {
        {
            .id = "mem.flash_size", .name = "Flash 16MB check",
            .domain = TEST_DOM_MEMORY, .flags = 0,
            .timeout_ms = 5000, .run = run_flash_size,
        },
        {
            .id = "mem.psram_pattern", .name = "PSRAM 64KiB R/W",
            .domain = TEST_DOM_MEMORY, .flags = 0,
            .timeout_ms = 10000, .run = run_psram_pattern,
        },
        {
            .id = "mem.psram_alias_probe", .name = "PSRAM alias probe",
            .domain = TEST_DOM_MEMORY, .flags = 0,
            .timeout_ms = 30000, .run = run_psram_alias_probe,
        },
        {
            .id = "mem.bandwidth", .name = "Bandwidth bench",
            .domain = TEST_DOM_MEMORY, .flags = TEST_F_LONG,
            .timeout_ms = 60000, .run = run_mem_bandwidth,
        },
    };
    for (size_t i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); ++i) {
        test_register(&s_cases[i]);
    }
}
