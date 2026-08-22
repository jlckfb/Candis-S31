/*
 * Candis-S31 watch demo - file manager app.
 *
 * Directory browser over the TF card mount point: folders can be entered,
 * ".." returns to the parent, a short file tap shows details, and a long
 * press exposes deletion behind a confirm box. The bottom capacity bar mirrors
 * svc_storage_get_info(). Mount/unmount transitions are picked up by a
 * 500 ms poll of svc_storage_mounted() (the storage service has a single
 * subscriber - demo_main - so the app cannot register its own callback).
 * Directory and capacity I/O runs on a low-priority task under one storage
 * lease; the LVGL thread only applies immutable scan results. unlink() stays
 * synchronous because it is a single bounded path operation and holds a lease.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define FILES_MAX_ENTRIES 256
#define FILES_REL_PATH_MAX 192
#define FILES_NAME_MAX     256
#define FILES_POLL_MS      500
#define FILES_SCAN_STACK   4096
#define FILES_SCAN_PRIO    2
#define FILES_POST_RETRIES 20
#define FILES_POST_WAIT_MS 25

typedef struct {
    char name[FILES_NAME_MAX];
    uint32_t size;
    bool is_dir;
} file_entry_t;

typedef enum {
    FILES_SCAN_OK = 0,
    FILES_SCAN_NO_CARD,
    FILES_SCAN_OPEN_FAILED,
    FILES_SCAN_ALLOC_FAILED,
} files_scan_status_t;

typedef struct {
    uint32_t session_id;
    uint32_t scan_gen;
    bool mounted;
    char mount[24];
    char requested_rel[FILES_REL_PATH_MAX];
    char effective_rel[FILES_REL_PATH_MAX];
    files_scan_status_t status;
    file_entry_t *entries; /* PSRAM; task owns until a valid UI apply transfers it */
    int entry_count;
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool capacity_valid;
} files_scan_result_t;

static struct {
    bool active;
    bool was_mounted;
    lv_obj_t *path_lbl;
    lv_obj_t *list;
    lv_obj_t *cap_lbl;
    lv_obj_t *cap_bar;
    lv_obj_t *empty_lbl;
    lv_timer_t *timer;
    file_entry_t *entries;   /* PSRAM; owned by page between valid applies */
    int entry_count;
    bool has_parent;
    bool scan_running;
    bool scan_again;
    uint32_t session_id;
    uint32_t scan_gen;
    char rel_path[FILES_REL_PATH_MAX]; /* relative to mount root, no slash */
    char pending_delete[FILES_NAME_MAX];
} s_files;

