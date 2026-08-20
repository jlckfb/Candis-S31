/*
 * Candis-S31 watch demo - file manager app.
 *
 * Directory browser over the TF card mount point: folders can be entered,
 * ".." returns to the parent, files show their size and are deletable
 * behind a confirm box. The bottom capacity bar mirrors
 * svc_storage_get_info(). Mount/unmount transitions are picked up by a
 * 500 ms poll of svc_storage_mounted() (the storage service has a single
 * subscriber - demo_main - so the app cannot register its own callback).
 *
 * Note: directory I/O (opendir/stat/unlink) runs on the LVGL thread; the
 * storage contract exposes no asynchronous listing API, so this is the
 * pragmatic path. Entries are capped and the entry table lives in PSRAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#include "demo_apps.h"
#include "services/svc_storage.h"
#include "ui/ui_manager.h"

#define FILES_MAX_ENTRIES 256
#define FILES_REL_PATH_MAX 192
#define FILES_NAME_MAX     256
#define FILES_POLL_MS      500

typedef struct {
    char name[FILES_NAME_MAX];
    uint32_t size;
    bool is_dir;
} file_entry_t;

static struct {
    bool active;
    bool was_mounted;
    lv_obj_t *path_lbl;
    lv_obj_t *list;
    lv_obj_t *cap_lbl;
    lv_obj_t *cap_bar;
    lv_obj_t *empty_lbl;
    lv_timer_t *timer;
    file_entry_t *entries;   /* PSRAM, FILES_MAX_ENTRIES */
    int entry_count;
    bool has_parent;
    char rel_path[FILES_REL_PATH_MAX]; /* relative to mount root, no slash */
    char pending_delete[FILES_NAME_MAX];
} s_files;

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

