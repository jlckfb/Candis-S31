/*
 * Candis-S31 watch demo - recorder app.
 *
 * Four-way input route (left mic only / right mic only / dual mic / basic
 * noise reduction), 0-36 dB gain slider in 3 dB steps, live level meter,
 * record button with elapsed time, and the saved-recording list (tap =
 * play/stop, long-press = delete with confirm). The heavy lifting lives in
 * svc_audio; this app renders state and forwards user intent.
 *
 * Threading: svc callbacks run on the audio task and are forwarded through
 * ui_async() with a session generation counter, so events from a stale
 * recording/playback can never touch widgets of a new session. The static
 * state plus active flag follow the app teardown discipline: on
 * LV_EVENT_DELETE the app stops every service operation it started.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define MAX_FILES        32
#define ROW_WIDTH        444
#define ROW_HEIGHT       44
#define ROW_PITCH        48
#define RECORD_DIR_NAME  "recordings"
#define RECORD_CAP_S     60

typedef struct {
    bool active;
    uint32_t gen;          /* session generation, filters stale svc events */
    /* widgets */
    lv_obj_t *route_btn[4];
    lv_obj_t *gain_slider;
    lv_obj_t *gain_label;
    lv_obj_t *level_bar;
    lv_obj_t *time_label;
    lv_obj_t *rec_btn;
    lv_obj_t *rec_icon;
    lv_obj_t *list;
    lv_timer_t *timer;
    /* runtime */
    bool sd_mounted;
    svc_audio_route_t route;
    bool recording;
    bool saving;
    int64_t rec_start_us;
    bool list_playing;
    char play_path[96];
    lv_obj_t *play_meta;   /* meta label of the playing row, if visible */
    int file_count;
    char paths[MAX_FILES][96];
} recorder_state_t;

static recorder_state_t s;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char *copy_str(const char *text)
{
    const size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

static void fmt_mmss(char buffer[8], int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    snprintf(buffer, 8, "%02d:%02d", seconds / 60, seconds % 60);
}

/* ------------------------------------------------------------------ */
/* Widget refresh                                                      */
/* ------------------------------------------------------------------ */

static void route_refresh(void)
{
    for (int index = 0; index < 4; ++index) {
        const bool selected = index == (int)s.route;
        lv_obj_set_style_bg_color(s.route_btn[index],
                lv_color_hex(selected ? UI_COLOR_ACCENT : UI_COLOR_SURFACE),
                0);
        lv_obj_set_style_border_width(s.route_btn[index], selected ? 0 : 1, 0);
    }
}

static void rec_btn_refresh(void)
{
    if (s.saving) {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_SAVE);
        lv_obj_set_style_radius(s.rec_btn, 14, 0);
        lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COLOR_WARN), 0);
        lv_obj_add_state(s.rec_btn, LV_STATE_DISABLED);
        lv_label_set_text(s.time_label, "保存中…");
    } else if (s.recording) {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_STOP);
        lv_obj_set_style_radius(s.rec_btn, 14, 0);
        lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COLOR_ERR), 0);
        lv_obj_remove_state(s.rec_btn, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_AUDIO);
        lv_obj_set_style_radius(s.rec_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s.rec_btn,
                lv_color_hex(s.sd_mounted ? UI_COLOR_ERR : UI_COLOR_SURFACE),
                0);
        if (s.sd_mounted) {
            lv_obj_remove_state(s.rec_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s.rec_btn, LV_STATE_DISABLED);
        }
        lv_bar_set_value(s.level_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s.time_label, "待机");
    }
}

