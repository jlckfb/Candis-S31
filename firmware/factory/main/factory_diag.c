/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_cache.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "factory_console.h"
#include "factory_modules.h"
#include "factory_report.h"

static bool test_accepts_manual_result(factory_test_id_t test)
{
    return test == FACTORY_TEST_DISPLAY || test == FACTORY_TEST_RGB_LED ||
           test == FACTORY_TEST_SPEAKER || test == FACTORY_TEST_BUTTONS ||
           test == FACTORY_TEST_DISPLAY_SLEEP;
}

static int command_sys_tasks(int argc, char **argv)
{
    uint32_t period_ms = 1000;
    if (argc >= 2) {
        char *end = NULL;
        const unsigned long value = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value < 100 || value > 10000) {
            printf("usage: sys_tasks [PERIOD_MS 100-10000]\n");
            return ESP_ERR_INVALID_ARG;
        }
        period_ms = (uint32_t)value;
    }

    const UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *before = malloc(capacity * sizeof(TaskStatus_t));
    TaskStatus_t *after = malloc(capacity * sizeof(TaskStatus_t));
    if (before == NULL || after == NULL) {
        free(before);
        free(after);
        return ESP_ERR_NO_MEM;
    }

    uint32_t run_before = 0;
    uint32_t run_after = 0;
    uxTaskGetSystemState(before, capacity, &run_before);
    vTaskDelay(pdMS_TO_TICKS(period_ms));
    const UBaseType_t count = uxTaskGetSystemState(after, capacity, &run_after);
    const uint32_t span = run_after - run_before;
    if (span == 0) {
        printf("run-time stats clock did not advance\n");
        free(before);
        free(after);
        return ESP_FAIL;
    }

    printf("%-16s %8s %10s\n", "task", "cpu", "stack_hwm");
    for (UBaseType_t i = 0; i < count; ++i) {
        uint32_t delta = 0;
        for (UBaseType_t j = 0; j < capacity; ++j) {
            if (before[j].xHandle == after[i].xHandle) {
                delta = after[i].ulRunTimeCounter - before[j].ulRunTimeCounter;
                break;
            }
        }
        const uint32_t pct_x100 = delta * 10000U / span;
        printf("%-16s %3u.%02u%% %10u\n", after[i].pcTaskName,
               (unsigned)(pct_x100 / 100), (unsigned)(pct_x100 % 100),
               (unsigned)after[i].usStackHighWaterMark);
    }
    printf("FACTORY_SYS {\"period_ms\":%u,\"span_us\":%u,\"tasks\":%u}\n",
           (unsigned)period_ms, (unsigned)span, (unsigned)count);
    free(before);
    free(after);
    return ESP_OK;
}