/* Build the absolute path of the current directory into out. */
static void files_current_full_path(char *out, size_t out_size)
{
    if (s_files.rel_path[0] == '\0') {
        snprintf(out, out_size, "%s", svc_storage_mount_point());
    } else {
        snprintf(out, out_size, "%s/%s", svc_storage_mount_point(),
                 s_files.rel_path);
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

/* ------------------------------------------------------------------ */
/* Directory loading                                                   */
/* ------------------------------------------------------------------ */

static void files_wire_list_events(void);

static void files_update_path_label(void)
{
    if (s_files.rel_path[0] == '\0') {
        lv_label_set_text_fmt(s_files.path_lbl, "%s/", svc_storage_mount_point());
    } else {
        lv_label_set_text_fmt(s_files.path_lbl, "%s/%s",
                              svc_storage_mount_point(), s_files.rel_path);
    }
}

static void files_reload(void)
{
    if (!s_files.active || s_files.entries == NULL) {
        return;
    }
    char full_path[FILES_REL_PATH_MAX + 32];
    files_current_full_path(full_path, sizeof(full_path));

    DIR *dir = opendir(full_path);
    if (dir == NULL && s_files.rel_path[0] != '\0') {
        /* The remembered path vanished (card re-inserted with different
         * contents): fall back to the root once. */
        s_files.rel_path[0] = '\0';
        files_current_full_path(full_path, sizeof(full_path));
        dir = opendir(full_path);
    }

    lv_obj_clean(s_files.list);
    s_files.entry_count = 0;
    s_files.has_parent = s_files.rel_path[0] != '\0';

    if (s_files.has_parent) {
        /* Click wiring is attached by files_wire_list_events() after the
         * whole list is rebuilt, so no per-button callback here. */
        lv_list_add_button(s_files.list, LV_SYMBOL_UP, "..");
    }

    if (dir == NULL) {
        lv_obj_t *note = lv_label_create(s_files.list);
        lv_label_set_text(note, "目录读取失败");
        files_wire_list_events();
        files_update_path_label();
        return;
    }

    while (s_files.entry_count < FILES_MAX_ENTRIES) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        file_entry_t *item = &s_files.entries[s_files.entry_count];
        strncpy(item->name, entry->d_name, FILES_NAME_MAX - 1);
        item->name[FILES_NAME_MAX - 1] = '\0';
        item->size = 0;
        item->is_dir = false;

        char item_path[FILES_REL_PATH_MAX + FILES_NAME_MAX + 8];
        snprintf(item_path, sizeof(item_path), "%.192s/%.255s", full_path, item->name);
        struct stat st;
        if (stat(item_path, &st) == 0) {
            item->is_dir = S_ISDIR(st.st_mode);
            item->size = (uint32_t)(st.st_size > UINT32_MAX ?
                                    UINT32_MAX : st.st_size);
        }
        ++s_files.entry_count;
    }
    closedir(dir);

    qsort(s_files.entries, s_files.entry_count, sizeof(file_entry_t),
          entry_compare);

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

/* ------------------------------------------------------------------ */
/* Navigation and deletion (LVGL thread)                               */
/* ------------------------------------------------------------------ */

static void files_descend(const char *name)
{
    const size_t cur_len = strlen(s_files.rel_path);
    const size_t name_len = strlen(name);
    if (cur_len + 1 + name_len + 1 > sizeof(s_files.rel_path)) {
        ui_toast("路径过深");
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
    if (unlink(full_path) == 0) {
        ui_toast("已删除");
    } else {
        ui_toast("删除失败");
    }
    files_reload();
}

static void list_click_cb(lv_event_t *event)
{
    if (!s_files.active) {
        return;
    }
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_files.has_parent && index == 0) {
        files_go_up();
        return;
    }
    const int entry_index = index - (s_files.has_parent ? 1 : 0);
    if (entry_index < 0 || entry_index >= s_files.entry_count) {
        return;
    }
    const file_entry_t *item = &s_files.entries[entry_index];
    if (item->is_dir) {
        files_descend(item->name);
        return;
    }
    /* File tap: ask for deletion confirmation. */
    strncpy(s_files.pending_delete, item->name, FILES_NAME_MAX - 1);
    s_files.pending_delete[FILES_NAME_MAX - 1] = '\0';
    char text[FILES_NAME_MAX + 16];
    snprintf(text, sizeof(text), "删除文件?\n%s", item->name);
    ui_msgbox("删除", text, delete_confirm_cb, NULL);
}

/* Rebuild the click wiring after every list rebuild. Called right after
 * files_reload(); button order matches (optional "..") + sorted entries. */
static void files_wire_list_events(void)
{
    const int total = s_files.entry_count + (s_files.has_parent ? 1 : 0);
    for (int i = 0; i < total; ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_files.list, i);
        if (btn != NULL) {
            lv_obj_add_event_cb(btn, list_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Capacity bar                                                        */
/* ------------------------------------------------------------------ */

static void files_update_capacity(void)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (svc_storage_get_info(&total, &free_bytes) != ESP_OK || total == 0) {
        lv_label_set_text(s_files.cap_lbl, "容量未知");
        lv_bar_set_value(s_files.cap_bar, 0, LV_ANIM_OFF);
        return;
    }
    const uint64_t used = total - free_bytes;
    char total_text[24];
    char free_text[24];
    format_size(total, total_text, sizeof(total_text));
    format_size(free_bytes, free_text, sizeof(free_text));
    lv_label_set_text_fmt(s_files.cap_lbl, "总量 %s / 可用 %s",
                          total_text, free_text);
    lv_bar_set_value(s_files.cap_bar, (int32_t)(used * 100 / total),
                     LV_ANIM_OFF);
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
            lv_obj_clean(s_files.list);
            lv_obj_add_flag(s_files.list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.path_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.cap_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_files.cap_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (mounted) {
        files_update_capacity();
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void files_delete_cb(lv_event_t *event)
{
    (void)event;
    s_files.active = false;
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

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("文件", &content);
    lv_obj_add_event_cb(root, files_delete_cb, LV_EVENT_DELETE, NULL);

    s_files.path_lbl = lv_label_create(content);
    lv_obj_set_width(s_files.path_lbl, 448);
    lv_obj_set_pos(s_files.path_lbl, 2, 0);
    lv_label_set_long_mode(s_files.path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_files.path_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_files.list = lv_list_create(content);
    lv_obj_set_size(s_files.list, 448, 300);
    lv_obj_set_pos(s_files.list, 0, 24);
    lv_obj_set_style_bg_color(s_files.list, lv_color_hex(UI_COLOR_SURFACE), 0);

    s_files.cap_lbl = lv_label_create(content);
    lv_obj_set_pos(s_files.cap_lbl, 2, 332);
    lv_obj_set_style_text_color(s_files.cap_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);

    s_files.cap_bar = lv_bar_create(content);
    lv_obj_set_size(s_files.cap_bar, 448, 12);
    lv_obj_set_pos(s_files.cap_bar, 2, 358);
    lv_bar_set_range(s_files.cap_bar, 0, 100);
    lv_obj_set_style_bg_color(s_files.cap_bar, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(s_files.cap_bar, lv_color_hex(UI_COLOR_ACCENT),
                              LV_PART_INDICATOR);

    s_files.empty_lbl = lv_label_create(content);
    lv_label_set_text(s_files.empty_lbl, "请插入 TF 卡");
    lv_obj_center(s_files.empty_lbl);
    lv_obj_add_flag(s_files.empty_lbl, LV_OBJ_FLAG_HIDDEN);

    s_files.entries = heap_caps_malloc(FILES_MAX_ENTRIES * sizeof(file_entry_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (svc_storage_mounted()) {
        s_files.was_mounted = true;
        files_reload(); /* wires its own click events */
        files_update_capacity();
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
