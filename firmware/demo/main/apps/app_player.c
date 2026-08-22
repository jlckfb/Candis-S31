/*
 * Candis-S31 watch demo - WAV player app.
 *
 * Browses WAV files on the TF card (card root plus one directory level,
 * which includes /sdcard/recordings), plays them through svc_audio with
 * play/pause/stop, a 0-100 volume slider (default from demo_settings())
 * and a progress bar with elapsed/total time.
 *
 * Threading follows the recorder app: svc events and background TF scans are
 * forwarded through ui_async() with generation counters; the static state
 * plus active flag implement the teardown discipline (on LV_EVENT_DELETE the
 * app stops playback it started).
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "demo_apps.h"
#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define MAX_FILES   48
#define ROW_WIDTH   428
#define ROW_HEIGHT  64
#define ROW_PITCH   68
#define SCAN_TASK_STACK 4096
#define SCAN_TASK_PRIO  (tskIDLE_PRIORITY + 1)
#define SCAN_POST_RETRIES 50
#define SCAN_POST_RETRY_MS 20

typedef enum {
    PLAY_STATE_IDLE = 0,
    PLAY_STATE_PLAYING,
    PLAY_STATE_PAUSED,
} play_state_t;

typedef struct {
    bool active;
    uint32_t session_id;   /* page lifetime, filters stale scan results */
    uint32_t scan_gen;
    bool scan_running;
    bool scan_again;
    /* widgets */
    lv_obj_t *now_label;
    lv_obj_t *progress_bar;
    lv_obj_t *time_label;
    lv_obj_t *play_btn;
    lv_obj_t *play_icon;
    lv_obj_t *stop_btn;
    lv_obj_t *vol_slider;
    lv_obj_t *vol_label;
    lv_obj_t *list;
    lv_timer_t *timer;
    /* runtime */
    bool sd_mounted;
    play_state_t pstate;
    char cur_path[96];
    char cur_name[48];
    int cur_total_s;
    int file_count;
    char paths[MAX_FILES][96];
    char names[MAX_FILES][48];
    int durations[MAX_FILES];   /* seconds, -1 when unknown */
} player_state_t;

