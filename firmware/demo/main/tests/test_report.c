/*
 * Candis-S31 watch demo - test report export (spec C.6).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_report.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_timer.h"

#include "services/svc_storage.h"
#include "test_registry.h"

static const char *status_name(test_status_t st)
{
    switch (st) {
    case TEST_ST_PASS: return "PASS";
    case TEST_ST_FAIL: return "FAIL";
    case TEST_ST_SKIP: return "SKIP";
    case TEST_ST_WARN: return "WARN";
    case TEST_ST_NOT_RUN:
    default:           return "NOT_RUN";
    }
}

/* Minimal JSON string escaping for evidence text (quotes, backslashes,
 * control characters). Evidence is produced by test code only. */
static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (cap == 0) {
        return;
    }
    for (size_t i = 0; in && in[i] && o + 2 < cap; ++i) {
        const char c = in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            out[o++] = ' ';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

esp_err_t test_report_export_to_sd(char *path_out, size_t path_cap)
{
    if (!svc_storage_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    svc_storage_lease_t lease = {0};
    esp_err_t err = svc_storage_lease_acquire(&lease);
    if (err != ESP_OK) {
        return err;
    }

    /* File name from RTC; invalid RTC degrades to uptime seconds. */
    char path[96];
    bsp_rtc_time_t t;
    bsp_rtc_status_t rst;
    const bool rtc_ok = (bsp_rtc_get_time(&t, &rst) == ESP_OK) &&
                        rst.time_valid;
    if (rtc_ok) {
        snprintf(path, sizeof(path),
                 "%s/demo_report_%04d%02d%02d_%02d%02d%02d.jsonl",
                 svc_storage_mount_point(), (int)t.year, (int)t.month,
                 (int)t.day, (int)t.hour, (int)t.minute, (int)t.second);
    } else {
        snprintf(path, sizeof(path), "%s/demo_report_u%llu.jsonl",
                 svc_storage_mount_point(),
                 (unsigned long long)(esp_timer_get_time() / 1000000));
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        svc_storage_lease_release(&lease);
        return ESP_FAIL;
    }

    int total = 0, pass = 0, fail = 0, warn = 0, skip = 0, not_run = 0;
    const int n = test_count();
    for (int i = 0; i < n; ++i) {
        const test_case_t *tc = test_at(i);
        if (!tc) {
            continue;
        }
        test_result_t res;
        test_result_get(tc->id, &res);

        char ev[192];
        json_escape(res.evidence, ev, sizeof(ev));
        fprintf(f,
                "FACTORY_RESULT {\"id\":\"%s\",\"status\":\"%s\","
                "\"evidence\":\"%s\",\"duration_ms\":%lu}\n",
                tc->id, status_name(res.st), ev,
                (unsigned long)res.duration_ms);

        ++total;
        switch (res.st) {
        case TEST_ST_PASS: ++pass; break;
        case TEST_ST_FAIL: ++fail; break;
        case TEST_ST_WARN: ++warn; break;
        case TEST_ST_SKIP: ++skip; break;
        default:           ++not_run; break;
        }
    }
    fprintf(f,
            "FACTORY_SUMMARY {\"total\":%d,\"pass\":%d,\"fail\":%d,"
            "\"warn\":%d,\"skip\":%d,\"not_run\":%d}\n",
            total, pass, fail, warn, skip, not_run);

    const int io_err = ferror(f);
    fclose(f);
    svc_storage_lease_release(&lease);

    if (io_err) {
        return ESP_FAIL;
    }
    if (path_out && path_cap > 0) {
        strlcpy(path_out, path, path_cap);
    }
    return ESP_OK;
}