static atomic_uint_fast32_t s_session_counter;
static atomic_uint_fast32_t s_live_session;
static atomic_uint_fast32_t s_live_scan_gen;
static atomic_uint_fast32_t s_failed_session;
static atomic_uint_fast32_t s_failed_scan_gen;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void format_size(uint64_t bytes, char *out, size_t out_size)
{
    if (bytes >= UINT64_C(1073741824)) {
        snprintf(out, out_size, "%.1f GB",
                 (double)bytes / 1073741824.0);
    } else if (bytes >= UINT64_C(1048576)) {
        snprintf(out, out_size, "%.1f MB", (double)bytes / 1048576.0);
    } else if (bytes >= UINT64_C(1024)) {
        snprintf(out, out_size, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%" PRIu64 " B", bytes);
    }
}

static int entry_compare(const void *a, const void *b)
{
    const file_entry_t *ea = a;
    const file_entry_t *eb = b;
    if (ea->is_dir != eb->is_dir) {
        return ea->is_dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static bool files_is_wav(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension != NULL && strcasecmp(extension, ".wav") == 0;
}

/* ------------------------------------------------------------------ */
/* Directory loading                                                   */
/* ------------------------------------------------------------------ */

static void files_wire_list_events(void);
static void files_reload(void);

static void files_update_path_label(void)
{
    if (s_files.rel_path[0] == '\0') {
        lv_label_set_text_fmt(s_files.path_lbl, "%s/", svc_storage_mount_point());
    } else {
        lv_label_set_text_fmt(s_files.path_lbl, "%s/%s",
                              svc_storage_mount_point(), s_files.rel_path);
    }
}

static void files_retry_cb(lv_event_t *event)
{
    (void)event;
    files_reload();
}

static void files_show_retry(const char *text)
{
    lv_obj_clean(s_files.list);
    lv_obj_t *retry = lv_list_add_button(s_files.list, LV_SYMBOL_REFRESH, text);
    lv_obj_set_height(retry, 64);
    lv_obj_add_event_cb(retry, files_retry_cb, LV_EVENT_CLICKED, NULL);
}

static void files_apply_entries(void)
{
    lv_obj_clean(s_files.list);
    s_files.has_parent = s_files.rel_path[0] != '\0';
    if (s_files.has_parent) {
        lv_list_add_button(s_files.list, LV_SYMBOL_UP, "..");
    }

    for (int i = 0; i < s_files.entry_count; ++i) {
        const file_entry_t *item = &s_files.entries[i];
        char text[FILES_NAME_MAX + 24];
        if (item->is_dir) {
            snprintf(text, sizeof(text), "%s", item->name);
        } else {
            char size_text[24];
            format_size(item->size, size_text, sizeof(size_text));
            snprintf(text, sizeof(text), "%.248s  (%s)", item->name, size_text);
        }
        lv_list_add_button(s_files.list,
                           item->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                           text);
    }

    files_wire_list_events();
    files_update_path_label();
}

static void files_scan_start(void);

/* LVGL thread. Stale results never touch page widgets or page-owned entries. */
static void files_scan_apply(void *arg)
{
    files_scan_result_t *result = arg;
    if (!s_files.active || result->session_id != s_files.session_id) {
        heap_caps_free(result->entries);
        free(result);
        return;
    }

    s_files.scan_running = false;
    if (result->scan_gen == s_files.scan_gen) {
        heap_caps_free(s_files.entries);
        s_files.entries = result->entries;
        result->entries = NULL;
        s_files.entry_count = result->entry_count;
        snprintf(s_files.rel_path, sizeof(s_files.rel_path), "%s",
                 result->effective_rel);

        if (result->status == FILES_SCAN_OK) {
            files_apply_entries();
        } else if (result->status == FILES_SCAN_NO_CARD) {
            files_show_retry("TF card unavailable, retry");
        } else if (result->status == FILES_SCAN_ALLOC_FAILED) {
            files_show_retry("Out of memory, retry");
        } else {
            files_show_retry("Directory read failed, retry");
        }

        if (result->capacity_valid && result->total_bytes > 0) {
            const uint64_t used = result->total_bytes - result->free_bytes;
            char total_text[24];
            char free_text[24];
            format_size(result->total_bytes, total_text, sizeof(total_text));
            format_size(result->free_bytes, free_text, sizeof(free_text));
            lv_label_set_text_fmt(s_files.cap_lbl, "Total %s / free %s",
                                  total_text, free_text);
            lv_bar_set_value(s_files.cap_bar,
                             (int32_t)(used * 100 / result->total_bytes),
                             LV_ANIM_OFF);
        } else {
            lv_label_set_text(s_files.cap_lbl, "Capacity unknown");
            lv_bar_set_value(s_files.cap_bar, 0, LV_ANIM_OFF);
        }
    }
    heap_caps_free(result->entries);
    free(result);

    if (s_files.scan_again) {
        s_files.scan_again = false;
        files_scan_start();
    }
}

static bool files_scan_cancelled(const files_scan_result_t *result)
{
    return atomic_load_explicit(&s_live_session, memory_order_acquire) !=
               result->session_id ||
           atomic_load_explicit(&s_live_scan_gen, memory_order_acquire) !=
               result->scan_gen;
}

static bool files_session_cancelled(const files_scan_result_t *result)
{
    return atomic_load_explicit(&s_live_session, memory_order_acquire) !=
           result->session_id;
}

/* The task owns result and its PSRAM entries until the fixed UI queue accepts
 * the pointer. Queue pressure is retried for at most 500 ms, never forever. */
static bool files_scan_post(files_scan_result_t *result)
{
    for (int attempt = 0; attempt < FILES_POST_RETRIES; ++attempt) {
        if (files_session_cancelled(result)) {
            heap_caps_free(result->entries);
            free(result);
            return false;
        }
        if (ui_async(files_scan_apply, result)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(FILES_POST_WAIT_MS));
    }
    if (!files_session_cancelled(result)) {
        atomic_store_explicit(&s_failed_scan_gen, result->scan_gen,
                              memory_order_relaxed);
        atomic_store_explicit(&s_failed_session, result->session_id,
                              memory_order_release);
    }
    heap_caps_free(result->entries);
    free(result);
    return false;
}

/* Worker task: all blocking directory/stat/capacity I/O is below this point
 * and covered by one storage lease. It only consumes request snapshots. */
static void files_scan_task(void *arg)
{
    files_scan_result_t *result = arg;
    result->status = FILES_SCAN_NO_CARD;
    if (!result->mounted || files_scan_cancelled(result)) {
        (void)files_scan_post(result);
        vTaskDelete(NULL);
        return;
    }

    result->entries = heap_caps_calloc(FILES_MAX_ENTRIES, sizeof(file_entry_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (result->entries == NULL) {
        result->status = FILES_SCAN_ALLOC_FAILED;
        (void)files_scan_post(result);
        vTaskDelete(NULL);
        return;
    }

    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        (void)files_scan_post(result);
        vTaskDelete(NULL);
        return;
    }

    char full_path[FILES_REL_PATH_MAX + 32];
    if (result->requested_rel[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s", result->mount);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", result->mount,
                 result->requested_rel);
    }
    DIR *dir = opendir(full_path);
    if (dir == NULL && result->requested_rel[0] != '\0') {
        result->effective_rel[0] = '\0';
        snprintf(full_path, sizeof(full_path), "%s", result->mount);
        dir = opendir(full_path);
    } else {
        snprintf(result->effective_rel, sizeof(result->effective_rel), "%s",
                 result->requested_rel);
    }

    if (dir != NULL) {
        while (result->entry_count < FILES_MAX_ENTRIES &&
               !files_scan_cancelled(result)) {
            const struct dirent *entry = readdir(dir);
            if (entry == NULL) {
                break;
            }
            if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            file_entry_t *item = &result->entries[result->entry_count];
            snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
            char item_path[FILES_REL_PATH_MAX + FILES_NAME_MAX + 32];
            snprintf(item_path, sizeof(item_path), "%.215s/%.255s", full_path,
                     item->name);
            struct stat st;
            if (stat(item_path, &st) == 0) {
                item->is_dir = S_ISDIR(st.st_mode);
                item->size = (uint32_t)(st.st_size > UINT32_MAX ?
                                        UINT32_MAX : st.st_size);
            }
            ++result->entry_count;
        }
        closedir(dir);
        qsort(result->entries, result->entry_count, sizeof(file_entry_t),
              entry_compare);
        result->status = FILES_SCAN_OK;
    } else {
        result->status = FILES_SCAN_OPEN_FAILED;
    }

    if (!files_scan_cancelled(result)) {
        result->capacity_valid =
            svc_storage_get_info(&result->total_bytes, &result->free_bytes) ==
            ESP_OK;
    }
    svc_storage_lease_release(&lease);
    (void)files_scan_post(result);
    vTaskDelete(NULL);
}

/* LVGL thread: snapshot request, coalesce repeated refreshes, no filesystem I/O. */
static void files_scan_start(void)
{
    if (!s_files.active) {
        return;
    }
    s_files.scan_gen++;
    atomic_store_explicit(&s_live_scan_gen, s_files.scan_gen,
                          memory_order_release);
    if (s_files.scan_running) {
        s_files.scan_again = true;
        return;
    }

    files_scan_result_t *result = calloc(1, sizeof(*result));
    if (result == NULL) {
        files_show_retry("Out of memory, retry");
        return;
    }
    result->session_id = s_files.session_id;
    result->scan_gen = s_files.scan_gen;
    result->mounted = s_files.was_mounted;
    snprintf(result->mount, sizeof(result->mount), "%s",
             svc_storage_mount_point());
    snprintf(result->requested_rel, sizeof(result->requested_rel), "%s",
             s_files.rel_path);

    lv_obj_clean(s_files.list);
    lv_obj_t *scanning = lv_label_create(s_files.list);
    lv_label_set_text(scanning, "Scanning...");
    s_files.scan_running = true;
    if (xTaskCreate(files_scan_task, "files_scan", FILES_SCAN_STACK, result,
                    FILES_SCAN_PRIO, NULL) != pdPASS) {
        s_files.scan_running = false;
        free(result);
        files_show_retry("Scan start failed, retry");
    }
}

static void files_reload(void)
{
    files_scan_start();
}

/* ------------------------------------------------------------------ */
/* Navigation and deletion (LVGL thread)                               */
/* ------------------------------------------------------------------ */

static void files_descend(const char *name)
{
    const size_t cur_len = strlen(s_files.rel_path);
    const size_t name_len = strlen(name);
    if (cur_len + 1 + name_len + 1 > sizeof(s_files.rel_path)) {
        ui_toast("Path too deep");
        return;
    }
    if (cur_len > 0) {
        s_files.rel_path[cur_len] = '/';
        memcpy(&s_files.rel_path[cur_len + 1], name, name_len + 1);
    } else {
        memcpy(s_files.rel_path, name, name_len + 1);
    }
    files_reload();
}

static void files_go_up(void)
{
    char *slash = strrchr(s_files.rel_path, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        s_files.rel_path[0] = '\0';
    }
    files_reload();
}

static void delete_confirm_cb(bool ok, void *user)
{
    (void)user;
    if (!ok || !s_files.active) {
        return;
    }
    char full_path[FILES_REL_PATH_MAX + FILES_NAME_MAX + 32];
    if (s_files.rel_path[0] == '\0') {
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 svc_storage_mount_point(), s_files.pending_delete);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s/%s",
                 svc_storage_mount_point(), s_files.rel_path,
                 s_files.pending_delete);
    }
    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) != ESP_OK) {
        ui_toast("TF card removing");
        return;
    }
    const int unlink_result = unlink(full_path);
    svc_storage_lease_release(&lease);
    if (unlink_result == 0) {
        ui_toast("Deleted");
    } else {
        ui_toast("Delete failed");
    }
    files_reload();
}

