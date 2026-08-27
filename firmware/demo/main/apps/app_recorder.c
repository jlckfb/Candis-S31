/*
 * Candis-S31 watch demo - recorder app.
 *
 * Four-way input route (left mic only / right mic only / dual mic / basic
 * noise reduction), 0-36 dB gain slider in 3 dB steps, live level meter,
 * record button with elapsed time, and the saved-recording list (tap =
 * play/stop, long-press = delete with confirm). The heavy lifting lives in
 * svc_audio; this app renders state and forwards user intent.
 *
 * Threading: svc callbacks and background TF scans are forwarded through
 * ui_async() with generation counters, so stale work can never touch widgets
 * of a new session. The static state plus active flag follow the app teardown
 * discipline: on LV_EVENT_DELETE the app stops every service operation it
 * started.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define MAX_FILES        32
#define ROW_WIDTH        428
#define ROW_HEIGHT       64
#define ROW_PITCH        68
#define RECORD_DIR_NAME  "recordings"
#define RECORD_CAP_S     60
#define SCAN_TASK_STACK  4096
#define SCAN_TASK_PRIO   (tskIDLE_PRIORITY + 1)
#define SCAN_POST_RETRIES 50
#define SCAN_POST_RETRY_MS 20

typedef struct {
    bool active;
    uint32_t session_id;   /* page lifetime, filters stale scan results */
    uint32_t scan_gen;
    bool scan_running;
    bool scan_again;
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
    uint32_t sizes_kb[MAX_FILES];
} recorder_state_t;

static recorder_state_t s;
static uint32_t s_session_counter;
static atomic_uint_fast32_t s_live_session;
static atomic_uint_fast32_t s_failed_session;
static uint32_t s_operation_counter;
static atomic_uint_fast32_t s_live_audio_token;
static atomic_uint_fast32_t s_pending_audio_token;
static atomic_uint_fast32_t s_pending_audio_type;

typedef enum {
    SCAN_RESULT_FILES = 0,
    SCAN_RESULT_NO_CARD,
    SCAN_RESULT_EMPTY,
} scan_result_status_t;

typedef struct {
    uint32_t session_id;
    uint32_t scan_gen;
    bool mounted;
    char mount[32];
    scan_result_status_t status;
    int file_count;
    char paths[MAX_FILES][96];
    uint32_t sizes_kb[MAX_FILES];
} recorder_scan_result_t;

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
                lv_color_hex(selected ? UI_COL_ACCENT : UI_COL_SURFACE),
                0);
        lv_obj_set_style_border_width(s.route_btn[index], selected ? 0 : 1, 0);
    }
}

static void rec_btn_refresh(void)
{
    if (s.saving) {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_SAVE);
        lv_obj_set_style_radius(s.rec_btn, 14, 0);
        lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COL_WARN), 0);
        lv_obj_add_state(s.rec_btn, LV_STATE_DISABLED);
        lv_label_set_text(s.time_label, "Saving...");
    } else if (s.recording) {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_STOP);
        lv_obj_set_style_radius(s.rec_btn, 14, 0);
        lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COL_FAIL), 0);
        lv_obj_remove_state(s.rec_btn, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s.rec_icon, LV_SYMBOL_AUDIO);
        lv_obj_set_style_radius(s.rec_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s.rec_btn,
                lv_color_hex(s.sd_mounted ? UI_COL_FAIL : UI_COL_SURFACE),
                0);
        if (s.sd_mounted) {
            lv_obj_remove_state(s.rec_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s.rec_btn, LV_STATE_DISABLED);
        }
        lv_bar_set_value(s.level_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s.time_label, "Idle");
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

static void list_scan_start(void);

static void row_click_cb(lv_event_t *event);
static void row_long_press_cb(lv_event_t *event);

static lv_obj_t *row_create(int index)
{
    lv_obj_t *row = lv_button_create(s.list);
    lv_obj_set_size(row, ROW_WIDTH, ROW_HEIGHT);
    lv_obj_set_pos(row, 0, index * ROW_PITCH);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    const char *path = s.paths[index];
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;

    lv_obj_t *name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_width(name_lbl, 292);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_font(name_lbl, ui_font_body(), 0);

    lv_obj_t *meta = lv_label_create(row);
    lv_label_set_text_fmt(meta, "%u KB", (unsigned)s.sizes_kb[index]);
    lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(UI_COL_TEXT_DIM), 0);

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
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_center(label);
}