static void route_set_enabled(bool enabled)
{
    for (int index = 0; index < 4; ++index) {
        if (enabled) {
            lv_obj_remove_state(s.route_btn[index], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s.route_btn[index], LV_STATE_DISABLED);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Recording list                                                      */
/* ------------------------------------------------------------------ */

static void list_scan(void);

static void row_click_cb(lv_event_t *event);
static void row_long_press_cb(lv_event_t *event);

static lv_obj_t *row_create(int index)
{
    lv_obj_t *row = lv_button_create(s.list);
    lv_obj_set_size(row, ROW_WIDTH, ROW_HEIGHT);
    lv_obj_set_pos(row, 0, index * ROW_PITCH);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    const char *path = s.paths[index];
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_width(name_lbl, 300);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(name_lbl, 12, 5);
    lv_obj_set_style_text_font(name_lbl, ui_font_text(), 0);

    lv_obj_t *meta = lv_label_create(row);
    char meta_text[24];
    struct stat st;
    if (stat(path, &st) == 0) {
        snprintf(meta_text, sizeof(meta_text), "%u KB",
                 (unsigned)(st.st_size / 1024));
    } else {
        lv_label_set_text(meta, "");
        meta_text[0] = '\0';
    }
    if (meta_text[0] != '\0') {
        lv_label_set_text(meta, meta_text);
    }
    lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    lv_obj_add_event_cb(row, row_long_press_cb, LV_EVENT_LONG_PRESSED,
                        (void *)(intptr_t)index);
    return row;
}

static void list_placeholder(const char *text)
{
    lv_obj_t *label = lv_label_create(s.list);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_center(label);
}

/* Directory IO runs on the LVGL thread here: the recordings folder holds a
 * few small files, so readdir/stat cost stays in the low-millisecond range
 * (the no-blocking-SD-on-UI rule targets multi-second operations). */
static void list_scan(void)
{
    lv_obj_clean(s.list);
    s.play_meta = NULL;
    s.file_count = 0;
    if (!s.sd_mounted) {
        list_placeholder("未插入 TF 卡");
        return;
    }
    char dir[112];
    snprintf(dir, sizeof(dir), "%s/%s", svc_storage_mount_point(),
             RECORD_DIR_NAME);
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        list_placeholder("暂无录音");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL && s.file_count < MAX_FILES) {
        const size_t length = strlen(entry->d_name);
        if (length < 5 ||
                strcasecmp(entry->d_name + length - 4, ".wav") != 0) {
            continue;
        }
        snprintf(s.paths[s.file_count], sizeof(s.paths[0]), "%.40s/%.54s", dir,
                 entry->d_name);
        s.file_count++;
    }
    closedir(handle);
    if (s.file_count == 0) {
        list_placeholder("暂无录音");
        return;
    }
    for (int index = 0; index < s.file_count; ++index) {
        row_create(index);
    }
}

/* ------------------------------------------------------------------ */
/* Session resets                                                      */
/* ------------------------------------------------------------------ */

static void reset_record_ui(void)
{
    s.recording = false;
    s.saving = false;
    route_set_enabled(true);
    rec_btn_refresh();
}

static void reset_play_ui(void)
{
    s.list_playing = false;
    s.play_path[0] = '\0';
    s.play_meta = NULL;
}

/* ------------------------------------------------------------------ */
/* Audio service events                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    svc_audio_event_msg_t event;
    uint32_t gen;
} audio_ui_msg_t;

static void audio_event_ui(void *arg);

/* Runs on the audio task: never touch widgets here. */
static void audio_cb(const svc_audio_event_msg_t *event, void *user)
{
    if (!s.active) {
        return;
    }
    audio_ui_msg_t *msg = malloc(sizeof(*msg));
    if (msg == NULL) {
        return;
    }
    msg->event = *event;
    msg->gen = (uint32_t)(uintptr_t)user;
    ui_async(audio_event_ui, msg);
}

/* Runs on the LVGL thread. */
static void audio_event_ui(void *arg)
{
    audio_ui_msg_t *msg = arg;
    if (!s.active || msg->gen != s.gen) {
        free(msg);
        return;
    }
    const svc_audio_event_msg_t *event = &msg->event;
    switch (event->type) {
    case SVC_AUDIO_EV_LEVEL:
        if (s.recording) {
            lv_bar_set_value(s.level_bar, event->value, LV_ANIM_OFF);
        }
        break;
    case SVC_AUDIO_EV_RECORD_DONE:
        reset_record_ui();
        ui_toast("录音已保存");
        list_scan();
        break;
    case SVC_AUDIO_EV_PLAY_PROGRESS:
        if (s.list_playing && s.play_meta != NULL) {
            lv_label_set_text_fmt(s.play_meta, LV_SYMBOL_PLAY " %ds",
                                  event->value);
        }
        break;
    case SVC_AUDIO_EV_PLAY_DONE:
        reset_play_ui();
        list_scan();
        break;
    case SVC_AUDIO_EV_ERROR:
        if (s.recording || s.saving) {
            reset_record_ui();
            ui_toast("录音失败");
        } else if (s.list_playing) {
            reset_play_ui();
            list_scan();
            ui_toast("播放失败");
        } else {
            ui_toast("音频错误");
        }
        break;
    default:
        break;
    }
    free(msg);
}

/* ------------------------------------------------------------------ */
/* User actions                                                        */
/* ------------------------------------------------------------------ */

static void record_button_cb(lv_event_t *event)
{
    (void)event;
    if (s.saving) {
        return;
    }
    if (!s.recording) {
        if (!s.sd_mounted) {
            ui_toast("请先插入 TF 卡");
            return;
        }
        if (svc_audio_is_recording() || svc_audio_is_playing()) {
            ui_toast("音频正忙");
            return;
        }
        const int gain_db = (int)lv_slider_get_value(s.gain_slider) * 3;
        s.gen++;
        const esp_err_t error =
            svc_audio_record_start(s.route, gain_db, audio_cb,
                                   (void *)(uintptr_t)s.gen);
        if (error != ESP_OK) {
            ui_toast("无法开始录音");
            return;
        }
        s.recording = true;
        s.rec_start_us = esp_timer_get_time();
        route_set_enabled(false);
        rec_btn_refresh();
    } else {
        svc_audio_record_stop();       /* capture stops; WAV save follows */
        s.saving = true;
        rec_btn_refresh();
    }
}

static void route_button_cb(lv_event_t *event)
{
    if (s.recording || s.saving) {
        return;
    }
    s.route = (svc_audio_route_t)(intptr_t)lv_event_get_user_data(event);
    route_refresh();
}

static void gain_changed_cb(lv_event_t *event)
{
    const int gain_db = (int)lv_slider_get_value(s.gain_slider) * 3;
    lv_label_set_text_fmt(s.gain_label, "增益 %d dB", gain_db);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        if (s.recording) {
            svc_audio_record_set_gain(gain_db);
        }
        demo_settings()->mic_gain_db = gain_db;
        demo_settings_save();
    }
}

static void row_click_cb(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s.file_count) {
        return;
    }
    const char *path = s.paths[index];
    if (s.recording || s.saving) {
        ui_toast("正在录音");
        return;
    }
    if (s.list_playing && strcmp(path, s.play_path) == 0) {
        svc_audio_play_stop();       /* PLAY_DONE refreshes the list */
        return;
    }
    if (s.list_playing) {
        svc_audio_play_stop();
    }
    s.gen++;
    const esp_err_t error =
        svc_audio_play(path, demo_settings()->volume, audio_cb,
                       (void *)(uintptr_t)s.gen);
    if (error != ESP_OK) {
        ui_toast("无法播放");
        return;
    }
    s.list_playing = true;
    snprintf(s.play_path, sizeof(s.play_path), "%s", path);
    lv_obj_t *row = lv_event_get_target_obj(event);
    s.play_meta = lv_obj_get_child(row, 1);
    if (s.play_meta != NULL) {
        lv_label_set_text(s.play_meta, LV_SYMBOL_PLAY " 0s");
    }
}