static int files_entry_index_from_event(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_files.has_parent && index == 0) {
        return -1;
    }
    return index - (s_files.has_parent ? 1 : 0);
}

static void list_short_click_cb(lv_event_t *event)
{
    if (!s_files.active) {
        return;
    }
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_files.has_parent && index == 0) {
        files_go_up();
        return;
    }
    const int entry_index = files_entry_index_from_event(event);
    if (entry_index < 0 || entry_index >= s_files.entry_count) {
        return;
    }
    const file_entry_t *item = &s_files.entries[entry_index];
    if (item->is_dir) {
        files_descend(item->name);
        return;
    }

    char size_text[24];
    format_size(item->size, size_text, sizeof(size_text));
    char text[FILES_NAME_MAX + 160];
    if (files_is_wav(item->name)) {
        snprintf(text, sizeof(text),
                 "%.255s\nSize: %s\nWAV recording, play in Player\n\nDelete: long-press file",
                 item->name, size_text);
    } else {
        snprintf(text, sizeof(text), "%.255s\nSize: %s\n\nDelete: long-press file",
                 item->name, size_text);
    }
    ui_msgbox("File details", text, NULL, NULL);
}

static void list_long_press_cb(lv_event_t *event)
{
    if (!s_files.active) {
        return;
    }
    const int entry_index = files_entry_index_from_event(event);
    if (entry_index < 0 || entry_index >= s_files.entry_count) {
        return;
    }
    const file_entry_t *item = &s_files.entries[entry_index];
    if (item->is_dir) {
        ui_toast("Folders cannot be deleted here");
        return;
    }

    strncpy(s_files.pending_delete, item->name, FILES_NAME_MAX - 1);
    s_files.pending_delete[FILES_NAME_MAX - 1] = '\0';
    char text[FILES_NAME_MAX + 40];
    snprintf(text, sizeof(text), "Permanently delete file?\n%s", item->name);
    ui_msgbox("Hold to delete", text, delete_confirm_cb, NULL);
}