static int command_mark(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mark TEST pass|fail|skip [detail]\n");
        return ESP_ERR_INVALID_ARG;
    }
    const factory_test_id_t test = factory_report_find(argv[1]);
    if (test == FACTORY_TEST_COUNT) {
        printf("unknown test: %s\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }
    const factory_status_t status = strcmp(argv[2], "pass") == 0 ? FACTORY_STATUS_PASS :
                                            strcmp(argv[2], "fail") == 0 ? FACTORY_STATUS_FAIL :
                                            strcmp(argv[2], "skip") == 0 ? FACTORY_STATUS_SKIP :
                                                                            FACTORY_STATUS_NOT_RUN;
    if (status == FACTORY_STATUS_NOT_RUN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (status != FACTORY_STATUS_SKIP && !test_accepts_manual_result(test)) {
        printf("%s is software-scored; run its test command instead\n", argv[1]);
        return ESP_ERR_INVALID_ARG;
    }

    char detail[96] = "operator marked";
    if (argc > 3) {
        detail[0] = '\0';
        for (int index = 3; index < argc; ++index) {
            const size_t used = strlen(detail);
            snprintf(detail + used, sizeof(detail) - used, "%s%s",
                     used > 0 ? " " : "", argv[index]);
            if (strlen(detail) == sizeof(detail) - 1) {
                break;
            }
        }
    }
    factory_report_set(test, status, detail);
    factory_report_print_one(test);
    return ESP_OK;
}
/* ---- mem_psram_verify --------------------------------------------------
 * Adjudicate the capacity disagreement: boot reports 32 MB of PSRAM while
 * the product data and the NRV16 part naming point at 16 MB. An octal PSRAM
 * die ignores the address bits above its physical depth, so on a 16 MB part
 * the 32 MB map wraps: virtual address X and X+16MB hit the same physical
 * cell. Both modes hunt that wrap; every read below is forced past the
 * cache with esp_cache_msync so a stale line can never masquerade as the
 * physical cell. ESP32-S31 supports cache writeback
 * (SOC_CACHE_WRITEBACK_SUPPORTED), and esp_cache_msync rejects unaligned
 * ranges, so every sync window is a whole cache line. */

#define PSRAM_VERIFY_CHUNK_BYTES  (1024U * 1024U)
#define PSRAM_VERIFY_FULL_MIN     (17U * 1024U * 1024U)
#define PSRAM_VERIFY_PATTERN      UINT32_C(0xa5a50000) /* pattern base of command_psram_test */
#define PSRAM_VERIFY_MAGIC_LOW    UINT32_C(0xc0ffee11)
#define PSRAM_VERIFY_MAGIC_HIGH   UINT32_C(0x5afe2002)
#define PSRAM_VERIFY_MAX_LINE     128U

static size_t psram_verify_line_size(const void *addr)
{
    const size_t line = esp_cache_get_line_size_by_addr(addr);
    return (line >= 16U && line <= PSRAM_VERIFY_MAX_LINE) ? line : 64U;
}

/* Write back then invalidate the line holding addr: the write reaches the
 * physical cell (wrapping with it, if the chip wraps) and the next read
 * misses the cache. */
static void psram_verify_publish(const void *addr, size_t line)
{
    const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1U);
    esp_cache_msync((void *)start, line,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static void psram_verify_reload(const void *addr, size_t line)
{
    const uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(line - 1U);
    esp_cache_msync((void *)start, line, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

/* One save -> write -> observe -> restore round across a (low, high)
 * virtual pair. Both addresses must sit inside the caller's own
 * allocation: on a wrapping chip the high write then lands on a cell the
 * allocation already owns, keeping the probe non-destructive for the rest
 * of the system. Returns true when the two addresses alias one physical
 * cell. */
static bool psram_verify_pair_aliases(uint32_t *low, uint32_t *high,
                                      size_t line)
{
    volatile uint32_t *const vlow = low;
    volatile uint32_t *const vhigh = high;
    const uintptr_t low_line = (uintptr_t)low & ~(uintptr_t)(line - 1U);
    const uintptr_t high_line = (uintptr_t)high & ~(uintptr_t)(line - 1U);
    uint8_t low_save[PSRAM_VERIFY_MAX_LINE];
    uint8_t high_save[PSRAM_VERIFY_MAX_LINE];

    /* Start from clean lines so both saves read the physical cells. */
    psram_verify_publish(low, line);
    psram_verify_publish(high, line);
    memcpy(low_save, (const void *)low_line, line);
    memcpy(high_save, (const void *)high_line, line);

    /* Direction 1: write through low, then look through high. */
    *vlow = PSRAM_VERIFY_MAGIC_LOW;
    psram_verify_publish(low, line);
    psram_verify_reload(high, line);
    const bool low_to_high = (*vhigh == PSRAM_VERIFY_MAGIC_LOW);
    memcpy((void *)low_line, low_save, line);
    psram_verify_publish(low, line);

    /* Direction 2: write through high, then look through low. A high write
     * that changes the low read is the wrap signature. */
    *vhigh = PSRAM_VERIFY_MAGIC_HIGH;
    psram_verify_publish(high, line);
    psram_verify_reload(low, line);
    const bool high_to_low = (*vlow == PSRAM_VERIFY_MAGIC_HIGH);

    /* Restore both lines. On a wrapping chip they are the same physical
     * cell, and the two saves hold identical content (equal line offset,
     * read before any magic write), so the order is irrelevant. */
    memcpy((void *)low_line, low_save, line);
    psram_verify_publish(low, line);
    memcpy((void *)high_line, high_save, line);
    psram_verify_publish(high, line);

    return low_to_high || high_to_low;
}

static void psram_verify_print_state(void)
{
    printf("psram_initialized=%s\n",
           esp_psram_is_initialized() ? "yes" : "no");
    printf("idf_detected_psram_bytes=%zu\n", esp_psram_get_size());
    printf("spiram_free_bytes=%zu\n",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    printf("spiram_largest_free_block=%zu\n",
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT));
}

static void psram_verify_print_verdict(const char *mode, const char *token,
                                       const char *inconclusive_reason)
{
    printf("PSRAM capacity verdict: %s", token);
    if (inconclusive_reason != NULL) {
        printf("(%s)", inconclusive_reason);
    }
    putchar('\n');
    printf("cross-check the verdict against the BOM and the chip top mark "
           "before filing it\n");
    fputs("FACTORY_PSRAM_VERIFY {\"mode\":", stdout);
    factory_report_print_json_string(mode);
    fputs(",\"verdict\":", stdout);
    factory_report_print_json_string(token);
    printf(",\"detected_bytes\":%zu}\n", esp_psram_get_size());
}

static int psram_verify_probe(void)
{
    static const size_t anchors[] = {1U << 20, 4U << 20, 12U << 20};
    static const size_t deltas[] = {8U << 20, 16U << 20};
    const size_t min_window = anchors[0] + deltas[1] + PSRAM_VERIFY_MAX_LINE;

    size_t size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (size < min_window) {
        printf("probe: largest free block %zu bytes < %zu needed for a "
               "1 MB-anchor +16 MB pair\n", size, min_window);
        psram_verify_print_verdict("probe", "INCONCLUSIVE",
                                   "largest free block too small for a +16 MB pair");
        return ESP_OK;
    }
    /* The window must be one owned allocation: every probed address then
     * belongs to this command, even after a wrap. */
    uint32_t *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
                                       MALLOC_CAP_8BIT);
    if (block == NULL) {
        size = min_window;
        block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (block == NULL) {
        psram_verify_print_verdict("probe", "INCONCLUSIVE",
                                   "probe window allocation failed");
        return ESP_ERR_NO_MEM;
    }
    const size_t line = psram_verify_line_size(block);
    printf("probe window: base=%p size=%zu cache_line=%zu\n",
           (void *)block, size, line);

    unsigned tested8 = 0;
    unsigned aliased8 = 0;
    unsigned tested16 = 0;
    unsigned aliased16 = 0;
    for (size_t d = 0; d < sizeof(deltas) / sizeof(deltas[0]); ++d) {
        for (size_t a = 0; a < sizeof(anchors) / sizeof(anchors[0]); ++a) {
            if (anchors[a] + deltas[d] + line > size) {
                continue;
            }
            uint32_t *low = (uint32_t *)((uint8_t *)block + anchors[a]);
            uint32_t *high = (uint32_t *)((uint8_t *)block + anchors[a] +
                                          deltas[d]);
            const bool aliased = psram_verify_pair_aliases(low, high, line);
            printf("probe pair +0x%08zx vs +0x%08zx (delta=%u MB): %s\n",
                   anchors[a], anchors[a] + deltas[d],
                   (unsigned)(deltas[d] >> 20),
                   aliased ? "ALIASED" : "distinct");
            if (deltas[d] == (16U << 20)) {
                ++tested16;
                aliased16 += aliased ? 1U : 0U;
            } else {
                ++tested8;
                aliased8 += aliased ? 1U : 0U;
            }
        }
    }
    heap_caps_free(block);

    printf("probe summary: 16MB pairs %u tested/%u aliased, 8MB pairs %u "
           "tested/%u aliased\n", tested16, aliased16, tested8, aliased8);
    if (aliased16 > 0 || aliased8 > 0) {
        /* A wrap at +8 MB means <= 8 MB physical, which would also explain a
         * 16 MB misreport; either way the reported 32 MB is aliased. */
        psram_verify_print_verdict("probe", "ALIASED_16MB", NULL);
    } else if (tested16 > 0) {
        printf("probe sampled %u +16 MB pair(s) only; run "
               "'mem_psram_verify full' for whole-range coverage\n", tested16);
        psram_verify_print_verdict("probe", "TRUE_32MB", NULL);
    } else {
        psram_verify_print_verdict("probe", "INCONCLUSIVE",
                                   "no +16 MB pair fit the probe window");
    }
    return ESP_OK;
}

static int psram_verify_full(void)
{
    size_t size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (size < PSRAM_VERIFY_FULL_MIN) {
        printf("full: largest free block %zu bytes < %u needed to cross a "
               "16 MB wrap boundary\n", size, (unsigned)PSRAM_VERIFY_FULL_MIN);
        psram_verify_print_verdict("full", "INCONCLUSIVE",
                                   "largest free block below 17 MB");
        return ESP_OK;
    }
    uint32_t *block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
                                       MALLOC_CAP_8BIT);
    if (block == NULL) {
        size = PSRAM_VERIFY_FULL_MIN;
        block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (block == NULL) {
        psram_verify_print_verdict("full", "INCONCLUSIVE",
                                   "full-range allocation failed");
        return ESP_ERR_NO_MEM;
    }
    const size_t words = size / sizeof(*block);
    printf("mem_psram_verify full: DESTRUCTIVE run over %zu bytes at %p\n",
           size, (void *)block);
    printf("on a wrapping 16 MB chip the write pass overwrites every physical "
           "PSRAM cell,\nheap metadata included; each progress line lands on "
           "the UART before the chunk\nit names, so if the console dies the "
           "last line is the evidence\n");
    fflush(stdout);

    for (size_t base = 0; base < words;
            base += PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block)) {
        const size_t end = base + PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block) <=
                           words ?
                           base + PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block) :
                           words;
        printf("full write  +0x%08zx..+0x%08zx / %zu\n",
               base * sizeof(*block), end * sizeof(*block), size);
        fflush(stdout);
        for (size_t w = base; w < end; ++w) {
            block[w] = (uint32_t)(uintptr_t)&block[w] ^ PSRAM_VERIFY_PATTERN;
        }
    }
    /* Commit every dirty line to the physical cells so the wrap (if any) is
     * in memory before the verify pass, then drop all cached copies. */
    esp_cache_msync(block, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(block, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    size_t mismatches = 0;
    size_t first = words;
    uint32_t first_expected = 0;
    uint32_t first_actual = 0;
    for (size_t base = 0; base < words;
            base += PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block)) {
        const size_t end = base + PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block) <=
                           words ?
                           base + PSRAM_VERIFY_CHUNK_BYTES / sizeof(*block) :
                           words;
        printf("full verify +0x%08zx..+0x%08zx / %zu\n",
               base * sizeof(*block), end * sizeof(*block), size);
        fflush(stdout);
        for (size_t w = base; w < end; ++w) {
            const uint32_t expected =
                (uint32_t)(uintptr_t)&block[w] ^ PSRAM_VERIFY_PATTERN;
            const uint32_t actual = ((const volatile uint32_t *)block)[w];
            if (actual != expected) {
                if (first == words) {
                    first = w;
                    first_expected = expected;
                    first_actual = actual;
                }
                ++mismatches;
            }
        }
    }
    printf("full verify: %zu/%zu words mismatched\n", mismatches, words);
    if (mismatches == 0) {
        heap_caps_free(block);
        printf("buffer released cleanly; heap intact\n");
        psram_verify_print_verdict("full", "TRUE_32MB", NULL);
    } else {
        printf("first mismatch at +0x%08zx (addr %p): wrote 0x%08" PRIx32
               " read 0x%08" PRIx32 "\n",
               first * sizeof(*block), (void *)&block[first],
               first_expected, first_actual);
        psram_verify_print_verdict("full", "ALIASED_16MB", NULL);
        /* On a wrapping chip the SPIRAM heap metadata is clobbered; keep the
         * block (freeing it could crash) and have the operator power-cycle. */
        printf("block left allocated on purpose; restart the board before "
               "any further test\n");
    }
    return ESP_OK;
}

static int command_mem_psram_verify(int argc, char **argv)
{
    bool full = false;
    if (argc >= 2) {
        if (strcmp(argv[1], "full") == 0) {
            full = true;
        } else if (strcmp(argv[1], "probe") != 0) {
            printf("usage: mem_psram_verify [probe|full]\n"
                   "  probe (default): non-destructive +8/+16 MB alias probe; "
                   "every touched cache line is saved and restored\n"
                   "  full: DESTRUCTIVE; patterns the largest free block "
                   "(>= 17 MB) to expose a 16 MB wrap; may crash; run last\n");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (argc > 2) {
        printf("usage: mem_psram_verify [probe|full]\n");
        return ESP_ERR_INVALID_ARG;
    }

    psram_verify_print_state();
    if (!esp_psram_is_initialized()) {
        psram_verify_print_verdict(full ? "full" : "probe", "INCONCLUSIVE",
                                   "PSRAM not initialized this boot");
        return ESP_FAIL;
    }
    return full ? psram_verify_full() : psram_verify_probe();
}
/* ---- mem_bandwidth -----------------------------------------------------
 * Application-visible throughput at the currently configured clocks
 * (QIO 80 MHz flash, octal PSRAM 250 MHz). These numbers measure the
 * cache/heap/driver path, not controller peak ratings, and exist to catch
 * signal-integrity regressions when the speed grade changes. */

#define MEM_BW_CHUNK_BYTES  (64U * 1024U)
#define MEM_BW_SPAN_MAX     (8U * 1024U * 1024U)
#define MEM_BW_FLASH_BYTES  (4U * 1024U * 1024U)
#define MEM_BW_FLASH_OFFSET (1024U * 1024U)

static double mem_bw_mbps(size_t bytes, int64_t elapsed_us)
{
    return elapsed_us > 0 ? (double)bytes / (double)elapsed_us : 0.0;
}

static int command_mem_bandwidth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!esp_psram_is_initialized()) {
        printf("mem_bandwidth: PSRAM not initialized this boot\n");
        return ESP_FAIL;
    }

    size_t span = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
    if (span > MEM_BW_SPAN_MAX) {
        span = MEM_BW_SPAN_MAX;
    }
    span &= ~(size_t)4095U;
    uint32_t *psram = heap_caps_malloc(span, MALLOC_CAP_SPIRAM |
                                       MALLOC_CAP_8BIT);
    uint8_t *iram = heap_caps_malloc(MEM_BW_CHUNK_BYTES, MALLOC_CAP_INTERNAL |
                                     MALLOC_CAP_8BIT);
    if (psram == NULL || iram == NULL || span < 2U * MEM_BW_CHUNK_BYTES) {
        printf("mem_bandwidth: allocation failed (psram=%p span=%zu iram=%p)\n",
               (void *)psram, span, (void *)iram);
        heap_caps_free(psram);
        heap_caps_free(iram);
        return ESP_ERR_NO_MEM;
    }
    const size_t words = span / sizeof(*psram);
    printf("mem_bandwidth: psram window %zu bytes at %p\n", span,
           (void *)psram);

    int64_t t0 = esp_timer_get_time();
    for (size_t w = 0; w < words; ++w) {
        psram[w] = (uint32_t)w ^ PSRAM_VERIFY_PATTERN;
    }
    const int64_t write_us = esp_timer_get_time() - t0;

    volatile uint32_t sink = 0;
    t0 = esp_timer_get_time();
    for (size_t w = 0; w < words; ++w) {
        sink += ((const volatile uint32_t *)psram)[w];
    }
    const int64_t read_us = esp_timer_get_time() - t0;

    const size_t half = span / 2U;
    t0 = esp_timer_get_time();
    memcpy((uint8_t *)psram + half, psram, half);
    const int64_t copy_us = esp_timer_get_time() - t0;

    size_t flash_done = 0;
    uint32_t flash_sink = 0;
    t0 = esp_timer_get_time();
    esp_err_t flash_error = ESP_OK;
    while (flash_done < MEM_BW_FLASH_BYTES) {
        flash_error = esp_flash_read(esp_flash_default_chip, iram,
                                     MEM_BW_FLASH_OFFSET + flash_done,
                                     MEM_BW_CHUNK_BYTES);
        if (flash_error != ESP_OK) {
            break;
        }
        flash_sink += ((const uint32_t *)iram)[0] +
                      ((const uint32_t *)iram)[MEM_BW_CHUNK_BYTES / 4U - 1U];
        flash_done += MEM_BW_CHUNK_BYTES;
    }
    const int64_t flash_us = esp_timer_get_time() - t0;

    printf("psram_write  %8.1f MB/s (%zu bytes in %" PRId64 " us)\n",
           mem_bw_mbps(span, write_us), span, write_us);
    printf("psram_read   %8.1f MB/s (%zu bytes in %" PRId64 " us)\n",
           mem_bw_mbps(span, read_us), span, read_us);
    printf("psram_memcpy %8.1f MB/s (%zu bytes in %" PRId64 " us)\n",
           mem_bw_mbps(half, copy_us), half, copy_us);
    if (flash_error == ESP_OK) {
        printf("flash_read   %8.1f MB/s (%zu bytes in %" PRId64 " us)\n",
               mem_bw_mbps(flash_done, flash_us), flash_done, flash_us);
    } else {
        printf("flash_read   FAILED %s after %zu bytes\n",
               esp_err_to_name(flash_error), flash_done);
    }
    printf("FACTORY_BANDWIDTH {\"span\":%zu,\"psram_write_mbps\":%.1f,"
           "\"psram_read_mbps\":%.1f,\"psram_memcpy_mbps\":%.1f,"
           "\"flash_read_mbps\":%.1f,\"sink\":%u}\n", span,
           mem_bw_mbps(span, write_us), mem_bw_mbps(span, read_us),
           mem_bw_mbps(half, copy_us),
           flash_error == ESP_OK ? mem_bw_mbps(flash_done, flash_us) : 0.0,
           (unsigned)(sink + flash_sink));

    heap_caps_free(psram);
    heap_caps_free(iram);
    return flash_error;
}



esp_err_t factory_diag_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "sys_tasks", .help = "Print per-task CPU usage and stack high-water marks: sys_tasks [PERIOD_MS 100-10000].", .func = command_sys_tasks},
        {.command = "mark", .help = "Record a manual result: mark TEST pass|fail|skip [detail].", .func = command_mark},
        {.command = "mem_psram_verify", .help = "Adjudicate 16 vs 32 MB physical PSRAM by address-alias evidence: mem_psram_verify [probe|full]; probe (default) saves/restores, full is DESTRUCTIVE and may crash after progress lands on the UART.", .func = command_mem_psram_verify},
        {.command = "mem_bandwidth", .help = "Measure PSRAM write/read/copy and flash read throughput at the current clocks.", .func = command_mem_bandwidth},
    };
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const esp_err_t error = esp_console_cmd_register(&commands[index]);
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}