static void delete_confirm_cb(bool ok, void *user);

static void row_long_press_cb(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s.file_count || s.saving) {
        return;
    }
    char *path = copy_str(s.paths[index]);
    if (path == NULL) {
        return;
    }
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    char text[140];
    snprintf(text, sizeof(text), "确定删除这段录音?\n%.100s", name);
    ui_msgbox("删除录音", text, delete_confirm_cb, path);
}

/* Runs on the LVGL thread; the msgbox may outlive the app screen, so the
 * active flag guards every widget access. */
static void delete_confirm_cb(bool ok, void *user)
{
    char *path = user;
    if (!s.active || path == NULL) {
        free(path);
        return;
    }
    if (ok) {
        if (s.list_playing && strcmp(path, s.play_path) == 0) {
            svc_audio_play_stop();
            reset_play_ui();
        }
        if (unlink(path) == 0) {
            ui_toast("已删除");
        } else {
            ui_toast("删除失败");
        }
        list_scan();
    }
    free(path);
}

/* ------------------------------------------------------------------ */
/* Periodic tick: card state, elapsed time, saving indicator           */
/* ------------------------------------------------------------------ */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active) {
        return;
    }
    const bool mounted = svc_storage_mounted();
    if (mounted != s.sd_mounted) {
        s.sd_mounted = mounted;
        if (!mounted) {
            reset_play_ui();
        }
        list_scan();
        rec_btn_refresh();
    }
    if (s.recording) {
        int elapsed = (int)((esp_timer_get_time() - s.rec_start_us) /
                            1000000);
        if (elapsed > RECORD_CAP_S) {
            elapsed = RECORD_CAP_S;
        }
        if (elapsed >= RECORD_CAP_S && !s.saving) {
            /* Auto-stop fired; the WAV save is in flight. */
            s.saving = true;
            rec_btn_refresh();
        } else if (!s.saving) {
            char left[8];
            char cap[8];
            fmt_mmss(left, elapsed);
            fmt_mmss(cap, RECORD_CAP_S);
            lv_label_set_text_fmt(s.time_label, "%s / %s", left, cap);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */
/* ------------------------------------------------------------------ */

static void on_delete(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.timer != NULL) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
    if (s.recording || s.saving) {
        svc_audio_record_stop();   /* INVALID_STATE while saving: fine */
    }
    if (s.list_playing) {
        svc_audio_play_stop();
    }
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *route_button_create(lv_obj_t *parent, int index,
                                     const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 110, 42);
    lv_obj_set_pos(button, index * 114, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_text(), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, route_button_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    return button;
}

lv_obj_t *app_recorder_create(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.gen = 1;
    s.route = SVC_AUDIO_ROUTE_STEREO;
    s.sd_mounted = svc_storage_mounted();

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("录音机", &content);
    lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, NULL);

    static const char *route_names[4] = {"仅左麦", "仅右麦", "双麦", "降噪"};
    for (int index = 0; index < 4; ++index) {
        s.route_btn[index] = route_button_create(content, index,
                                                 route_names[index]);
    }
    route_refresh();

    s.gain_label = lv_label_create(content);
    lv_obj_set_pos(s.gain_label, 0, 58);
    lv_obj_set_style_text_font(s.gain_label, ui_font_text(), 0);

    s.gain_slider = lv_slider_create(content);
    lv_slider_set_range(s.gain_slider, 0, 12);
    lv_obj_set_size(s.gain_slider, 330, 16);
    lv_obj_set_pos(s.gain_slider, 118, 58);
    const int initial_gain = demo_settings()->mic_gain_db / 3;
    lv_slider_set_value(s.gain_slider, initial_gain, LV_ANIM_OFF);
    lv_obj_add_event_cb(s.gain_slider, gain_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.gain_slider, gain_changed_cb, LV_EVENT_RELEASED,
                        NULL);
    lv_label_set_text_fmt(s.gain_label, "增益 %d dB", initial_gain * 3);

    s.level_bar = lv_bar_create(content);
    lv_obj_set_size(s.level_bar, 300, 14);
    lv_obj_set_pos(s.level_bar, 0, 90);
    lv_bar_set_range(s.level_bar, 0, 100);
    lv_bar_set_value(s.level_bar, 0, LV_ANIM_OFF);

    s.time_label = lv_label_create(content);
    lv_obj_set_pos(s.time_label, 312, 86);
    lv_obj_set_width(s.time_label, 140);
    lv_obj_set_style_text_align(s.time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s.time_label, ui_font_text(), 0);
    lv_label_set_text(s.time_label, "待机");

    s.rec_btn = lv_button_create(content);
    lv_obj_set_size(s.rec_btn, 76, 76);
    lv_obj_set_pos(s.rec_btn, (452 - 76) / 2, 112);
    lv_obj_set_style_radius(s.rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COLOR_ERR), 0);
    lv_obj_set_style_border_width(s.rec_btn, 0, 0);
    lv_obj_add_event_cb(s.rec_btn, record_button_cb, LV_EVENT_CLICKED, NULL);
    s.rec_icon = lv_label_create(s.rec_btn);
    lv_label_set_text(s.rec_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(s.rec_icon);

    s.list = lv_obj_create(content);
    lv_obj_set_size(s.list, 452, 180);
    lv_obj_set_pos(s.list, 0, 196);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_scrollbar_mode(s.list, LV_SCROLLBAR_MODE_AUTO);

    list_scan();
    rec_btn_refresh();
    s.timer = lv_timer_create(tick_cb, 500, NULL);
    return root;
}