/* Rebuild the click wiring after every list rebuild. Called right after
 * files_reload(); button order matches (optional "..") + sorted entries. */
static void files_wire_list_events(void)
{
    const int total = s_files.entry_count + (s_files.has_parent ? 1 : 0);
    for (int i = 0; i < total; ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_files.list, i);
        if (btn != NULL) {
            lv_obj_set_height(btn, 64);
            lv_obj_add_event_cb(btn, list_short_click_cb, LV_EVENT_SHORT_CLICKED,
                                (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, list_long_press_cb, LV_EVENT_LONG_PRESSED,
                                (void *)(intptr_t)i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mount-state poll timer                                              */
/* ------------------------------------------------------------------ */

static void files_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_files.active) {
        return;
    }

    if (s_files.scan_running &&
        atomic_load_explicit(&s_failed_session, memory_order_acquire) ==
            s_files.session_id) {
        const uint32_t failed_gen = (uint32_t)atomic_load_explicit(
            &s_failed_scan_gen, memory_order_acquire);
        atomic_store_explicit(&s_failed_session, 0, memory_order_release);
        s_files.scan_running = false;
        if (s_files.scan_again) {
            s_files.scan_again = false;
            files_scan_start();
        } else if (failed_gen == s_files.scan_gen) {
            files_show_retry("Scan failed, retry");
        }
    }

    const bool mounted = svc_storage_mounted();
    if (mounted != s_files.was_mounted) {
        s_files.was_mounted = mounted;
        if (mounted) {
            lv_obj_add_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.path_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.cap_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.cap_bar, LV_OBJ_FLAG_HIDDEN);
            files_reload(); /* wires its own click events */
        } else {
            s_files.rel_path[0] = '\0';
            s_files.scan_gen++;
            atomic_store_explicit(&s_live_scan_gen, s_files.scan_gen,
                                  memory_order_release);
            lv_obj_clean(s_files.list);
            lv_obj_add_flag(s_files.list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.path_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.cap_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.cap_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void files_delete_cb(lv_event_t *event)
{
    (void)event;
    s_files.active = false;
    atomic_store_explicit(&s_live_session, 0, memory_order_release);
    if (s_files.timer != NULL) {
        lv_timer_delete(s_files.timer);
        s_files.timer = NULL;
    }
    if (s_files.entries != NULL) {
        heap_caps_free(s_files.entries);
        s_files.entries = NULL;
    }
}

lv_obj_t *app_files_create(void)
{
    memset(&s_files, 0, sizeof(s_files));
    s_files.active = true;
    s_files.session_id = (uint32_t)atomic_fetch_add_explicit(
                             &s_session_counter, 1, memory_order_relaxed) + 1;
    atomic_store_explicit(&s_live_session, s_files.session_id,
                          memory_order_release);
    atomic_store_explicit(&s_live_scan_gen, 0, memory_order_release);
    atomic_store_explicit(&s_failed_session, 0, memory_order_release);

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("Files", &content);
    lv_obj_add_event_cb(root, files_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    s_files.path_lbl = lv_label_create(content);
    lv_obj_set_width(s_files.path_lbl, LV_PCT(100));
    lv_obj_set_pos(s_files.path_lbl, 0, 0);
    lv_label_set_long_mode(s_files.path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_files.path_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    lv_obj_t *gesture_hint = lv_label_create(content);
    lv_label_set_text(gesture_hint, "Tap details, hold file to delete");
    lv_obj_set_pos(gesture_hint, 0, 28);
    lv_obj_set_style_text_color(gesture_hint,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_files.list = lv_list_create(content);
    lv_obj_set_size(s_files.list, LV_PCT(100), 228);
    lv_obj_set_pos(s_files.list, 0, 56);
    lv_obj_set_style_bg_color(s_files.list, lv_color_hex(UI_COLOR_SURFACE), 0);

    s_files.cap_lbl = lv_label_create(content);
    lv_obj_set_pos(s_files.cap_lbl, 0, 292);
    lv_obj_set_style_text_color(s_files.cap_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_files.cap_bar = lv_bar_create(content);
    lv_obj_set_size(s_files.cap_bar, LV_PCT(100), 12);
    lv_obj_set_pos(s_files.cap_bar, 0, 320);
    lv_bar_set_range(s_files.cap_bar, 0, 100);
    lv_obj_set_style_bg_color(s_files.cap_bar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_files.cap_bar, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_INDICATOR);

    s_files.empty_lbl = lv_label_create(content);
    lv_label_set_text(s_files.empty_lbl, "Insert TF card");
    lv_obj_center(s_files.empty_lbl);
    lv_obj_add_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);

    if (svc_storage_mounted()) {
        s_files.was_mounted = true;
        files_reload(); /* wires its own click events */
    } else {
        lv_obj_add_flag(s_files.list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_files.path_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_files.cap_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_files.cap_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    s_files.timer = lv_timer_create(files_timer_cb, FILES_POLL_MS, NULL);
    return root;
}
