/*
 * Candis-S31 watch demo - test registry: storage, domain sorting and the
 * thread-safe RAM result library (spec C.2/C.6).
 *
 * Registration happens once on the svc_test_start path (single task);
 * readers are the LVGL task (test center) and the svc_test runner. A
 * mutex guards the result library and the lazy sort; case pointers are
 * stable after registration, so lookup helpers only lock around the sort.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_registry.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TEST_MAX_CASES 64

static const test_case_t *s_cases[TEST_MAX_CASES];
static test_result_t      s_results[TEST_MAX_CASES];
static int                s_order[TEST_MAX_CASES]; /* domain-sorted view */
static int                s_count;
static bool               s_sorted;
static uint32_t           s_run_seq;
static SemaphoreHandle_t  s_lock;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void test_registry_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_count = 0;
    s_sorted = true;
    memset(s_results, 0, sizeof(s_results));
}

/* Stable insertion sort of the order view by domain (ascending enum);
 * registration order is preserved inside each domain. */
static void ensure_sorted(void)
{
    if (s_sorted) {
        return;
    }
    for (int i = 1; i < s_count; ++i) {
        const int key = s_order[i];
        const int key_dom = (int)s_cases[key]->domain;
        int j = i - 1;
        while (j >= 0 && (int)s_cases[s_order[j]]->domain > key_dom) {
            s_order[j + 1] = s_order[j];
            --j;
        }
        s_order[j + 1] = key;
    }
    s_sorted = true;
}

esp_err_t test_register(const test_case_t *tc)
{
    if (!tc || !tc->id || !tc->run) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_count >= TEST_MAX_CASES) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    if (test_find(tc->id) != NULL) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_cases[s_count] = tc;
    s_order[s_count] = s_count;
    s_results[s_count].st = TEST_ST_NOT_RUN;
    ++s_count;
    s_sorted = false;
    unlock();
    return ESP_OK;
}

int test_count(void)
{
    lock();
    ensure_sorted();
    const int n = s_count;
    unlock();
    return n;
}

const test_case_t *test_at(int index)
{
    lock();
    ensure_sorted();
    const test_case_t *tc =
        (index >= 0 && index < s_count) ? s_cases[s_order[index]] : NULL;
    unlock();
    return tc;
}

int test_domain_range(test_domain_t d, int *first, int *cnt)
{
    lock();
    ensure_sorted();
    int found_first = -1;
    int found_cnt = 0;
    for (int i = 0; i < s_count; ++i) {
        if (s_cases[s_order[i]]->domain == d) {
            if (found_first < 0) {
                found_first = i;
            }
            ++found_cnt;
        }
    }
    if (first) {
        *first = found_first;
    }
    if (cnt) {
        *cnt = found_cnt;
    }
    unlock();
    return found_cnt;
}

const test_case_t *test_find(const char *id)
{
    if (!id) {
        return NULL;
    }
    /* Case pointers are stable after registration; no lock needed for the
     * linear scan once svc_test_start has run (and registration itself is
     * single-task). */
    for (int i = 0; i < s_count; ++i) {
        if (s_cases[i] && strcmp(s_cases[i]->id, id) == 0) {
            return s_cases[i];
        }
    }
    return NULL;
}

/* Registry index of a case pointer, -1 when unknown. */
static int case_index(const test_case_t *tc)
{
    for (int i = 0; i < s_count; ++i) {
        if (s_cases[i] == tc) {
            return i;
        }
    }
    return -1;
}

void test_result_get(const char *id, test_result_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const int idx = case_index(test_find(id));
    if (idx < 0) {
        return;
    }
    lock();
    *out = s_results[idx];
    unlock();
}

void test_result_set(const char *id, const test_result_t *res)
{
    if (!res) {
        return;
    }
    const int idx = case_index(test_find(id));
    if (idx < 0) {
        return;
    }
    lock();
    s_results[idx] = *res;
    s_results[idx].run_seq = ++s_run_seq;
    unlock();
}

void test_results_reset(void)
{
    lock();
    for (int i = 0; i < s_count; ++i) {
        s_results[i].st = TEST_ST_NOT_RUN;
        s_results[i].evidence[0] = '\0';
        s_results[i].duration_ms = 0;
        /* Bump the sequence so sequence-deduped UI repaints every row. */
        s_results[i].run_seq = ++s_run_seq;
    }
    unlock();
}

const char *test_domain_name(test_domain_t d)
{
    static const char *const names[TEST_DOM_COUNT] = {
        [TEST_DOM_DISPLAY_TOUCH] = "Display",
        [TEST_DOM_CAMERA]        = "Camera",
        [TEST_DOM_AUDIO]         = "Audio",
        [TEST_DOM_STORAGE]       = "Storage",
        [TEST_DOM_NETWORK]       = "Network",
        [TEST_DOM_SYSTEM]        = "System",
        [TEST_DOM_MEMORY]        = "Memory",
        [TEST_DOM_ACCEL]         = "Accel",
        [TEST_DOM_POWER]         = "Power",
    };
    if (d < 0 || d >= TEST_DOM_COUNT) {
        return "?";
    }
    return names[d];
}

test_status_t test_domain_aggregate(test_domain_t d)
{
    /* C.1 priority: FAIL > WARN > NOT_RUN > SKIP > PASS. */
    int best = 0;
    bool any = false;
    lock();
    for (int i = 0; i < s_count; ++i) {
        if (s_cases[i]->domain != d) {
            continue;
        }
        any = true;
        int rank;
        switch (s_results[i].st) {
        case TEST_ST_FAIL:    rank = 5; break;
        case TEST_ST_WARN:    rank = 4; break;
        case TEST_ST_NOT_RUN: rank = 3; break;
        case TEST_ST_SKIP:    rank = 2; break;
        case TEST_ST_PASS:
        default:              rank = 1; break;
        }
        if (rank > best) {
            best = rank;
        }
    }
    unlock();
    if (!any) {
        return TEST_ST_NOT_RUN;
    }
    switch (best) {
    case 5:  return TEST_ST_FAIL;
    case 4:  return TEST_ST_WARN;
    case 3:  return TEST_ST_NOT_RUN;
    case 2:  return TEST_ST_SKIP;
    default: return TEST_ST_PASS;
    }
}

int test_auto_count(test_domain_t d)
{
    int n = 0;
    for (int i = 0; i < s_count; ++i) {
        if (d >= 0 && s_cases[i]->domain != d) {
            continue;
        }
        if ((s_cases[i]->flags &
             (TEST_F_INTERACTIVE | TEST_F_NO_RUNALL)) == 0) {
            ++n;
        }
    }
    return n;
}
