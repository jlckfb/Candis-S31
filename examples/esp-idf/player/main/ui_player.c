/*
 * Candis-S31 player demo - LVGL UI.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui_player.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_init.h"
#include "storage_service.h"
#include "video_player.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#define MAX_FILES 32
#define FILE_PATH_LEN 96
#define FILE_NAME_LEN 48

static lv_obj_t *s_scr_browse = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_lbl_title = NULL;
static lv_obj_t *s_bar_progress = NULL;
static lv_obj_t *s_lbl_progress = NULL;
static lv_obj_t *s_slider_volume = NULL;
static lv_obj_t *s_slider_brightness = NULL;
static lv_obj_t *s_btn_play = NULL;
static lv_obj_t *s_btn_loop = NULL;
static lv_obj_t *s_btn_speed = NULL;
static float s_speed = 1.0f;

static int s_file_count = 0;
static char s_paths[MAX_FILES][FILE_PATH_LEN];
static char s_names[MAX_FILES][FILE_NAME_LEN];
static bool s_loop = false;
static bool s_overlay_visible = false;
static lv_timer_t *s_hide_timer = NULL;
static char s_current_path[FILE_PATH_LEN];

typedef enum {
    UI_EV_REFRESH,
    UI_EV_SHOW_OVERLAY,
    UI_EV_HIDE_OVERLAY,
    UI_EV_SET_PROGRESS,
} ui_event_t;

typedef struct {
    ui_event_t ev;
    int value;
} ui_msg_t;

#define UI_QUEUE_DEPTH       8
#define UI_TASK_STACK_BYTES  4096
#define UI_TASK_PRIORITY     3

static QueueHandle_t s_ui_queue = NULL;
static TaskHandle_t s_ui_task = NULL;

static void ui_task(void *arg);
static void start_playback(const char *path);
static void stop_playback(void);
static void show_overlay(void);
static void hide_overlay(void);

static void ui_lock(void)
{
    bsp_display_lock(portMAX_DELAY);
}

static void ui_unlock(void)
{
    bsp_display_unlock();
}

static void ui_post(ui_event_t ev, int value)
{
    if (s_ui_queue == NULL) {
        return;
    }
    ui_msg_t msg = {
        .ev = ev,
        .value = value,
    };
    xQueueSend(s_ui_queue, &msg, pdMS_TO_TICKS(10));
}

static void on_player_event(player_event_t ev, int value, void *user);

static void event_play_clicked(lv_event_t *e)
{
    (void)e;
    if (video_player_is_active()) {
        video_player_pause(!video_player_is_paused());
    } else if (s_current_path[0] != '\0') {
        start_playback(s_current_path);
    }
    show_overlay();
}

static void event_stop_clicked(lv_event_t *e)
{
    (void)e;
    stop_playback();
    ui_lock();
    lv_scr_load(s_scr_browse);
    ui_unlock();
}

static void event_speed_clicked(lv_event_t *e)
{
    (void)e;
    const float speeds[] = {0.5f, 1.0f, 1.5f, 2.0f};
    size_t idx = 0;
    for (size_t i = 0; i < sizeof(speeds)/sizeof(speeds[0]); ++i) {
        if (fabsf(s_speed - speeds[i]) < 0.01f) {
            idx = (i + 1) % (sizeof(speeds)/sizeof(speeds[0]));
            break;
        }
    }
    s_speed = speeds[idx];
    video_player_set_speed(s_speed);
    if (s_btn_speed != NULL) {
        lv_label_set_text_fmt(lv_obj_get_child(s_btn_speed, 0), "%.1fx", s_speed);
    }
    show_overlay();
}

static void event_loop_clicked(lv_event_t *e)
{
    (void)e;
    s_loop = !s_loop;
    if (s_btn_loop != NULL) {
        lv_obj_set_style_bg_color(s_btn_loop,
            s_loop ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_GREY), 0);
    }
    show_overlay();
}

static void event_volume_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    video_player_set_volume(v);
    player_settings()->volume = v;
    player_settings_save();
    show_overlay();
}

static void event_brightness_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    video_player_set_brightness(v);
    player_settings()->brightness = v;
    player_settings_save();
    show_overlay();
}

static void event_progress_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *bar = lv_event_get_target(e);
    int p = (int)lv_bar_get_value(bar);
    video_player_seek(p);
    show_overlay();
}

static void event_file_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_file_count) {
        return;
    }
    snprintf(s_current_path, sizeof(s_current_path), "%s", s_paths[idx]);
    start_playback(s_current_path);
}

static void event_screen_touched(lv_event_t *e)
{
    (void)e;
    show_overlay();
}

static void hide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    hide_overlay();
}

static void build_browse_screen(void)
{
    s_scr_browse = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_browse, lv_color_black(), 0);

    lv_obj_t *lbl = lv_label_create(s_scr_browse);
    lv_label_set_text(lbl, "Video Files");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 10);

    s_list = lv_obj_create(s_scr_browse);
    lv_obj_set_size(s_list, 440, 380);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(s_list, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_lbl_status = lv_label_create(s_scr_browse);
    lv_label_set_text(s_lbl_status, "Insert TF card");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_white(), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static lv_obj_t *build_overlay_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(scr, event_screen_touched, LV_EVENT_PRESSED, NULL);

    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, 460, 120);
    lv_obj_align(s_overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_overlay, 8, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);

    s_lbl_title = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_title, "Playing");
    lv_obj_set_style_text_color(s_lbl_title, lv_color_white(), 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_LEFT, 10, 6);

    s_bar_progress = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar_progress, 300, 12);
    lv_obj_align(s_bar_progress, LV_ALIGN_TOP_MID, 0, 30);
    lv_bar_set_range(s_bar_progress, 0, 100);
    lv_obj_add_event_cb(s_bar_progress, event_progress_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_progress = lv_label_create(s_overlay);
    lv_label_set_text(s_lbl_progress, "0%");
    lv_obj_set_style_text_color(s_lbl_progress, lv_color_white(), 0);
    lv_obj_align(s_lbl_progress, LV_ALIGN_TOP_RIGHT, -10, 26);

    s_btn_play = lv_button_create(s_overlay);
    lv_obj_set_size(s_btn_play, 50, 36);
    lv_obj_align(s_btn_play, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_t *lbl_play = lv_label_create(s_btn_play);
    lv_label_set_text(lbl_play, LV_SYMBOL_PLAY);
    lv_obj_center(lbl_play);
    lv_obj_add_event_cb(s_btn_play, event_play_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_stop = lv_button_create(s_overlay);
    lv_obj_set_size(btn_stop, 50, 36);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_LEFT, 70, -10);
    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, LV_SYMBOL_STOP);
    lv_obj_center(lbl_stop);
    lv_obj_add_event_cb(btn_stop, event_stop_clicked, LV_EVENT_CLICKED, NULL);

    s_btn_loop = lv_button_create(s_overlay);
    lv_obj_set_size(s_btn_loop, 50, 36);
    lv_obj_align(s_btn_loop, LV_ALIGN_BOTTOM_LEFT, 130, -10);
    lv_obj_t *lbl_loop = lv_label_create(s_btn_loop);
    lv_label_set_text(lbl_loop, "Loop");
    lv_obj_center(lbl_loop);
    lv_obj_add_event_cb(s_btn_loop, event_loop_clicked, LV_EVENT_CLICKED, NULL);

    s_btn_speed = lv_button_create(s_overlay);
    lv_obj_set_size(s_btn_speed, 60, 36);
    lv_obj_align(s_btn_speed, LV_ALIGN_BOTTOM_LEFT, 190, -10);
    lv_obj_t *lbl_speed = lv_label_create(s_btn_speed);
    lv_label_set_text(lbl_speed, "1.0x");
    lv_obj_center(lbl_speed);
    lv_obj_add_event_cb(s_btn_speed, event_speed_clicked, LV_EVENT_CLICKED, NULL);

    s_slider_volume = lv_slider_create(s_overlay);
    lv_obj_set_size(s_slider_volume, 120, 10);
    lv_obj_align(s_slider_volume, LV_ALIGN_BOTTOM_RIGHT, -10, -40);
    lv_slider_set_range(s_slider_volume, 0, 100);
    lv_slider_set_value(s_slider_volume, player_settings()->volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_volume, event_volume_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_slider_brightness = lv_slider_create(s_overlay);
    lv_obj_set_size(s_slider_brightness, 120, 10);
    lv_obj_align(s_slider_brightness, LV_ALIGN_BOTTOM_RIGHT, -10, -15);
    lv_slider_set_range(s_slider_brightness, 0, 100);
    lv_slider_set_value(s_slider_brightness, player_settings()->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider_brightness, event_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lbl_vol = lv_label_create(s_overlay);
    lv_label_set_text(lbl_vol, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vol, lv_color_white(), 0);
    lv_obj_align(lbl_vol, LV_ALIGN_BOTTOM_RIGHT, -140, -38);

    lv_obj_t *lbl_bright = lv_label_create(s_overlay);
    lv_label_set_text(lbl_bright, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(lbl_bright, lv_color_white(), 0);
    lv_obj_align(lbl_bright, LV_ALIGN_BOTTOM_RIGHT, -140, -13);

    hide_overlay();
    return scr;
}

static void start_playback(const char *path)
{
    video_play_request_t req = {
        .video_path = path,
        .volume = player_settings()->volume,
        .loop = s_loop,
        .speed = 1.0f,
        .cb = on_player_event,
        .user = NULL,
    };
    video_player_play(&req);
    ui_lock();
    if (s_lbl_title != NULL) {
        lv_label_set_text_fmt(s_lbl_title, "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    }
    ui_unlock();
    show_overlay();
}

static void stop_playback(void)
{
    video_player_stop();
}

static void show_overlay(void)
{
    if (s_overlay == NULL) {
        return;
    }
    ui_lock();
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_overlay_visible = true;
    if (s_hide_timer != NULL) {
        lv_timer_reset(s_hide_timer);
    }
    ui_unlock();
}

static void hide_overlay(void)
{
    if (s_overlay == NULL) {
        return;
    }
    ui_lock();
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_overlay_visible = false;
    ui_unlock();
}

static void scan_files(void)
{
    if (s_list == NULL) {
        return;
    }
    ui_lock();
    /* Remove old children. */
    lv_obj_clean(s_list);
    s_file_count = 0;

    if (!storage_mounted()) {
        lv_label_set_text(s_lbl_status, "Insert TF card");
        return;
    }
    const char *mp = storage_mount_point();
    DIR *dir = opendir(mp);
    if (dir == NULL) {
        lv_label_set_text(s_lbl_status, "Cannot open card");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        if (entry->d_type != DT_REG) {
            continue;
        }
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcasecmp(entry->d_name + len - 4, ".avi") != 0) {
            continue;
        }
        snprintf(s_paths[s_file_count], FILE_PATH_LEN, "%s/%s", mp, entry->d_name);
        snprintf(s_names[s_file_count], FILE_NAME_LEN, "%s", entry->d_name);

        lv_obj_t *btn = lv_button_create(s_list);
        lv_obj_set_size(btn, 420, 48);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_names[s_file_count]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, event_file_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_file_count);
        s_file_count++;
    }
    closedir(dir);
    lv_label_set_text_fmt(s_lbl_status, "%d video(s)", s_file_count);
    ui_unlock();
}