static player_state_t s;
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
    SCAN_RESULT_OPEN_FAILED,
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
    char names[MAX_FILES][48];
    int durations[MAX_FILES];
} player_scan_result_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void fmt_mmss(char buffer[8], int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    int minutes = seconds / 60;
    if (minutes > 99) {
        minutes = 99;
    }
    snprintf(buffer, 8, "%02d:%02d", minutes, seconds % 60);
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool has_wav_suffix(const char *name)
{
    const size_t length = strlen(name);
    return length >= 5 && strcasecmp(name + length - 4, ".wav") == 0;
}

/* Exact sample rates the ES8389 supports under this board's clocking
 * policy - mirrors wav_sample_rate_supported() in svc_audio.c. */
/* Duration sentinel: WAV decodable but sample rate unsupported. */
#define WAV_UNSUPPORTED (-2)

static bool wav_rate_supported(uint32_t rate)
{
    return rate == 8000U || rate == 16000U || rate == 44100U ||
           rate == 48000U;
}

/* Duration probe: walk the RIFF chunks for fmt/data and derive seconds
 * from byte_rate. Return values: >=0 duration, -1 unknown/malformed
 * (still listed, svc_audio decides at play time), WAV_UNSUPPORTED
 * (-2) sample rate outside the supported set - the row is rendered
 * disabled and cannot be played. */
static int wav_duration_seconds(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
    int seconds = -1;
    uint8_t header[12];
    uint32_t byte_rate = 0;
    uint32_t data_size = 0;
    bool rate_ok = false;
    if (fread(header, 1, sizeof(header), file) == sizeof(header) &&
            memcmp(header, "RIFF", 4) == 0 &&
            memcmp(header + 8, "WAVE", 4) == 0) {
        for (int chunk = 0; chunk < 16; ++chunk) {
            uint8_t chunk_header[8];
            if (fread(chunk_header, 1, sizeof(chunk_header), file) !=
                    sizeof(chunk_header)) {
                break;
            }
            const uint32_t size = read_le32(chunk_header + 4);
            const long payload = ftell(file);
            if (payload < 0) {
                break;
            }
            if (memcmp(chunk_header, "fmt ", 4) == 0 && size >= 16) {
                uint8_t format[16];
                if (fread(format, 1, sizeof(format), file) !=
                        sizeof(format)) {
                    break;
                }
                if (read_le16(format) == 1 && read_le16(format + 14) == 16) {
                    if (!wav_rate_supported(read_le32(format + 4))) {
                        seconds = WAV_UNSUPPORTED;
                        break;
                    }
                    rate_ok = true;
                    byte_rate = read_le32(format + 8);
                }
            } else if (memcmp(chunk_header, "data", 4) == 0) {
                data_size = size;
            }
            if (rate_ok && byte_rate != 0 && data_size != 0) {
                seconds = (int)((data_size + byte_rate / 2) / byte_rate);
                break;
            }
            if (fseek(file, payload + (long)size + (size & 1U), SEEK_SET) !=
                    0) {
                break;
            }
        }
    }
    fclose(file);
    return seconds;
}

/* ------------------------------------------------------------------ */
/* Widget refresh                                                      */
/* ------------------------------------------------------------------ */

static void transport_refresh(void)
{
    if (s.pstate == PLAY_STATE_PLAYING) {
        lv_label_set_text(s.play_icon, LV_SYMBOL_PAUSE);
        lv_obj_remove_state(s.play_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s.stop_btn, LV_STATE_DISABLED);
    } else if (s.pstate == PLAY_STATE_PAUSED) {
        lv_label_set_text(s.play_icon, LV_SYMBOL_PLAY);
        lv_obj_remove_state(s.play_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(s.stop_btn, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(s.play_icon, LV_SYMBOL_PLAY);
        lv_obj_remove_state(s.play_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s.stop_btn, LV_STATE_DISABLED);
    }
}

static void time_label_refresh(int elapsed)
{
    char left[8];
    char total[8];
    fmt_mmss(left, elapsed);
    fmt_mmss(total, s.cur_total_s);
    lv_label_set_text_fmt(s.time_label, "%s / %s", left, total);
}

static void progress_reset(void)
{
    lv_bar_set_range(s.progress_bar, 0,
                     s.cur_total_s > 0 ? s.cur_total_s : 1);
    lv_bar_set_value(s.progress_bar, 0, LV_ANIM_OFF);
    time_label_refresh(0);
}

/* ------------------------------------------------------------------ */
/* File list                                                           */
/* ------------------------------------------------------------------ */

static void list_placeholder(const char *text)
{
    lv_obj_t *label = lv_label_create(s.list);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_center(label);
}

static void row_click_cb(lv_event_t *event);

static void row_create(int index)
{
    lv_obj_t *row = lv_button_create(s.list);
    lv_obj_set_size(row, ROW_WIDTH, ROW_HEIGHT);
    lv_obj_set_pos(row, 0, index * ROW_PITCH);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, s.names[index]);
    lv_obj_set_width(name, 320);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_font(name, ui_font_body(), 0);

    lv_obj_t *meta = lv_label_create(row);
    if (s.durations[index] == WAV_UNSUPPORTED) {
        /* Unplayable on this hardware: dim the whole row and show why
         * instead of failing only after the user taps it. */
        lv_obj_set_style_text_color(name, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
        lv_label_set_text(meta, "unsupported");
    } else if (s.durations[index] >= 0) {
        char meta_text[8];
        fmt_mmss(meta_text, s.durations[index]);
        lv_label_set_text(meta, meta_text);
    } else {
        lv_label_set_text(meta, "--:--");
    }
    lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)index);
}

static void scan_add_file(player_scan_result_t *result, const char *full_path,
                          const char *display)
{
    if (result->file_count >= MAX_FILES) {
        return;
    }
    if (strlen(full_path) >= sizeof(result->paths[0])) {
        return; /* FAT long names beyond our buffer: skip, do not truncate */
    }
    const int index = result->file_count;
    snprintf(result->paths[index], sizeof(result->paths[0]), "%s", full_path);
    snprintf(result->names[index], sizeof(result->names[0]), "%s", display);
    result->durations[index] = wav_duration_seconds(full_path);
    result->file_count++;
}

static void list_scan_start(void);

/* Runs on the LVGL thread. A stale result may be delivered after page exit,
 * card removal or a newer refresh; the page/session generations reject it. */
static void list_scan_apply(void *arg)
{
    player_scan_result_t *result = arg;
    if (!s.active || result->session_id != s.session_id) {
        free(result);
        return;
    }

    s.scan_running = false;
    if (result->scan_gen == s.scan_gen) {
        lv_obj_clean(s.list);
        s.file_count = result->file_count;
        memcpy(s.paths, result->paths, sizeof(s.paths));
        memcpy(s.names, result->names, sizeof(s.names));
        memcpy(s.durations, result->durations, sizeof(s.durations));

        if (result->status == SCAN_RESULT_NO_CARD) {
            list_placeholder("No TF card");
        } else if (result->status == SCAN_RESULT_OPEN_FAILED) {
            list_placeholder("Cannot open TF card");
        } else if (result->status == SCAN_RESULT_EMPTY) {
            list_placeholder("No WAV files found");
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
static bool list_scan_post(player_scan_result_t *result)
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

/* Scans the card root and one directory level (covers recordings/).
 * This task owns its request/result buffer and never reads page state. */
static void list_scan_task(void *arg)
{
    player_scan_result_t *result = arg;
    if (!result->mounted) {
        result->status = SCAN_RESULT_NO_CARD;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }

    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        result->status = SCAN_RESULT_NO_CARD;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }
    DIR *root = opendir(result->mount);
    if (root == NULL) {
        svc_storage_lease_release(&lease);
        result->status = SCAN_RESULT_OPEN_FAILED;
        (void)list_scan_post(result);
        vTaskDelete(NULL);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL &&
            result->file_count < MAX_FILES) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char full[160];
        snprintf(full, sizeof(full), "%.24s/%.134s", result->mount,
                 entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            DIR *sub = opendir(full);
            if (sub == NULL) {
                continue;
            }
            struct dirent *sub_entry;
            while ((sub_entry = readdir(sub)) != NULL &&
                    result->file_count < MAX_FILES) {
                if (sub_entry->d_name[0] == '.' ||
                        !has_wav_suffix(sub_entry->d_name)) {
                    continue;
                }
                char sub_full[160];
                snprintf(sub_full, sizeof(sub_full), "%.40s/%.118s", full,
                         sub_entry->d_name);
                struct stat sub_st;
                if (stat(sub_full, &sub_st) != 0 ||
                        !S_ISREG(sub_st.st_mode)) {
                    continue;
                }
                char display[56];
                snprintf(display, sizeof(display), "%s/%s", entry->d_name,
                         sub_entry->d_name);
                scan_add_file(result, sub_full, display);
            }
            closedir(sub);
        } else if (S_ISREG(st.st_mode) && has_wav_suffix(entry->d_name)) {
            scan_add_file(result, full, entry->d_name);
        }
    }
    closedir(root);
    svc_storage_lease_release(&lease);
    result->status = result->file_count == 0 ? SCAN_RESULT_EMPTY :
                                               SCAN_RESULT_FILES;
    (void)list_scan_post(result);
    vTaskDelete(NULL);
}

/* Runs on the LVGL thread. At most one scan per page is active; repeated
 * refreshes coalesce into one newer generation. */
static void list_scan_start(void)
{
    s.scan_gen++;
    if (!s.sd_mounted) {
        s.scan_again = false;
        lv_obj_clean(s.list);
        s.file_count = 0;
        list_placeholder("No TF card");
        return;
    }
    if (s.scan_running) {
        s.scan_again = true;
        return;
    }

    lv_obj_clean(s.list);
    s.file_count = 0;
    player_scan_result_t *result = calloc(1, sizeof(*result));
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
    if (xTaskCreate(list_scan_task, "player_scan", SCAN_TASK_STACK, result,
                    SCAN_TASK_PRIO, NULL) != pdPASS) {
        s.scan_running = false;
        free(result);
        list_placeholder("Scan task start failed");
    }
}

/* ------------------------------------------------------------------ */
/* Playback control                                                    */
/* ------------------------------------------------------------------ */

/* Defined with its event landing below; start_play only needs the address. */
static void audio_cb(const svc_audio_event_msg_t *event, void *user);

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

static void start_play(const char *path)
{
    if (!s.sd_mounted) {
        ui_toast("No TF card");
        return;
    }
    if (svc_audio_is_recording()) {
        ui_toast("Recording, cannot play");
        return;
    }
    const uint32_t token = audio_token_begin();
    const int volume = (int)lv_slider_get_value(s.vol_slider);
    const esp_err_t error = svc_audio_play(path, volume, audio_cb,
                                           (void *)(uintptr_t)token);
    if (error != ESP_OK) {
        audio_token_invalidate(token);
        ui_toast("Cannot play");
        return;
    }
    s.pstate = PLAY_STATE_PLAYING;
    lv_label_set_text(s.now_label, s.cur_name);
    progress_reset();
    transport_refresh();
}

static void play_button_cb(lv_event_t *event)
{
    (void)event;
    if (s.pstate == PLAY_STATE_PLAYING) {
        if (svc_audio_play_pause(true) == ESP_OK) {
            s.pstate = PLAY_STATE_PAUSED;
            transport_refresh();
        }
    } else if (s.pstate == PLAY_STATE_PAUSED) {
        if (svc_audio_play_pause(false) == ESP_OK) {
            s.pstate = PLAY_STATE_PLAYING;
            transport_refresh();
        }
    } else if (s.cur_path[0] != '\0') {
        start_play(s.cur_path);
    } else {
        ui_toast("Select a file first");
    }
}

static void stop_button_cb(lv_event_t *event)
{
    (void)event;
    if (s.pstate == PLAY_STATE_IDLE) {
        return;
    }
    const uint32_t token = (uint32_t)atomic_load_explicit(
        &s_live_audio_token, memory_order_acquire);
    audio_token_invalidate(token);
    svc_audio_play_stop();
    s.pstate = PLAY_STATE_IDLE;
    progress_reset();
    transport_refresh();
}

static void row_click_cb(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s.file_count) {
        return;
    }
    if (s.durations[index] == WAV_UNSUPPORTED) {
        ui_toast("Sample rate not supported");
        return;
    }
    const char *path = s.paths[index];
    if (s.pstate != PLAY_STATE_IDLE && strcmp(path, s.cur_path) == 0) {
        play_button_cb(event);   /* same file: toggle pause/resume */
        return;
    }
    if (s.pstate != PLAY_STATE_IDLE) {
        const uint32_t token = (uint32_t)atomic_load_explicit(
            &s_live_audio_token, memory_order_acquire);
        audio_token_invalidate(token);
        svc_audio_play_stop();
        s.pstate = PLAY_STATE_IDLE;
    }
    snprintf(s.cur_path, sizeof(s.cur_path), "%s", path);
    snprintf(s.cur_name, sizeof(s.cur_name), "%s", s.names[index]);
    s.cur_total_s = s.durations[index];
    start_play(path);
}

static void volume_changed_cb(lv_event_t *event)
{
    const int volume = (int)lv_slider_get_value(s.vol_slider);
    lv_label_set_text_fmt(s.vol_label, "%d", volume);
    if (s.pstate != PLAY_STATE_IDLE) {
        svc_audio_set_volume(volume);
    }
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        demo_settings()->volume = volume;
        demo_settings_save();
    }
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
    return type == SVC_AUDIO_EV_PLAY_DONE || type == SVC_AUDIO_EV_ERROR;
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
    case SVC_AUDIO_EV_PLAY_PROGRESS:
        if (s.pstate != PLAY_STATE_IDLE) {
            const int elapsed = event->value;
            lv_bar_set_value(s.progress_bar,
                             s.cur_total_s > 0 && elapsed > s.cur_total_s ?
                             s.cur_total_s : elapsed,
                             LV_ANIM_OFF);
            time_label_refresh(elapsed);
        }
        break;
    case SVC_AUDIO_EV_PLAY_DONE:
        audio_token_invalidate(token);
        s.pstate = PLAY_STATE_IDLE;
        progress_reset();
        transport_refresh();
        break;
    case SVC_AUDIO_EV_ERROR:
        audio_token_invalidate(token);
        s.pstate = PLAY_STATE_IDLE;
        progress_reset();
        transport_refresh();
        ui_toast("Playback failed");
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
/* Periodic tick: card hot-plug                                        */
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
        s.file_count = 0;
        list_placeholder("Scan failed, retry");
    }
    const bool mounted = svc_storage_mounted();
    if (mounted != s.sd_mounted) {
        s.sd_mounted = mounted;
        if (!mounted) {
            const uint32_t token = (uint32_t)atomic_load_explicit(
                &s_live_audio_token, memory_order_acquire);
            audio_token_invalidate(token);
            if (s.pstate != PLAY_STATE_IDLE) {
                svc_audio_play_stop();
            }
            s.pstate = PLAY_STATE_IDLE;
            s.cur_path[0] = '\0';
            transport_refresh();
        }
        list_scan_start();
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
    if (s.pstate != PLAY_STATE_IDLE) {
        /* Async: the audio task may sit in a slow SD read; a synchronous
         * stop would block the LVGL thread up to the ack timeout during
         * navigation. The token was zeroed above, so the late completion
         * event is dropped by audio_cb's liveness check. */
        svc_audio_play_stop_async();
        s.pstate = PLAY_STATE_IDLE;
    }
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

lv_obj_t *app_player_create(void)
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
    s.sd_mounted = svc_storage_mounted();
    for (int index = 0; index < MAX_FILES; ++index) {
        s.durations[index] = -1;
    }

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Player", &content);
    lv_obj_add_event_cb(root, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    s.now_label = lv_label_create(content);
    lv_label_set_text(s.now_label, "No file selected");
    lv_obj_set_width(s.now_label, 428);
    lv_label_set_long_mode(s.now_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s.now_label, 0, 0);
    lv_obj_set_style_text_font(s.now_label, ui_font_body(), 0);

    s.progress_bar = lv_bar_create(content);
    lv_obj_set_size(s.progress_bar, 428, 14);
    lv_obj_set_pos(s.progress_bar, 0, 30);
    lv_bar_set_range(s.progress_bar, 0, 1);
    lv_bar_set_value(s.progress_bar, 0, LV_ANIM_OFF);

    s.time_label = lv_label_create(content);
    lv_obj_set_width(s.time_label, 428);
    lv_obj_set_pos(s.time_label, 0, 48);
    lv_obj_set_style_text_align(s.time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s.time_label, ui_font_body(), 0);
    lv_label_set_text(s.time_label, "00:00 / 00:00");

    s.play_btn = lv_button_create(content);
    lv_obj_set_size(s.play_btn, 96, UI_TOUCH_MIN);
    lv_obj_set_pos(s.play_btn, 10, 76);
    lv_obj_set_style_radius(s.play_btn, 12, 0);
    lv_obj_set_style_bg_color(s.play_btn, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(s.play_btn, 0, 0);
    lv_obj_add_event_cb(s.play_btn, play_button_cb, LV_EVENT_CLICKED, NULL);
    s.play_icon = lv_label_create(s.play_btn);
    lv_label_set_text(s.play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(s.play_icon);

    s.stop_btn = lv_button_create(content);
    lv_obj_set_size(s.stop_btn, 96, UI_TOUCH_MIN);
    lv_obj_set_pos(s.stop_btn, 118, 76);
    lv_obj_set_style_radius(s.stop_btn, 12, 0);
    lv_obj_set_style_bg_color(s.stop_btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_border_width(s.stop_btn, 0, 0);
    lv_obj_add_event_cb(s.stop_btn, stop_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_icon = lv_label_create(s.stop_btn);
    lv_label_set_text(stop_icon, LV_SYMBOL_STOP);
    lv_obj_center(stop_icon);

    lv_obj_t *vol_icon = lv_label_create(content);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_pos(vol_icon, 4, 150);
    s.vol_slider = lv_slider_create(content);
    lv_slider_set_range(s.vol_slider, 0, 100);
    lv_obj_set_size(s.vol_slider, 320, UI_TOUCH_MIN);
    lv_obj_set_pos(s.vol_slider, 44, 132);
    const int initial_volume = demo_settings()->volume;
    lv_slider_set_value(s.vol_slider, initial_volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(s.vol_slider, volume_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s.vol_slider, volume_changed_cb, LV_EVENT_RELEASED,
                        NULL);
    s.vol_label = lv_label_create(content);
    lv_label_set_text_fmt(s.vol_label, "%d", initial_volume);
    lv_obj_set_width(s.vol_label, 56);
    lv_obj_set_pos(s.vol_label, 372, 148);
    lv_obj_set_style_text_font(s.vol_label, ui_font_body(), 0);

    s.list = lv_obj_create(content);
    lv_obj_set_size(s.list, 428, 180);
    lv_obj_set_pos(s.list, 0, 196);
    lv_obj_set_style_bg_opa(s.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s.list, 0, 0);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_scrollbar_mode(s.list, LV_SCROLLBAR_MODE_AUTO);

    transport_refresh();
    list_scan_start();
    s.timer = lv_timer_create(tick_cb, 500, NULL);
    return root;
}
