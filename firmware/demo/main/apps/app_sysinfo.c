/*
 * Candis-S31 watch demo - system information app.
 *
 * Static facts (chip, IDF/BSP revision, flash, PSRAM total) are rendered
 * once; free memory, uptime, battery voltage and the FreeRTOS task table
 * refresh every second from an lv_timer (LVGL thread only, no extra task).
 * Task rows use the bundled 16 px text face for readable diagnostics.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demo_apps.h"
#include "esp_flash.h"
#include "services/svc_power.h"
#include "ui/ui_manager.h"

#define SYSINFO_ROW_WIDTH   428
#define SYSINFO_MAX_ROWS    48
#define SYSINFO_REFRESH_MS  1000

typedef struct {
    bool active;
    lv_timer_t *timer;
    lv_obj_t *lbl_ram_free;
    lv_obj_t *lbl_psram_free;
    lv_obj_t *lbl_uptime;
    lv_obj_t *lbl_battery;
    lv_obj_t *task_row[SYSINFO_MAX_ROWS];
    int task_row_count;
    TaskStatus_t *task_buf;
    UBaseType_t task_cap;
} sysinfo_state_t;

static sysinfo_state_t s;

/* ------------------------------------------------------------------ */
/* Layout helpers                                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *info_row(lv_obj_t *parent, const char *name,
                          lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, SYSINFO_ROW_WIDTH);
    lv_obj_set_height(row, 26);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *value_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(value_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
    if (value_out) {
        *value_out = value_lbl;
    }
    return row;
}

static void info_row_fixed(lv_obj_t *parent, const char *name,
                           const char *fmt, ...)
{
    lv_obj_t *value_lbl = NULL;
    info_row(parent, name, &value_lbl);
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(value_lbl, buf);
}

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32S31:
        return "ESP32-S31";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    default:
        return CONFIG_IDF_TARGET;
    }
}

/* ------------------------------------------------------------------ */
/* Periodic refresh                                                    */
/* ------------------------------------------------------------------ */

static void sysinfo_update_tasks(void)
{
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count > s.task_cap) {
        free(s.task_buf);
        s.task_buf = heap_caps_malloc(count * sizeof(TaskStatus_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s.task_buf) {
            s.task_cap = 0;
            return;
        }
        s.task_cap = count;
    }
    if (s.task_buf == NULL || s.task_cap == 0) {
        return; /* allocation failed this cycle: skip, never query NULL */
    }
    const UBaseType_t got = uxTaskGetSystemState(s.task_buf, s.task_cap, NULL);
    /* Clamp to the rows actually built at page entry. */
    int rows = (int)got;
    if (rows > s.task_row_count) {
        rows = s.task_row_count;
    }
    for (int i = 0; i < rows; ++i) {
        const TaskStatus_t *t = &s.task_buf[i];
        lv_label_set_text_fmt(s.task_row[i], "%-16.16s %6u B",
                              t->pcTaskName,
                              (unsigned)(t->usStackHighWaterMark *
                                         sizeof(StackType_t)));
        lv_obj_remove_flag(s.task_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = rows; i < s.task_row_count; ++i) {
        lv_obj_add_flag(s.task_row[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void sysinfo_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active) {
        return;
    }

    lv_label_set_text_fmt(s.lbl_ram_free, "%u KB",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text_fmt(s.lbl_psram_free, "%u KB",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    const int64_t up_s = esp_timer_get_time() / 1000000;
    lv_label_set_text_fmt(s.lbl_uptime, "%02d:%02d:%02d",
                          (int)(up_s / 3600), (int)((up_s % 3600) / 60),
                          (int)(up_s % 60));

    svc_power_status_t power;
    svc_power_get_status(&power);
    if (power.present && power.battery_mv > 0) {
        lv_label_set_text_fmt(s.lbl_battery, "%d mV", power.battery_mv);
    } else {
        lv_label_set_text(s.lbl_battery, "--");
    }

    sysinfo_update_tasks();
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void sysinfo_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.timer) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
    free(s.task_buf);
    s.task_buf = NULL;
    s.task_cap = 0;
    s.task_row_count = 0;
}

lv_obj_t *app_sysinfo_create(void)
{
    s = (sysinfo_state_t){ .active = true };

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("System info", &content);
    lv_obj_add_event_cb(root, sysinfo_root_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    info_row_fixed(content, "Chip", "%s - %d cores",
                   chip_model_name(chip.model), chip.cores);
    info_row_fixed(content, "IDF version", "%s", esp_get_idf_version());
    info_row_fixed(content, "BSP version", "%s", CANDIS_S31_BSP_GIT_REV);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        info_row_fixed(content, "Flash", "%u MB",
                       (unsigned)(flash_bytes / (1024 * 1024)));
    } else {
        info_row_fixed(content, "Flash", "--");
    }
    info_row_fixed(content, "PSRAM total", "%u KB",
                   (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));

    info_row(content, "Internal RAM free", &s.lbl_ram_free);
    info_row(content, "PSRAM free", &s.lbl_psram_free);
    info_row(content, "Uptime", &s.lbl_uptime);
    info_row(content, "Battery voltage", &s.lbl_battery);

    lv_obj_t *head = lv_label_create(content);
    lv_label_set_text(head, "Task stack high-water (bytes free)");
    lv_obj_set_style_text_color(head, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_pad_left(head, 8, 0);
    lv_obj_set_style_pad_top(head, 8, 0);

    /* Create exactly as many rows as there are tasks right now (capped),
     * instead of a fixed 48 that mostly sit hidden: fewer widgets to lay
     * out and no hidden-row churn on every refresh. A task created after
     * entry simply has no row - the page is a point-in-time snapshot. */
    const UBaseType_t live_tasks = uxTaskGetNumberOfTasks();
    const int rows = live_tasks < SYSINFO_MAX_ROWS ?
                     (int)live_tasks : SYSINFO_MAX_ROWS;
    for (int i = 0; i < rows; ++i) {
        lv_obj_t *row = lv_label_create(content);
        lv_obj_set_width(row, SYSINFO_ROW_WIDTH);
        lv_label_set_text(row, "--");
        lv_obj_set_style_text_font(row, ui_font_text(), 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        s.task_row[i] = row;
    }
    s.task_row_count = rows;

    s.timer = lv_timer_create(sysinfo_timer_cb, SYSINFO_REFRESH_MS, NULL);
    sysinfo_timer_cb(s.timer); /* first paint without waiting a second */

    return root;
}