esp_err_t ui_player_init(void)
{
    s_ui_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_msg_t));
    if (s_ui_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ui_task, "player_ui", UI_TASK_STACK_BYTES, NULL,
                    UI_TASK_PRIORITY, &s_ui_task) != pdPASS) {
        vQueueDelete(s_ui_queue);
        s_ui_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ui_lock();
    build_browse_screen();
    lv_obj_t *scr_play = build_overlay_screen();
    (void)scr_play;
    lv_scr_load(s_scr_browse);
    s_hide_timer = lv_timer_create(hide_timer_cb, 5000, NULL);
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
    if (show) {
        ui_post(UI_EV_SHOW_OVERLAY, 0);
    } else {
        ui_post(UI_EV_HIDE_OVERLAY, 0);
    }
}

void ui_player_set_progress(int percent)
{
    ui_post(UI_EV_SET_PROGRESS, percent);
}
static void ui_task(void *arg)
{
    (void)arg;
    ui_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_ui_queue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }
        ui_lock();
        switch (msg.ev) {
        case UI_EV_REFRESH:
            scan_files();
            break;
        case UI_EV_SHOW_OVERLAY:
            show_overlay();
            break;
        case UI_EV_HIDE_OVERLAY:
            hide_overlay();
            break;
        case UI_EV_SET_PROGRESS:
            if (s_bar_progress != NULL) {
                lv_bar_set_value(s_bar_progress, msg.value, LV_ANIM_OFF);
            }
            if (s_lbl_progress != NULL) {
                lv_label_set_text_fmt(s_lbl_progress, "%d%%", msg.value);
            }
            break;
        default:
            break;
        }
        ui_unlock();
    }
}

static void on_player_event(player_event_t ev, int value, void *user)
{
    (void)user;
    if (ev == PLAYER_EV_PROGRESS) {
        ui_player_set_progress(value);
    }
}

