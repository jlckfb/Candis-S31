/*
 * Candis-S31 watch demo - page navigation timing instrumentation.
 *
 * One ESP_LOG line per page navigation (create time + open-to-first-flush
 * interval + flush-gap count since the previous navigation). Single-shot:
 * the work is one timestamp compare inside FLUSH_START and one log line,
 * never a timer or an allocation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_perf.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ui_perf";

#define UI_PERF_GAP_THRESHOLD_MS 50

static struct {
    /* Create-side bookkeeping. */
    const char *create_id;
    int64_t create_start_us;
    int64_t create_done_us;
    bool create_valid;

    /* Load-side bookkeeping. */
    const char *load_id;
    int64_t load_us;
    bool load_pending;

    /* Frame-interval stats since the previous report. */
    uint32_t flush_count;
    int64_t prev_flush_us;
    uint32_t gaps_over_threshold;
} s_perf;

void ui_perf_create_begin(const char *id)
{
    s_perf.create_id = id;
    s_perf.create_valid = true;
    s_perf.create_start_us = esp_timer_get_time();
}

void ui_perf_create_end(void)
{
    s_perf.create_done_us = esp_timer_get_time();
}

void ui_perf_load_begin(const char *id)
{
    s_perf.load_id = id ? id : s_perf.create_id;
    s_perf.load_us = esp_timer_get_time();
    s_perf.load_pending = true;
}

static void perf_flush_start(lv_event_t *event)
{
    (void)event;
    const int64_t now = esp_timer_get_time();

    if (s_perf.prev_flush_us != 0) {
        const int64_t gap_ms = (now - s_perf.prev_flush_us) / 1000;
        if (gap_ms >= UI_PERF_GAP_THRESHOLD_MS) {
            ++s_perf.gaps_over_threshold;
        }
    }
    s_perf.prev_flush_us = now;
    ++s_perf.flush_count;

    /* A navigation is pending: this first flush after scr_load_anim is the
     * moment the new page becomes visible. Emit the one-line report here
     * and restart the window counters for the page's dwell period. */
    if (!s_perf.load_pending) {
        return;
    }
    s_perf.load_pending = false;

    const int create_ms = s_perf.create_valid
        ? (int)((s_perf.create_done_us - s_perf.create_start_us) / 1000)
        : -1;
    const int open_ms = s_perf.create_valid
        ? (int)((now - s_perf.create_start_us) / 1000)
        : (int)((now - s_perf.load_us) / 1000);
    ESP_LOGI(TAG, "page=%s create=%dms open=%dms prev_gaps(>=%dms)=%lu/%lu",
             s_perf.load_id ? s_perf.load_id : "?",
             create_ms, open_ms,
             UI_PERF_GAP_THRESHOLD_MS,
             (unsigned long)s_perf.gaps_over_threshold,
             (unsigned long)s_perf.flush_count);

    s_perf.create_valid = false;
    s_perf.flush_count = 0;
    s_perf.gaps_over_threshold = 0;
}

void ui_perf_attach(lv_display_t *display)
{
    lv_display_add_event_cb(display, perf_flush_start,
                            LV_EVENT_FLUSH_START, NULL);
}