/* Runs on the LVGL thread. Page/session generations reject results from a
 * closed page, removed card or superseded refresh. */
static void list_scan_apply(void *arg)
{
    recorder_scan_result_t *result = arg;
    if (!s.active || result->session_id != s.session_id) {
        free(result);
        return;
    }

    s.scan_running = false;
    if (result->scan_gen == s.scan_gen) {
        lv_obj_clean(s.list);
        s.play_meta = NULL;
        s.file_count = result->file_count;
        memcpy(s.paths, result->paths, sizeof(s.paths));
        memcpy(s.sizes_kb, result->sizes_kb, sizeof(s.sizes_kb));
        if (result->status == SCAN_RESULT_NO_CARD) {
            list_placeholder("No TF card");
        } else if (result->status == SCAN_RESULT_EMPTY) {
            list_placeholder("No recordings");
        } else {
            for (int index = 0; index < s.file_count; ++index) {
                row_create(index);
            }
        }
    }
    free(result);

    if (s.scan_again) {
        s.scan_again = false;
        list_scan_start();
    }
}

/* The scan task keeps ownership until LVGL accepts the result. Retry is
 * bounded to about one second; cancellation/failure is published atomically
 * for the page timer to clear scan_running without cross-thread UI access. */
static bool list_scan_post(recorder_scan_result_t *result)
{
    for (int attempt = 0; attempt < SCAN_POST_RETRIES; ++attempt) {
        if (atomic_load_explicit(&s_live_session, memory_order_acquire) !=
                result->session_id) {
            free(result);
            return false;
        }
        if (ui_async(list_scan_apply, result)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_POST_RETRY_MS));
    }
    if (atomic_load_explicit(&s_live_session, memory_order_acquire) ==
            result->session_id) {
        atomic_store_explicit(&s_failed_session, result->session_id,
                              memory_order_release);
    }
    free(result);
    return false;
}

/* This task owns its request/result buffer and never reads page state. */
static void list_scan_task(void *arg)
{
    recorder_scan_result_t *result = arg;
    if (!result->mounted) {
        result->status = SCAN_RESULT_NO_CARD;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }

    char dir[112];
    snprintf(dir, sizeof(dir), "%s/%s", result->mount, RECORD_DIR_NAME);
    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        result->status = SCAN_RESULT_NO_CARD;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }
    DIR *handle = opendir(dir);
    if (handle == NULL) {
        svc_storage_lease_release(&lease);
        result->status = SCAN_RESULT_EMPTY;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL &&
            result->file_count < MAX_FILES) {
        const size_t length = strlen(entry->d_name);
        if (length < 5 ||
                strcasecmp(entry->d_name + length - 4, ".wav") != 0) {
            continue;
        }
        const int index = result->file_count;
        snprintf(result->paths[index], sizeof(result->paths[0]),
                 "%.40s/%.54s", dir, entry->d_name);
        struct stat st;
        result->sizes_kb[index] = stat(result->paths[index], &st) == 0 ?
                                  (uint32_t)(st.st_size / 1024) : 0;
        result->file_count++;
    }
    closedir(handle);
    svc_storage_lease_release(&lease);
    result->status = result->file_count == 0 ? SCAN_RESULT_EMPTY :
                                               SCAN_RESULT_FILES;
    (void)list_scan_post(result);
    vTaskDelete(NULL);
}

