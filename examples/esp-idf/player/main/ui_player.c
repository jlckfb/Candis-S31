/*
 * Candis-S31 full-screen player UI.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_player.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "board_init.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "player_utils.h"
#include "player_video_backend.h"
#include "storage_service.h"
#include "video_player.h"

static const char *TAG = "player_ui";

#define MAX_FILES           64
#define FILE_PATH_LEN       320
#define FILE_NAME_LEN       256
#define MAX_SCAN_DEPTH      4
#define UI_QUEUE_DEPTH      16
#define UI_TASK_STACK_BYTES 8192
#define UI_TASK_PRIORITY    3
#define OVERLAY_TIMEOUT_MS  5000

typedef struct {
    char path[FILE_PATH_LEN];
    char name[FILE_NAME_LEN];
} media_file_t;

typedef enum {
    UI_EV_REFRESH = 0,
    UI_EV_SHOW_OVERLAY,
    UI_EV_HIDE_OVERLAY,
    UI_EV_PROGRESS,
    UI_EV_STARTED,
    UI_EV_FINISHED,
    UI_EV_ERROR,
} ui_event_t;

typedef struct {
    ui_event_t event;
    int value;
} ui_msg_t;

static lv_obj_t *s_scr_browse;
static lv_obj_t *s_scr_play;
static lv_obj_t *s_list;
static lv_obj_t *s_status;
static lv_obj_t *s_overlay;
static lv_obj_t *s_title;
static lv_obj_t *s_progress;
static lv_obj_t *s_progress_text;
static lv_obj_t *s_play_label;
static lv_obj_t *s_loop_button;
static lv_obj_t *s_volume;
static lv_obj_t *s_brightness;
static lv_timer_t *s_hide_timer;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static media_file_t s_files[MAX_FILES];
static int s_file_count;
static bool s_loop;
static bool s_seeking;
static bool s_ready;
static char s_current_path[FILE_PATH_LEN];
static lv_font_t *s_media_font;

extern const uint8_t s_noto_font_start[]
    asm("_binary_NotoSansSC_Regular_sub_ttf_start");
extern const uint8_t s_noto_font_end[]
    asm("_binary_NotoSansSC_Regular_sub_ttf_end");

static const lv_font_t *media_font(void)
{
    return s_media_font != NULL ? s_media_font : &lv_font_source_han_sans_sc_16_cjk;
}

static bool ui_lock(void)
{
    return bsp_display_lock(portMAX_DELAY);
}

static void ui_unlock(void)
{
    bsp_display_unlock();
}

static void ui_post(ui_event_t event, int value)
{
    if (s_queue == NULL) {
        return;
    }
    const ui_msg_t message = {.event = event, .value = value};
    if (xQueueSend(s_queue, &message, pdMS_TO_TICKS(20)) != pdPASS) {
        ESP_LOGW(TAG, "UI queue full, dropping event %d", event);
    }
}

static void overlay_hide_locked(void)
{
    if (s_overlay == NULL) {
        return;
    }
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_hide_timer != NULL) {
        lv_timer_pause(s_hide_timer);
    }
    if (lv_screen_active() == s_scr_play && video_player_is_active()) {
        player_video_backend_set_direct(true);
    }
}

static void overlay_show_locked(void)
{
    if (s_overlay == NULL || lv_screen_active() != s_scr_play) {
        return;
    }
    player_video_backend_set_direct(false);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_hide_timer != NULL) {
        lv_timer_reset(s_hide_timer);
        lv_timer_resume(s_hide_timer);
    }
}

static void overlay_timeout(lv_timer_t *timer)
{
    (void)timer;
    overlay_hide_locked();
}

static void on_player_event(player_event_t event, int value, void *user)
{
    (void)user;
    switch (event) {
    case PLAYER_EV_STARTED:
        ui_post(UI_EV_STARTED, 0);
        break;
    case PLAYER_EV_FINISHED:
        ui_post(UI_EV_FINISHED, value);
        break;
    case PLAYER_EV_ERROR:
        ui_post(UI_EV_ERROR, value);
        break;
    case PLAYER_EV_PROGRESS:
        ui_post(UI_EV_PROGRESS, value);
        break;
    }
}

static void start_playback_locked(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    snprintf(s_current_path, sizeof(s_current_path), "%s", path);
    lv_screen_load(s_scr_play);
    lv_label_set_text(s_title, strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path);
    lv_slider_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_label_set_text(s_progress_text, "0.0%");
    lv_label_set_text(s_play_label, LV_SYMBOL_PAUSE);

    video_play_request_t request = {
        .video_path = s_current_path,
        .volume = player_settings()->volume,
        .loop = s_loop,
        .cb = on_player_event,
    };
    const esp_err_t error = video_player_play(&request);
    if (error != ESP_OK) {
        lv_label_set_text_fmt(s_title, "Playback failed: %s", esp_err_to_name(error));
        lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
        ESP_LOGE(TAG, "video_player_play failed: %s", esp_err_to_name(error));
    }
    overlay_hide_locked();
}

static void event_file_clicked(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index >= 0 && index < s_file_count) {
        start_playback_locked(s_files[index].path);
    }
}

static void event_play_clicked(lv_event_t *event)
{
    (void)event;
    if (video_player_is_active()) {
        const bool pause = !video_player_is_paused();
        video_player_pause(pause);
        lv_label_set_text(s_play_label, pause ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    } else {
        start_playback_locked(s_current_path);
    }
    overlay_show_locked();
}

static void event_stop_clicked(lv_event_t *event)
{
    (void)event;
    player_video_backend_set_direct(false);
    video_player_stop();
    lv_screen_load(s_scr_browse);
}

static void event_loop_clicked(lv_event_t *event)
{
    (void)event;
    s_loop = !s_loop;
    video_player_set_loop(s_loop);
    lv_obj_set_style_bg_color(s_loop_button,
                              s_loop ? lv_color_hex(0x238636) : lv_color_hex(0x30363D), 0);
    overlay_show_locked();
}

static void event_volume(lv_event_t *event)
{
    const int value = (int)lv_slider_get_value(lv_event_get_target(event));
    player_settings()->volume = value;
    video_player_set_volume(value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        player_settings_save();
    }
    overlay_show_locked();
}

static void event_brightness(lv_event_t *event)
{
    const int value = (int)lv_slider_get_value(lv_event_get_target(event));
    player_settings()->brightness = value;
    video_player_set_brightness(value);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        player_settings_save();
    }
    overlay_show_locked();
}

static void event_progress(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        s_seeking = true;
    }
    if (s_seeking && (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_PRESSING)) {
        const int value = (int)lv_slider_get_value(s_progress);
        lv_label_set_text_fmt(s_progress_text, "%d.%d%%", value / 10, value % 10);
    }
    if (code == LV_EVENT_RELEASED) {
        const int value = (int)lv_slider_get_value(s_progress);
        video_player_seek((value + 5) / 10);
        s_seeking = false;
    }
    overlay_show_locked();
}

static void event_screen_touch(lv_event_t *event)
{
    (void)event;
    overlay_show_locked();
}

static void event_refresh(lv_event_t *event)
{
    (void)event;
    ui_player_refresh_files();
}

static void style_screen(lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
}

static void build_browse_screen_locked(void)
{
    s_scr_browse = lv_obj_create(NULL);
    style_screen(s_scr_browse);

    lv_obj_t *heading = lv_label_create(s_scr_browse);
    lv_label_set_text(heading, "Media");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(heading, lv_color_white(), 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 12);

    s_list = lv_obj_create(s_scr_browse);
    lv_obj_set_size(s_list, 436, 376);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(s_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    s_status = lv_label_create(s_scr_browse);
    lv_label_set_text(s_status, "Insert TF card");
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    lv_obj_t *refresh = lv_button_create(s_scr_browse);
    lv_obj_set_size(refresh, 46, 34);
    lv_obj_align(refresh, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
    lv_obj_t *label = lv_label_create(refresh);
    lv_label_set_text(label, LV_SYMBOL_REFRESH);
    lv_obj_center(label);
    lv_obj_add_event_cb(refresh, event_refresh, LV_EVENT_CLICKED, NULL);
}

static void build_play_screen_locked(void)
{
    s_scr_play = lv_obj_create(NULL);
    style_screen(s_scr_play);
    lv_obj_add_event_cb(s_scr_play, event_screen_touch, LV_EVENT_PRESSED, NULL);
    lv_screen_load(s_scr_play);
}

static lv_obj_t *make_button(lv_obj_t *parent, int x, int width,
                             const char *text, lv_event_cb_t callback,
                             lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 64);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, 88);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x30363D), 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    if (label_out != NULL) {
        *label_out = label;
    }
    return button;
}

static void build_overlay_locked(void)
{
    s_overlay = lv_obj_create(s_scr_play);
    lv_obj_set_size(s_overlay, 460, 330);
    lv_obj_align(s_overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 8, 0);

    s_title = lv_label_create(s_overlay);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, 380);
    lv_obj_set_style_text_font(s_title, media_font(), 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "Ready");
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 4, 0);

    s_progress_text = lv_label_create(s_overlay);
    lv_label_set_text(s_progress_text, "0.0%");
    lv_obj_set_style_text_color(s_progress_text, lv_color_white(), 0);
    lv_obj_align(s_progress_text, LV_ALIGN_TOP_RIGHT, -4, 0);

    s_progress = lv_slider_create(s_overlay);
    lv_obj_set_size(s_progress, 436, 48);
    lv_obj_align(s_progress, LV_ALIGN_TOP_MID, 0, 30);
    lv_slider_set_range(s_progress, 0, 1000);
    lv_obj_set_style_pad_all(s_progress, 12, LV_PART_KNOB);
    lv_obj_add_event_cb(s_progress, event_progress, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_progress, event_progress, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_progress, event_progress, LV_EVENT_RELEASED, NULL);

    make_button(s_overlay, 4, 100, LV_SYMBOL_PLAY, event_play_clicked, &s_play_label);
    make_button(s_overlay, 112, 100, LV_SYMBOL_STOP, event_stop_clicked, NULL);
    s_loop_button = make_button(s_overlay, 220, 100, LV_SYMBOL_LOOP,
                                event_loop_clicked, NULL);

    lv_obj_t *volume_icon = lv_label_create(s_overlay);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(volume_icon, lv_color_white(), 0);
    lv_obj_align(volume_icon, LV_ALIGN_TOP_LEFT, 8, 188);
    s_volume = lv_slider_create(s_overlay);
    lv_obj_set_size(s_volume, 390, 48);
    lv_obj_align(s_volume, LV_ALIGN_TOP_LEFT, 42, 170);
    lv_obj_set_style_pad_all(s_volume, 12, LV_PART_KNOB);
    lv_slider_set_range(s_volume, 0, 100);
    lv_slider_set_value(s_volume, player_settings()->volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_volume, event_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume, event_volume, LV_EVENT_RELEASED, NULL);

    lv_obj_t *brightness_icon = lv_label_create(s_overlay);
    lv_label_set_text(brightness_icon, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(brightness_icon, lv_color_white(), 0);
    lv_obj_align(brightness_icon, LV_ALIGN_TOP_LEFT, 8, 258);
    s_brightness = lv_slider_create(s_overlay);
    lv_obj_set_size(s_brightness, 390, 48);
    lv_obj_align(s_brightness, LV_ALIGN_TOP_LEFT, 42, 240);
    lv_obj_set_style_pad_all(s_brightness, 12, LV_PART_KNOB);
    lv_slider_set_range(s_brightness, 5, 100);
    lv_slider_set_value(s_brightness, player_settings()->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_brightness, event_brightness, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_brightness, event_brightness, LV_EVENT_RELEASED, NULL);

    s_hide_timer = lv_timer_create(overlay_timeout, OVERLAY_TIMEOUT_MS, NULL);
    overlay_hide_locked();
}

static void add_media_file(const char *path, const char *name)
{
    if (s_file_count >= MAX_FILES) {
        return;
    }
    snprintf(s_files[s_file_count].path, sizeof(s_files[s_file_count].path), "%s", path);
    snprintf(s_files[s_file_count].name, sizeof(s_files[s_file_count].name), "%s", name);
    ++s_file_count;
}

static void scan_directory(const char *directory, int depth)
{
    if (depth > MAX_SCAN_DEPTH || s_file_count >= MAX_FILES) {
        return;
    }
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char path[FILE_PATH_LEN];
        int written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (written <= 0 || written >= (int)sizeof(path)) {
            continue;
        }
        struct stat status;
        if (stat(path, &status) != 0) {
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            scan_directory(path, depth + 1);
        } else if (S_ISREG(status.st_mode) && player_media_file_supported(entry->d_name)) {
            add_media_file(path, entry->d_name);
        }
    }
    closedir(dir);
}

static int compare_media(const void *left, const void *right)
{
    const media_file_t *a = left;
    const media_file_t *b = right;
    return strcasecmp(a->name, b->name);
}

static void refresh_files_locked(void)
{
    lv_obj_clean(s_list);
    s_file_count = 0;
    if (!storage_mounted()) {
        lv_label_set_text(s_status, "Insert TF card");
        return;
    }
    storage_lease_t lease = {0};
    if (storage_lease_acquire(&lease) != ESP_OK) {
        lv_label_set_text(s_status, "TF card is busy/removing");
        return;
    }
    scan_directory(storage_mount_point(), 0);
    storage_lease_release(&lease);
    qsort(s_files, (size_t)s_file_count, sizeof(s_files[0]), compare_media);

    for (int i = 0; i < s_file_count; ++i) {
        lv_obj_t *button = lv_button_create(s_list);
        lv_obj_set_width(button, 416);
        lv_obj_set_height(button, 48);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, s_files[i].name);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(label, 390);
        lv_obj_set_style_text_font(label, media_font(), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, event_file_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }
    lv_label_set_text_fmt(s_status, "%d media file(s)", s_file_count);
    ESP_LOGI(TAG, "scanned %d media file(s)", s_file_count);
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_msg_t message;
    for (;;) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdPASS || !s_ready) {
            continue;
        }
        if (!ui_lock()) {
            continue;
        }
        switch (message.event) {
        case UI_EV_REFRESH:
            refresh_files_locked();
            break;
        case UI_EV_SHOW_OVERLAY:
            overlay_show_locked();
            break;
        case UI_EV_HIDE_OVERLAY:
            overlay_hide_locked();
            break;
        case UI_EV_PROGRESS:
            if (!s_seeking) {
                lv_slider_set_value(s_progress, message.value, LV_ANIM_OFF);
                lv_label_set_text_fmt(s_progress_text, "%d.%d%%",
                                      message.value / 10, message.value % 10);
            }
            break;
        case UI_EV_STARTED:
            lv_label_set_text(s_play_label, LV_SYMBOL_PAUSE);
            break;
        case UI_EV_FINISHED:
            lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
            overlay_show_locked();
            break;
        case UI_EV_ERROR:
            lv_label_set_text_fmt(s_title, "Playback error (%d)", message.value);
            lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
            overlay_show_locked();
            break;
        }
        ui_unlock();
    }
}

esp_err_t ui_player_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_msg_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ui_task, "player_ui", UI_TASK_STACK_BYTES, NULL,
                    UI_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!ui_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    const size_t font_size = (size_t)(s_noto_font_end - s_noto_font_start);
    s_media_font = lv_tiny_ttf_create_data_ex(s_noto_font_start, font_size, 16,
                                               LV_FONT_KERNING_NORMAL, 128);
    if (s_media_font == NULL) {
        ESP_LOGW(TAG, "failed to load embedded Noto Sans SC; using LVGL fallback");
    } else {
        ESP_LOGI(TAG, "embedded Noto Sans SC ready: %u bytes", (unsigned)font_size);
    }
    build_browse_screen_locked();
    build_play_screen_locked();
    ui_unlock();
    return ESP_OK;
}

esp_err_t ui_player_complete_init(void)
{
    if (s_scr_play == NULL || s_ready) {
        return s_ready ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!ui_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    build_overlay_locked();
    lv_screen_load(s_scr_browse);
    s_ready = true;
    ui_unlock();
    ui_player_refresh_files();
    return ESP_OK;
}

void ui_player_refresh_files(void)
{
    ui_post(UI_EV_REFRESH, 0);
}

void ui_player_show_overlay(bool show)
{
    ui_post(show ? UI_EV_SHOW_OVERLAY : UI_EV_HIDE_OVERLAY, 0);
}