/* Runs on the LVGL thread. Repeated requests coalesce behind one task. */
static void list_scan_start(void)
{
    s.scan_gen++;
    if (!s.sd_mounted) {
        s.scan_again = false;
        lv_obj_clean(s.list);
        s.play_meta = NULL;
        s.file_count = 0;
        list_placeholder("No TF card");
        return;
    }
    if (s.scan_running) {
        s.scan_again = true;
        return;
    }

    lv_obj_clean(s.list);
    s.play_meta = NULL;
    s.file_count = 0;
    recorder_scan_result_t *result = calloc(1, sizeof(*result));
    if (result == NULL) {
        list_placeholder("Scan out of memory");
        return;
    }
    result->session_id = s.session_id;
    result->scan_gen = s.scan_gen;
    result->mounted = s.sd_mounted;
    snprintf(result->mount, sizeof(result->mount), "%s",
             svc_storage_mount_point());
    s.scan_running = true;
    if (xTaskCreate(list_scan_task, "recorder_scan", SCAN_TASK_STACK, result,
                    SCAN_TASK_PRIO, NULL) != pdPASS) {
        s.scan_running = false;
        free(result);
        list_placeholder("Scan task start failed");
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

static uint32_t audio_token_begin(void)
{
    uint32_t token = ++s_operation_counter;
    if (token == 0) {
        token = ++s_operation_counter;
    }
    atomic_store_explicit(&s_pending_audio_token, 0,
                          memory_order_release);
    atomic_store_explicit(&s_live_audio_token, token, memory_order_release);
    return token;
}

static void audio_token_invalidate(uint32_t token)
{
    uint_fast32_t expected = token;
    (void)atomic_compare_exchange_strong_explicit(
        &s_live_audio_token, &expected, 0, memory_order_acq_rel,
        memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Audio service events                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    svc_audio_event_msg_t event;
    uint32_t token;
} audio_ui_msg_t;

static void audio_event_ui(void *arg);

static bool audio_event_is_terminal(svc_audio_event_t type)
{
    return type == SVC_AUDIO_EV_RECORD_DONE ||
           type == SVC_AUDIO_EV_PLAY_DONE || type == SVC_AUDIO_EV_ERROR;
}

static void audio_terminal_defer(uint32_t token, svc_audio_event_t type)
{
    if (atomic_load_explicit(&s_live_audio_token, memory_order_acquire) !=
            token) {
        return;
    }
    atomic_store_explicit(&s_pending_audio_type, (uint32_t)type + 1U,
                          memory_order_relaxed);
    atomic_store_explicit(&s_pending_audio_token, token,
                          memory_order_release);
}

/* Runs on the audio task: never touch widgets here. */
static void audio_cb(const svc_audio_event_msg_t *event, void *user)
{
    const uint32_t token = (uint32_t)(uintptr_t)user;
    if (atomic_load_explicit(&s_live_audio_token, memory_order_acquire) !=
            token) {
        return;
    }
    audio_ui_msg_t *msg = malloc(sizeof(*msg));
    if (msg == NULL) {
        if (audio_event_is_terminal(event->type)) {
            audio_terminal_defer(token, event->type);
        }
        return;
    }
    msg->event = *event;
    msg->token = token;
    if (!ui_async(audio_event_ui, msg)) {
        if (audio_event_is_terminal(event->type)) {
            audio_terminal_defer(token, event->type);
        }
        free(msg);
    }
}

static void audio_event_apply(const svc_audio_event_msg_t *event,
                              uint32_t token)
{
    if (!s.active || atomic_load_explicit(&s_live_audio_token,
                                          memory_order_acquire) != token) {
        return;
    }
    switch (event->type) {
    case SVC_AUDIO_EV_LEVEL:
        if (s.recording) {
            lv_bar_set_value(s.level_bar, event->value, LV_ANIM_OFF);
        }
        break;
    case SVC_AUDIO_EV_RECORD_DONE:
        audio_token_invalidate(token);
        reset_record_ui();
        ui_toast("Recording saved");
        list_scan_start();
        break;
    case SVC_AUDIO_EV_PLAY_PROGRESS:
        if (s.list_playing && s.play_meta != NULL) {
            lv_label_set_text_fmt(s.play_meta, LV_SYMBOL_PLAY " %ds",
                                  event->value);
        }
        break;
    case SVC_AUDIO_EV_PLAY_DONE:
        audio_token_invalidate(token);
        reset_play_ui();
        list_scan_start();
        break;
    case SVC_AUDIO_EV_ERROR:
        audio_token_invalidate(token);
        if (s.recording || s.saving) {
            reset_record_ui();
            ui_toast("Record failed");
        } else if (s.list_playing) {
            reset_play_ui();
            list_scan_start();
            ui_toast("Playback failed");
        } else {
            ui_toast("Audio error");
        }
        break;
    default:
        break;
    }
}

/* Runs on the LVGL thread. */
static void audio_event_ui(void *arg)
{
    audio_ui_msg_t *msg = arg;
    audio_event_apply(&msg->event, msg->token);
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
            ui_toast("Insert TF card first");
            return;
        }
        if (svc_audio_is_recording() || svc_audio_is_playing()) {
            ui_toast("Audio busy");
            return;
        }
        const int gain_db = (int)lv_slider_get_value(s.gain_slider) * 3;
        const uint32_t token = audio_token_begin();
        const esp_err_t error =
            svc_audio_record_start(s.route, gain_db, audio_cb,
                                   (void *)(uintptr_t)token);
        if (error != ESP_OK) {
            audio_token_invalidate(token);
            ui_toast("Cannot start recording");
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
    lv_label_set_text_fmt(s.gain_label, "Gain %d dB", gain_db);
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
        ui_toast("Recording");
        return;
    }
    if (s.list_playing && strcmp(path, s.play_path) == 0) {
        const uint32_t token = (uint32_t)atomic_load_explicit(
            &s_live_audio_token, memory_order_acquire);
        audio_token_invalidate(token);
        svc_audio_play_stop();
        reset_play_ui();
        list_scan_start();
        return;
    }
    if (s.list_playing) {
        const uint32_t token = (uint32_t)atomic_load_explicit(
            &s_live_audio_token, memory_order_acquire);
        audio_token_invalidate(token);
        svc_audio_play_stop();
        reset_play_ui();
    }
    const uint32_t token = audio_token_begin();
    const esp_err_t error =
        svc_audio_play(path, demo_settings()->volume, audio_cb,
                       (void *)(uintptr_t)token);
    if (error != ESP_OK) {
        audio_token_invalidate(token);
        ui_toast("Cannot play");
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
    snprintf(text, sizeof(text), "Delete this recording?\n%.100s", name);
    ui_msgbox("Delete recording", text, delete_confirm_cb, path);
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
            const uint32_t token = (uint32_t)atomic_load_explicit(
                &s_live_audio_token, memory_order_acquire);
            audio_token_invalidate(token);
            svc_audio_play_stop();
            reset_play_ui();
        }
        svc_storage_lease_t lease = {0};
        if (svc_storage_lease_acquire(&lease) != ESP_OK) {
            ui_toast("TF card removing");
            free(path);
            return;
        }
        const int unlink_result = unlink(path);
        svc_storage_lease_release(&lease);
        if (unlink_result == 0) {
            ui_toast("Deleted");
        } else {
            ui_toast("Delete failed");
        }
        list_scan_start();
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
    const uint32_t pending_token = (uint32_t)atomic_exchange_explicit(
        &s_pending_audio_token, 0, memory_order_acq_rel);
    if (pending_token != 0) {
        svc_audio_event_msg_t event = {
            .type = (svc_audio_event_t)((uint32_t)atomic_load_explicit(
                &s_pending_audio_type, memory_order_relaxed) - 1U),
        };
        audio_event_apply(&event, pending_token);
    }
    const uint32_t failed_session = (uint32_t)atomic_exchange_explicit(
        &s_failed_session, 0, memory_order_acq_rel);
    if (failed_session == s.session_id && s.scan_running) {
        s.scan_running = false;
        s.scan_again = false;
        lv_obj_clean(s.list);
        s.play_meta = NULL;
        s.file_count = 0;
        list_placeholder("Scan failed, retry");
    }
    const bool mounted = svc_storage_mounted();
    if (mounted != s.sd_mounted) {
        s.sd_mounted = mounted;
        if (!mounted) {
            if (s.recording && !s.saving) {
                /* The card went away while capturing: the WAV save cannot
                 * succeed anyway. Invalidate the token FIRST so every late
                 * completion event is dropped, then stop asynchronously -
                 * a synchronous stop would block the LVGL thread up to the
                 * ack timeout on a write that cannot complete. s.saving is
                 * NOT set here (not even when the stop command reaches the
                 * audio task): with the token invalidated the completion
                 * event can never clear the flag again, and an enqueue
                 * failure means no save runs at all. Reset the UI locally
                 * instead - the card-less state disables the record button
                 * via rec_btn_refresh(). */
                const uint32_t token = (uint32_t)atomic_load_explicit(
                    &s_live_audio_token, memory_order_acquire);
                audio_token_invalidate(token);
                (void)svc_audio_record_stop_async();
                reset_record_ui();
            }
            if (s.list_playing) {
                const uint32_t token = (uint32_t)atomic_load_explicit(
                    &s_live_audio_token, memory_order_acquire);
                audio_token_invalidate(token);
                svc_audio_play_stop();
            }
            reset_play_ui();
        }
        list_scan_start();
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
    atomic_store_explicit(&s_live_session, 0, memory_order_release);
    atomic_store_explicit(&s_live_audio_token, 0, memory_order_release);
    atomic_store_explicit(&s_pending_audio_token, 0,
                          memory_order_release);
    s.scan_gen++;
    s.scan_again = false;
    if (s.timer != NULL) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
    if (s.recording || s.saving) {
        /* Async: the audio task may sit in a slow SD write; a synchronous
         * stop would block the LVGL thread up to the ack timeout during
         * navigation. The token was zeroed above, so the late completion
         * event is dropped by audio_cb's liveness check. */
        svc_audio_record_stop_async(); /* INVALID_STATE while saving: fine */
    }
    if (s.list_playing) {
        svc_audio_play_stop_async();
    }
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

static lv_obj_t *route_button_create(lv_obj_t *parent, int index,
                                     const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 101, UI_TOUCH_MIN);
    lv_obj_set_pos(button, index * 109, 0);
    lv_obj_set_style_pad_hor(button, 4, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_body(), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, route_button_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
    return button;
}

lv_obj_t *app_recorder_create(void)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.session_id = ++s_session_counter;
    atomic_store_explicit(&s_failed_session, 0, memory_order_release);
    atomic_store_explicit(&s_live_session, s.session_id,
                          memory_order_release);
    atomic_store_explicit(&s_live_audio_token, 0, memory_order_release);
    atomic_store_explicit(&s_pending_audio_token, 0,
                          memory_order_release);
    atomic_store_explicit(&s_pending_audio_type, 0, memory_order_relaxed);
    s.route = SVC_AUDIO_ROUTE_STEREO;
    s.sd_mounted = svc_storage_mounted();

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Recorder", &content);
    lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    static const char *route_names[4] = {
        "Left mic", "Right mic", "Both mics", "Basic NR"
    };
    for (int index = 0; index < 4; ++index) {
        s.route_btn[index] = route_button_create(content, index,
                                                 route_names[index]);
    }
    route_refresh();

    s.gain_label = lv_label_create(content);
    lv_obj_set_pos(s.gain_label, 0, 74);
    lv_obj_set_style_text_font(s.gain_label, ui_font_body(), 0);

    s.gain_slider = lv_slider_create(content);
    lv_slider_set_range(s.gain_slider, 0, 12);
    lv_obj_set_size(s.gain_slider, 316, UI_TOUCH_MIN);
    lv_obj_set_pos(s.gain_slider, 112, 56);
    const int initial_gain = demo_settings()->mic_gain_db / 3;
    lv_slider_set_value(s.gain_slider, initial_gain, LV_ANIM_OFF);
    lv_obj_add_event_cb(s.gain_slider, gain_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.gain_slider, gain_changed_cb, LV_EVENT_RELEASED,
                        NULL);
    lv_label_set_text_fmt(s.gain_label, "Gain %d dB", initial_gain * 3);

    s.level_bar = lv_bar_create(content);
    lv_obj_set_size(s.level_bar, 300, 14);
    lv_obj_set_pos(s.level_bar, 0, 120);
    lv_bar_set_range(s.level_bar, 0, 100);
    lv_bar_set_value(s.level_bar, 0, LV_ANIM_OFF);

    s.time_label = lv_label_create(content);
    lv_obj_set_pos(s.time_label, 288, 116);
    lv_obj_set_width(s.time_label, 140);
    lv_obj_set_style_text_align(s.time_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s.time_label, ui_font_body(), 0);
    lv_label_set_text(s.time_label, "Idle");

    s.rec_btn = lv_button_create(content);
    lv_obj_set_size(s.rec_btn, 76, 76);
    lv_obj_set_pos(s.rec_btn, (428 - 76) / 2, 144);
    lv_obj_set_style_radius(s.rec_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s.rec_btn, lv_color_hex(UI_COL_FAIL), 0);
    lv_obj_set_style_border_width(s.rec_btn, 0, 0);
    lv_obj_add_event_cb(s.rec_btn, record_button_cb, LV_EVENT_CLICKED, NULL);
    s.rec_icon = lv_label_create(s.rec_btn);
    lv_label_set_text(s.rec_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(s.rec_icon);

    s.list = lv_obj_create(content);
    lv_obj_set_size(s.list, 428, 148);
    lv_obj_set_pos(s.list, 0, 228);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_scrollbar_mode(s.list, LV_SCROLLBAR_MODE_AUTO);

    list_scan_start();
    rec_btn_refresh();
    s.timer = lv_timer_create(tick_cb, 500, NULL);
    return root;
}
