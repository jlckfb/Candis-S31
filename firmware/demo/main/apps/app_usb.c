/*
 * Candis-S31 watch demo - USB OTG app.
 *
 * While the page is open a 200 ms lv_timer polls bsp_type_c_get_status()
 * (clear_interrupts=false) and renders the FUSB303B role in Chinese.
 * Role Source offers "Mount USB drive": a worker task runs the factory-verified
 * sequence (bsp_usb_host_start -> msc_host_install -> wait for
 * MSC_DEVICE_CONNECTED -> install_device -> vfs_register on /usb0), then
 * the file list is shown. While the host stack runs, Type-C polling is
 * suspended (the host owns the FUSB303B, review item 7) and removal is
 * detected through the MSC event callback. Unmount/exit/removal run the
 * full teardown (vfs unregister -> device uninstall -> driver uninstall ->
 * bsp_usb_host_stop) in a worker task, never on the LVGL thread. Role Sink
 * starts a real USB Device session: the single OTG peripheral cannot run
 * both stacks at once, so the CDC path waits for the host side to be fully
 * down, then installs TinyUSB CDC (ping/hello/status command set, same
 * VID/PID as the verified firmware/usb_cdc_device image) and tears it down
 * again on detach, role change or page exit - restoring DRP so the port can
 * become a Source again. Device-session state reaches the UI through the
 * same landing mailbox as the host events.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "services/svc_power.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

#define USB_POLL_MS          200
#define USB_WORKER_STACK     6144
#define USB_WORKER_PRIO      4
#define USB_SUPERVISOR_STACK 2048
#define USB_ENUM_TIMEOUT_MS  10000
#define USB_MOUNT_PATH       "/usb0"
#define USB_LIST_MAX_ROWS    200
#define USB_DELIVERY_RETRIES 3
#define USB_DELIVERY_DELAY_MS 20
#define USB_TEARDOWN_RETRIES  5
#define USB_SUPERVISOR_MS     100

/* CDC device session. Task/buffer sizes stay at the esp_tinyusb defaults
 * (4 KB task stack, 512 B CDC buffers - a few KB of internal RAM total):
 * the standalone firmware/usb_cdc_device image validated exactly this
 * configuration on this board. */
#define USB_DEVICE_STACK      4096
#define USB_DEVICE_PRIO       4
#define USB_DEV_POLL_MS       100
#define USB_DEV_CMD_SIZE      128
#define USB_DEV_QUEUE_DEPTH   8

typedef enum {
    USB_PAGE_IDLE = 0,   /* detached or role unresolved */
    USB_PAGE_SOURCE,     /* attached as Source, not mounted */
    USB_PAGE_SINK,       /* attached as Sink (device mode) */
    USB_PAGE_MOUNTING,
    USB_PAGE_MOUNTED,
} usb_page_state_t;

static struct {
    bool active;
    usb_page_state_t state;
    lv_obj_t *info_lbl;
    lv_obj_t *sink_lbl;
    lv_obj_t *mount_btn;
    lv_obj_t *unmount_btn;
    lv_obj_t *list;
    lv_timer_t *poll_timer;
    uint32_t session;
    uint32_t last_event_seq;
    int last_status_icon; /* last value sent to ui_status_set_usb() */
} s_usb;

/* Host/worker bookkeeping shared with the MSC task and worker tasks. */
static atomic_bool s_host_running;      /* FUSB303B owned by USB host */
static atomic_bool s_msc_driver_installed;
static atomic_bool s_worker_busy;       /* mount or teardown task live */
static atomic_bool s_teardown_requested;
static atomic_bool s_teardown_failed;
static atomic_bool s_msc_connected;
static atomic_bool s_msc_disconnected;
static atomic_uchar s_msc_address;
static atomic_uint s_session_seq;
static atomic_uint s_live_session;
static atomic_uint s_event_seq;
static atomic_uint s_cleanup_session;
static atomic_uint s_teardown_attempts;
static msc_host_device_handle_t s_msc_device; /* worker-task owned */
static msc_host_vfs_handle_t s_msc_vfs;       /* worker-task owned */
static TaskHandle_t s_supervisor_task;
/* Page session that last consumed an automatic teardown-failure
 * recovery: each session retries at most once (no unbounded loop), but a
 * later session may try again - session ids are strictly increasing. */
static uint32_t s_teardown_retry_session;

/* CDC device session bookkeeping. s_dev_task_live is the single-writer
 * gate for the CDC session; the rest is read by the LVGL poll. */
static atomic_bool s_dev_task_live;
static atomic_bool s_dev_stop_requested;
static atomic_bool s_dev_running;      /* TinyUSB driver + CDC installed */
static atomic_bool s_dev_configured;   /* host finished enumeration */
static QueueHandle_t s_dev_queue;      /* command messages, device task only */
static volatile bool s_dev_dtr;
static volatile bool s_dev_banner_sent;

typedef struct {
    uint8_t data[USB_DEV_CMD_SIZE];
    size_t length;
} usb_dev_msg_t;

typedef enum {
    USB_LANDING_NONE = 0,
    USB_LANDING_MOUNTED,
    USB_LANDING_TEARDOWN_DONE,
    USB_LANDING_UNPLUGGED,
    USB_LANDING_DEV_STARTED,      /* TinyUSB CDC installed */
    USB_LANDING_DEV_CONFIGURED,   /* host enumerated the device */
    USB_LANDING_DEV_STOPPED,      /* CDC session torn down */
} usb_landing_event_t;

typedef enum {
    USB_LIST_OK = 0,
    USB_LIST_OPEN_FAILED,
    USB_LIST_ALLOC_FAILED,
} usb_list_status_t;

typedef struct {
    char name[256];
    uint64_t size;
    bool is_dir;
} usb_file_entry_t;

typedef struct {
    usb_list_status_t status;
    int count;
    usb_file_entry_t *entries; /* PSRAM; owned with this payload */
} usb_file_list_t;

typedef struct {
    uint32_t session;
    uint32_t seq;
    usb_landing_event_t event;
    usb_file_list_t *files; /* non-NULL only for MOUNTED */
} usb_landing_t;

static portMUX_TYPE s_landing_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_landing_t s_landing_mailbox;

static const char *TAG = "app_usb";

static void usb_apply_landing(void *arg);
static void usb_queue_teardown_session(uint32_t session);
static void usb_request_teardown_session(uint32_t session);

static void usb_file_list_free(usb_file_list_t *files)
{
    if (files == NULL) {
        return;
    }
    heap_caps_free(files->entries);
    free(files);
}

static bool usb_session_is_live(uint32_t session)
{
    return session != 0 &&
           atomic_load_explicit(&s_live_session, memory_order_acquire) ==
               session;
}

/* Takes ownership of landing->files whether the post is accepted or dropped. */
static void usb_mailbox_post(usb_landing_t *landing)
{
    if (!usb_session_is_live(landing->session)) {
        usb_file_list_free(landing->files);
        landing->files = NULL;
        return;
    }
    usb_file_list_t *replaced = NULL;
    bool accepted = false;
    portENTER_CRITICAL(&s_landing_lock);
    if (usb_session_is_live(landing->session) &&
            landing->seq >= s_landing_mailbox.seq) {
        replaced = s_landing_mailbox.files;
        s_landing_mailbox = *landing;
        landing->files = NULL;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_landing_lock);
    usb_file_list_free(replaced);
    if (!accepted) {
        usb_file_list_free(landing->files);
        landing->files = NULL;
    }
}

/* Transfers mailbox payload ownership to the caller. */
static usb_landing_t usb_mailbox_take(void)
{
    portENTER_CRITICAL(&s_landing_lock);
    const usb_landing_t landing = s_landing_mailbox;
    s_landing_mailbox = (usb_landing_t){ 0 };
    portEXIT_CRITICAL(&s_landing_lock);
    return landing;
}

/* Worker-task landing: retry a bounded number of times, then publish to the
 * fixed mailbox consumed by the page timer. */
static void usb_deliver(uint32_t session, usb_landing_event_t event,
                        bool allow_delay, usb_file_list_t *files)
{
    if (!usb_session_is_live(session)) {
        usb_file_list_free(files);
        return;
    }
    usb_landing_t *landing = malloc(sizeof(*landing));
    const uint32_t seq = atomic_fetch_add_explicit(
                             &s_event_seq, 1, memory_order_relaxed) + 1;
    if (landing != NULL) {
        *landing = (usb_landing_t){
            .session = session,
            .seq = seq,
            .event = event,
            .files = files,
        };
        files = NULL;
        const int attempts = allow_delay ? USB_DELIVERY_RETRIES : 1;
        for (int i = 0; i < attempts; ++i) {
            if (!usb_session_is_live(session)) {
                usb_file_list_free(landing->files);
                free(landing);
                return;
            }
            if (ui_async(usb_apply_landing, landing)) {
                return;
            }
            if (allow_delay && i + 1 < attempts) {
                vTaskDelay(pdMS_TO_TICKS(USB_DELIVERY_DELAY_MS));
            }
        }
    }
    const usb_landing_t fallback = {
        .session = session,
        .seq = seq,
        .event = event,
        .files = landing != NULL ? landing->files : files,
    };
    usb_landing_t mailbox_landing = fallback;
    if (landing != NULL) {
        landing->files = NULL;
    }
    usb_mailbox_post(&mailbox_landing);
    free(landing);
    ESP_LOGW(TAG, "UI landing queue busy, event=%d routed to mailbox",
             (int)event);
}

/* ------------------------------------------------------------------ */
/* UI helpers (LVGL thread only)                                       */
/* ------------------------------------------------------------------ */

static void usb_set_status_icon(int icon)
{
    if (icon != s_usb.last_status_icon) {
        s_usb.last_status_icon = icon;
        ui_status_set_usb(icon);
    }
}

/* Show/hide the page widgets for the current state. */
static void usb_visibility(bool show_info, bool show_sink, bool show_mount,
                           bool show_unmount, bool show_list)
{
    struct {
        lv_obj_t *obj;
        bool show;
    } items[] = {
        { s_usb.info_lbl, show_info },
        { s_usb.sink_lbl, show_sink },
        { s_usb.mount_btn, show_mount },
        { s_usb.unmount_btn, show_unmount },
        { s_usb.list, show_list },
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (items[i].obj == NULL) {
            continue;
        }
        if (items[i].show) {
            lv_obj_remove_flag(items[i].obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(items[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* LVGL thread: render the immutable worker snapshot, never touch VFS here. */
static void usb_render_file_list(const usb_file_list_t *files)
{
    lv_obj_clean(s_usb.list);
    if (files == NULL || files->status == USB_LIST_ALLOC_FAILED) {
        lv_obj_t *note = lv_label_create(s_usb.list);
        lv_label_set_text(note, "File list out of memory");
        return;
    }
    if (files->status == USB_LIST_OPEN_FAILED) {
        lv_obj_t *note = lv_label_create(s_usb.list);
        lv_label_set_text(note, "Directory read failed");
        return;
    }
    for (int row_index = 0; row_index < files->count; ++row_index) {
        const usb_file_entry_t *entry = &files->entries[row_index];
        char text[160];
        if (entry->is_dir) {
            snprintf(text, sizeof(text), "%.159s", entry->name);
        } else if (entry->size >= UINT64_C(1048576)) {
            snprintf(text, sizeof(text), "%.130s  (%.1f MB)", entry->name,
                     (double)entry->size / 1048576.0);
        } else if (entry->size >= UINT64_C(1024)) {
            snprintf(text, sizeof(text), "%.130s  (%.1f KB)", entry->name,
                     (double)entry->size / 1024.0);
        } else {
            snprintf(text, sizeof(text), "%.130s  (%" PRIu64 " B)",
                     entry->name, entry->size);
        }
        lv_obj_t *row = lv_list_add_button(
            s_usb.list,
            entry->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, text);
        lv_obj_set_height(row, 64);
        lv_obj_set_style_text_font(row, ui_font_body(), 0);
    }
    if (files->count == 0) {
        lv_obj_t *note = lv_label_create(s_usb.list);
        lv_label_set_text(note, "(empty drive)");
    }
}

/* ------------------------------------------------------------------ */
/* MSC worker tasks (never LVGL thread)                                */
/* ------------------------------------------------------------------ */

static void usb_msc_event_cb(const msc_host_event_t *event, void *arg)
{
    const uint32_t session = (uint32_t)(uintptr_t)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        atomic_store_explicit(&s_msc_address, event->device.address,
                              memory_order_release);
        atomic_store_explicit(&s_msc_connected, true, memory_order_release);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        atomic_store_explicit(&s_msc_connected, false, memory_order_release);
        atomic_store_explicit(&s_msc_disconnected, true,
                              memory_order_release);
        /* Make teardown independent of UI event ordering. A disconnect can
         * race the final mount landing; the poll/worker must still close the
         * VFS and host stack even if that landing is delivered last. */
        /* Only publish the request from the MSC callback. The permanent
         * supervisor starts cleanup after this driver callback has returned. */
        usb_queue_teardown_session(session);
        usb_deliver(session, USB_LANDING_UNPLUGGED, false, NULL);
    }
}

static bool usb_cleanup_step_done(esp_err_t err)
{
    /* The MSC/host teardown APIs report INVALID_STATE when the requested
     * layer is already absent. Treat that explicitly as idempotent success. */
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

/* Full teardown sequence; runs in one worker task only. A layer is marked
 * absent only after its API confirms success/already-absent. On a real error
 * the handle/state and all lower layers remain available for a safe retry. */
static esp_err_t usb_teardown_steps(void)
{
    if (s_msc_vfs != NULL) {
        const esp_err_t err = msc_host_vfs_unregister(s_msc_vfs);
        if (!usb_cleanup_step_done(err)) {
            ESP_LOGE(TAG, "vfs unregister retained for retry: %s",
                     esp_err_to_name(err));
            return err;
        }
        s_msc_vfs = NULL;
    }
    if (s_msc_device != NULL) {
        const esp_err_t err = msc_host_uninstall_device(s_msc_device);
        if (!usb_cleanup_step_done(err)) {
            ESP_LOGE(TAG, "device uninstall retained for retry: %s",
                     esp_err_to_name(err));
            return err;
        }
        s_msc_device = NULL;
    }
    if (atomic_load_explicit(&s_msc_driver_installed,
                             memory_order_acquire)) {
        const esp_err_t drv_err = msc_host_uninstall();
        if (!usb_cleanup_step_done(drv_err)) {
            ESP_LOGE(TAG, "msc driver retained for retry: %s",
                     esp_err_to_name(drv_err));
            return drv_err;
        }
        atomic_store_explicit(&s_msc_driver_installed, false,
                              memory_order_release);
    }
    if (atomic_load_explicit(&s_host_running, memory_order_acquire)) {
        const esp_err_t stop_err = bsp_usb_host_stop();
        if (!usb_cleanup_step_done(stop_err)) {
            ESP_LOGE(TAG, "usb host retained for retry: %s",
                     esp_err_to_name(stop_err));
            return stop_err;
        }
        atomic_store_explicit(&s_host_running, false, memory_order_release);
    }
    atomic_store_explicit(&s_msc_connected, false, memory_order_release);
    atomic_store_explicit(&s_msc_disconnected, false, memory_order_release);
    return ESP_OK;
}

/* Mount worker only. s_worker_busy stays true for the complete DIR/stat
 * lifetime, so usb_request_teardown() can only set the cancellation flag and
 * cannot start VFS unregister concurrently. Returns an owned UI payload. */
static usb_file_list_t *usb_scan_file_list(void)
{
    usb_file_list_t *files = calloc(1, sizeof(*files));
    if (files == NULL) {
        return NULL;
    }
    files->entries = heap_caps_calloc(USB_LIST_MAX_ROWS,
                                      sizeof(usb_file_entry_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (files->entries == NULL) {
        files->status = USB_LIST_ALLOC_FAILED;
        return files;
    }

    DIR *dir = opendir(USB_MOUNT_PATH);
    if (dir == NULL) {
        files->status = USB_LIST_OPEN_FAILED;
        return files;
    }
    files->status = USB_LIST_OK;
    while (files->count < USB_LIST_MAX_ROWS &&
           !atomic_load_explicit(&s_teardown_requested,
                                 memory_order_acquire) &&
           !atomic_load_explicit(&s_msc_disconnected,
                                 memory_order_acquire)) {
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        usb_file_entry_t *item = &files->entries[files->count];
        snprintf(item->name, sizeof(item->name), "%s", entry->d_name);
        char path[USB_LIST_MAX_ROWS + 120];
        snprintf(path, sizeof(path), USB_MOUNT_PATH "/%.255s", item->name);
        struct stat st;
        if (stat(path, &st) == 0) {
            item->is_dir = S_ISDIR(st.st_mode);
            item->size = (uint64_t)st.st_size;
        }
        ++files->count;
    }
    closedir(dir);
    return files;
}

static void usb_mount_task(void *arg)
{
    const uint32_t session = (uint32_t)(uintptr_t)arg;
    atomic_store_explicit(&s_cleanup_session, session, memory_order_release);
    usb_file_list_t *files = NULL;
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err == ESP_OK) {
        atomic_store_explicit(&s_host_running, true, memory_order_release);
        atomic_store_explicit(&s_msc_connected, false, memory_order_release);
        atomic_store_explicit(&s_msc_disconnected, false,
                              memory_order_release);
        atomic_store_explicit(&s_msc_address, 0, memory_order_release);
        const msc_host_driver_config_t cfg = {
            .create_backround_task = true,
            .task_priority = 5,
            .stack_size = 4096,
            .core_id = tskNO_AFFINITY,
            .callback = usb_msc_event_cb,
            .callback_arg = (void *)(uintptr_t)session,
        };
        err = msc_host_install(&cfg);
        if (err == ESP_OK) {
            atomic_store_explicit(&s_msc_driver_installed, true,
                                  memory_order_release);
        }
    }
    if (err == ESP_OK) {
        const int64_t deadline =
            esp_timer_get_time() + USB_ENUM_TIMEOUT_MS * INT64_C(1000);
        while (!atomic_load_explicit(&s_msc_connected, memory_order_acquire) &&
                !atomic_load_explicit(&s_msc_disconnected,
                                      memory_order_acquire) &&
                !atomic_load_explicit(&s_teardown_requested,
                                      memory_order_acquire) &&
                usb_session_is_live(session) &&
                esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!atomic_load_explicit(&s_msc_connected, memory_order_acquire)) {
            err = (atomic_load_explicit(&s_msc_disconnected,
                                        memory_order_acquire) ||
                   atomic_load_explicit(&s_teardown_requested,
                                        memory_order_acquire) ||
                   !usb_session_is_live(session)) ?
                      ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        }
        if (atomic_load_explicit(&s_teardown_requested,
                                 memory_order_acquire) ||
                !usb_session_is_live(session)) {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err == ESP_OK) {
        err = msc_host_install_device(
            atomic_load_explicit(&s_msc_address, memory_order_acquire),
            &s_msc_device);
    }
    if (err == ESP_OK) {
        const esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 1,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
            .use_one_fat = false,
        };
        err = msc_host_vfs_register(s_msc_device, USB_MOUNT_PATH, &mount_cfg,
                                    &s_msc_vfs);
    }

    if (err == ESP_OK &&
            !atomic_load_explicit(&s_msc_disconnected,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&s_teardown_requested,
                                  memory_order_acquire)) {
        files = usb_scan_file_list();
    }

    if (err != ESP_OK ||
            atomic_load_explicit(&s_msc_disconnected,
                                 memory_order_acquire) ||
            atomic_load_explicit(&s_teardown_requested,
                                 memory_order_acquire)) {
        /* Failed mount or the page closed mid-flight: clean up here so no
         * host state leaks, then report the teardown completion. */
        usb_file_list_free(files);
        const esp_err_t cleanup_err = usb_teardown_steps();
        if (cleanup_err == ESP_OK) {
            atomic_store_explicit(&s_teardown_requested, false,
                                  memory_order_release);
            atomic_store_explicit(&s_teardown_attempts, 0,
                                  memory_order_release);
            atomic_store_explicit(&s_teardown_failed, false,
                                  memory_order_release);
            usb_deliver(session, USB_LANDING_TEARDOWN_DONE, true, NULL);
            atomic_store_explicit(&s_worker_busy, false,
                                  memory_order_release);
        } else {
            atomic_fetch_add_explicit(&s_teardown_attempts, 1,
                                      memory_order_acq_rel);
            atomic_store_explicit(&s_teardown_requested, true,
                                  memory_order_release);
            atomic_store_explicit(&s_worker_busy, false,
                                  memory_order_release);
            ESP_LOGE(TAG, "mount cleanup deferred for supervisor: %s",
                     esp_err_to_name(cleanup_err));
        }
    } else {
        ESP_LOGI(TAG, "USB drive mounted at %s", USB_MOUNT_PATH);
        usb_deliver(session, USB_LANDING_MOUNTED, true, files);
        atomic_store_explicit(&s_worker_busy, false, memory_order_release);
    }
    vTaskDelete(NULL);
}

static void usb_teardown_task(void *arg)
{
    const uint32_t session = (uint32_t)(uintptr_t)arg;
    const esp_err_t err = usb_teardown_steps();
    if (err == ESP_OK) {
        atomic_store_explicit(&s_teardown_requested, false,
                              memory_order_release);
        atomic_store_explicit(&s_teardown_attempts, 0, memory_order_release);
        atomic_store_explicit(&s_teardown_failed, false, memory_order_release);
        usb_deliver(session, USB_LANDING_TEARDOWN_DONE, true, NULL);
        atomic_store_explicit(&s_worker_busy, false, memory_order_release);
    } else {
        const unsigned int attempts = atomic_fetch_add_explicit(
            &s_teardown_attempts, 1, memory_order_acq_rel) + 1;
        if (attempts >= USB_TEARDOWN_RETRIES) {
            atomic_store_explicit(&s_teardown_failed, true,
                                  memory_order_release);
            ESP_LOGE(TAG,
                     "USB teardown stopped after %u attempts; state retained: %s",
                     attempts, esp_err_to_name(err));
        }
        atomic_store_explicit(&s_worker_busy, false, memory_order_release);
    }
    vTaskDelete(NULL);
}

static void usb_try_start_teardown(void)
{
    if (!atomic_load_explicit(&s_teardown_requested, memory_order_acquire) ||
            atomic_load_explicit(&s_teardown_failed, memory_order_acquire) ||
            atomic_load_explicit(&s_teardown_attempts, memory_order_acquire) >=
                USB_TEARDOWN_RETRIES) {
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_worker_busy, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }
    if (xTaskCreate(usb_teardown_task, "usb_teardown", USB_WORKER_STACK,
                    (void *)(uintptr_t)atomic_load_explicit(
                        &s_cleanup_session, memory_order_acquire),
                    USB_WORKER_PRIO,
                    NULL) != pdPASS) {
        const unsigned int attempts = atomic_fetch_add_explicit(
            &s_teardown_attempts, 1, memory_order_acq_rel) + 1;
        atomic_store_explicit(&s_worker_busy, false, memory_order_release);
        if (attempts >= USB_TEARDOWN_RETRIES) {
            atomic_store_explicit(&s_teardown_failed, true,
                                  memory_order_release);
            ESP_LOGE(TAG,
                     "teardown task create failed %u times; host state retained",
                     attempts);
        } else {
            ESP_LOGW(TAG, "teardown task create failed (%u/%u), retry queued",
                     attempts, USB_TEARDOWN_RETRIES);
        }
    }
}

/* Permanent page-independent supervisor. It makes task-creation and cleanup
 * failures bounded/retryable even after LV_EVENT_DELETE removed the UI timer. */
static void usb_supervisor_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_try_start_teardown();
        vTaskDelay(pdMS_TO_TICKS(USB_SUPERVISOR_MS));
    }
}

static bool usb_ensure_supervisor(void)
{
    if (s_supervisor_task != NULL) {
        return true;
    }
    if (xTaskCreate(usb_supervisor_task, "usb_supervisor",
                    USB_SUPERVISOR_STACK, NULL, USB_WORKER_PRIO - 1,
                    &s_supervisor_task) != pdPASS) {
        s_supervisor_task = NULL;
        ESP_LOGE(TAG, "USB supervisor task create failed; mounting disabled");
        return false;
    }
    return true;
}

static void usb_queue_teardown_session(uint32_t session)
{
    const bool already_requested = atomic_exchange_explicit(
        &s_teardown_requested, true, memory_order_acq_rel);
    if (!already_requested ||
            atomic_load_explicit(&s_teardown_failed, memory_order_acquire)) {
        atomic_store_explicit(&s_cleanup_session, session,
                              memory_order_release);
        atomic_store_explicit(&s_teardown_attempts, 0, memory_order_release);
        atomic_store_explicit(&s_teardown_failed, false, memory_order_release);
    }
}

static void usb_request_teardown_session(uint32_t session)
{
    usb_queue_teardown_session(session);
    /* The direct attempt gives responsive button/page-exit handling; the
     * permanent supervisor owns all later retries. */
    usb_try_start_teardown();
}

static void usb_request_teardown(void)
{
    usb_request_teardown_session(s_usb.session);
}

/* ------------------------------------------------------------------ */
/* USB Device (CDC) session                                            */
/*                                                                     */
/* The OTG peripheral is single-role: the host stack must be completely */
/* down before TinyUSB installs, and the DRP role must be restored when */
/* the session ends so the port can become a Source again. The device   */
/* task owns the whole lifecycle; the LVGL poll only requests start and */
/* stop through the two atomic flags below.                             */
/* ------------------------------------------------------------------ */

static atomic_uint s_dev_session;        /* page session of the live/pending session */
static atomic_bool s_dev_start_requested;
static led_indicator_handle_t s_dev_led;
static bool s_dev_led_attempted;

static void usb_dev_send(const char *text)
{
    const size_t length = strlen(text);
    if (tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                   (const uint8_t *)text, length) == length) {
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
}

static void usb_dev_send_line(const char *text)
{
    usb_dev_send(text);
    usb_dev_send("\r\n");
}

static void usb_dev_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    if (s_dev_queue == NULL) {
        return;
    }
    usb_dev_msg_t message = {0};
    /* Leave room for the NUL: the command parser does strlen/strcmp on
     * data, so a full 128-byte read must never run off the end. */
    if (tinyusb_cdcacm_read(itf, message.data, sizeof(message.data) - 1,
                            &message.length) == ESP_OK &&
            message.length > 0) {
        message.data[message.length] = '\0';
        xQueueSend(s_dev_queue, &message, 0);
    }
}

static void usb_dev_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_dev_dtr = event->line_state_changed_data.dtr != 0;
    if (!s_dev_dtr) {
        s_dev_banner_sent = false;
    }
}

static void usb_dev_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        /* Host finished enumeration: publish through the mailbox so the
         * UI update keeps the documented LVGL-thread context. */
        atomic_store_explicit(&s_dev_configured, true, memory_order_release);
        usb_deliver(atomic_load_explicit(&s_dev_session,
                                         memory_order_acquire),
                    USB_LANDING_DEV_CONFIGURED, true, NULL);
    }
}

static void usb_dev_trim(char *command)
{
    size_t length = strlen(command);
    while (length > 0 && (command[length - 1] == '\r' ||
                          command[length - 1] == '\n')) {
        command[--length] = '\0';
    }
}

/* Command set mirrors firmware/usb_cdc_device (led commands included: the
 * BSP LED API is already a demo dependency). The status line reports
 * battery/charge state alongside the firmware identity. */
static void usb_dev_process(char *command)
{
    usb_dev_trim(command);
    if (strcmp(command, "ping") == 0) {
        usb_dev_send_line("pong");
    } else if (strcmp(command, "hello") == 0) {
        usb_dev_send_line("Hello from Candis-S31!");
    } else if (strcmp(command, "status") == 0) {
        char line[192];
        svc_power_status_t power = {0};
        svc_power_get_status(&power);
        char soc[24];
        if (!power.present) {
            snprintf(soc, sizeof(soc), "absent");
        } else if (power.percent < 0) {
            snprintf(soc, sizeof(soc), "no model");
        } else {
            snprintf(soc, sizeof(soc), "%d%%", power.percent);
        }
        snprintf(line, sizeof(line),
                 "status: Candis-S31 rev %s, battery=%s, %d mV, soc=%s, "
                 "charging=%s, vbus=%s, uptime=%lld s",
                 CANDIS_S31_BSP_GIT_REV,
                 power.present ? "present" : "absent", power.battery_mv,
                 soc, power.charging ? "yes" : "no",
                 power.vbus ? "yes" : "no",
                 esp_timer_get_time() / INT64_C(1000000));
        usb_dev_send_line(line);
    } else if (strcmp(command, "led on") == 0 ||
               strcmp(command, "led off") == 0) {
        const bool on = command[4] == 'o' && command[5] == 'n';
        if (!s_dev_led_attempted) {
            s_dev_led_attempted = true;
            led_indicator_handle_t leds[BSP_LED_NUM] = {0};
            int led_count = 0;
            if (bsp_led_indicator_create(leds, &led_count, BSP_LED_NUM) ==
                    ESP_OK &&
                    led_count == BSP_LED_NUM) {
                s_dev_led = leds[BSP_LED_1];
            }
        }
        if (s_dev_led == NULL) {
            usb_dev_send_line("led: unavailable");
        } else if (bsp_led_set(s_dev_led, on) == ESP_OK) {
            usb_dev_send_line(on ? "led: on" : "led: off");
        } else {
            usb_dev_send_line("led: failed");
        }
    } else if (command[0] != '\0') {
        char echo[USB_DEV_CMD_SIZE + 16];
        snprintf(echo, sizeof(echo), "echo: %s", command);
        usb_dev_send_line(echo);
    }
}

/* Bring the CDC stack up. Runs on the device task only, after the host
 * side is fully down (guaranteed by the caller). */
static esp_err_t usb_dev_stack_start(void)
{
    /* Lock the port in Sink: DRP could renegotiate Source mid-session and
     * re-apply VBUS on a peer that is powering us. */
    const esp_err_t role_err = bsp_type_c_set_role(BSP_TYPE_C_ROLE_SINK,
                                                   BSP_TYPE_C_CURRENT_DEFAULT);
    if (role_err != ESP_OK) {
        ESP_LOGW(TAG, "Sink role lock failed: %s", esp_err_to_name(role_err));
    }

    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(usb_dev_event_cb);
    config.task.size = USB_DEVICE_STACK;
    config.task.priority = USB_DEVICE_PRIO;
    esp_err_t err = tinyusb_driver_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb install failed: %s", esp_err_to_name(err));
        return err;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = usb_dev_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = usb_dev_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    err = tinyusb_cdcacm_init(&cdc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc acm init failed: %s", esp_err_to_name(err));
        const esp_err_t uninstall_err = tinyusb_driver_uninstall();
        if (uninstall_err != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb uninstall after cdc failure: %s",
                     esp_err_to_name(uninstall_err));
        }
        return err;
    }
    return ESP_OK;
}

static void usb_dev_stack_stop(void)
{
    const esp_err_t cdc_err = tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    if (cdc_err != ESP_OK && cdc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "cdc acm deinit: %s", esp_err_to_name(cdc_err));
    }
    const esp_err_t drv_err = tinyusb_driver_uninstall();
    if (drv_err != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb uninstall: %s", esp_err_to_name(drv_err));
    }
    /* Restore DRP so the port can negotiate Source again (drive mount). */
    const esp_err_t role_err = bsp_type_c_set_role(BSP_TYPE_C_ROLE_DRP,
                                                   BSP_TYPE_C_CURRENT_DEFAULT);
    if (role_err != ESP_OK) {
        ESP_LOGW(TAG, "DRP role restore failed: %s", esp_err_to_name(role_err));
    }
    s_dev_dtr = false;
    s_dev_banner_sent = false;
}

/* Full CDC session lifecycle, one task per session. */
static void usb_device_task(void *arg)
{
    (void)arg;
    const uint32_t session = atomic_load_explicit(&s_dev_session,
                                                  memory_order_acquire);
    usb_dev_msg_t message;

    /* The host stack owns the peripheral first: wait for every teardown
     * layer to be gone. usb_request_teardown() already runs from the
     * page poll/exit, so this normally just observes completion. */
    while (!atomic_load_explicit(&s_dev_stop_requested,
                                 memory_order_acquire) &&
            usb_session_is_live(session) &&
            (atomic_load_explicit(&s_host_running, memory_order_acquire) ||
             atomic_load_explicit(&s_worker_busy, memory_order_acquire) ||
             atomic_load_explicit(&s_teardown_requested,
                                  memory_order_acquire))) {
        vTaskDelay(pdMS_TO_TICKS(USB_SUPERVISOR_MS));
    }

    bool stack_up = false;
    if (!atomic_load_explicit(&s_dev_stop_requested, memory_order_acquire) &&
            usb_session_is_live(session)) {
        stack_up = usb_dev_stack_start() == ESP_OK;
    }
    if (!stack_up) {
        atomic_store_explicit(&s_dev_start_requested, false,
                              memory_order_release);
        atomic_store_explicit(&s_dev_task_live, false, memory_order_release);
        vTaskDelete(NULL);
        return;
    }
    atomic_store_explicit(&s_dev_running, true, memory_order_release);
    usb_deliver(session, USB_LANDING_DEV_STARTED, true, NULL);

    for (;;) {
        const bool stop =
            atomic_load_explicit(&s_dev_stop_requested,
                                 memory_order_acquire) ||
            !usb_session_is_live(session);
        if (stop) {
            break;
        }
        if (xQueueReceive(s_dev_queue, &message,
                          pdMS_TO_TICKS(USB_DEV_POLL_MS)) == pdTRUE) {
            usb_dev_process((char *)message.data);
        } else if (s_dev_dtr && !s_dev_banner_sent) {
            usb_dev_send_line("Candis-S31 USB CDC");
            usb_dev_send_line("Commands: ping, hello, status, led on, led off");
            s_dev_banner_sent = true;
        }
    }

    usb_dev_stack_stop();
    atomic_store_explicit(&s_dev_running, false, memory_order_release);
    atomic_store_explicit(&s_dev_configured, false, memory_order_release);
    atomic_store_explicit(&s_dev_stop_requested, false,
                          memory_order_release);
    atomic_store_explicit(&s_dev_start_requested, false,
                          memory_order_release);
    usb_deliver(session, USB_LANDING_DEV_STOPPED, false, NULL);
    atomic_store_explicit(&s_dev_task_live, false, memory_order_release);
    vTaskDelete(NULL);
}

/* LVGL poll: start a CDC session when attached as Sink with no host side. */
static void usb_device_start_request(void)
{
    if (atomic_load_explicit(&s_dev_task_live, memory_order_acquire) ||
            atomic_load_explicit(&s_dev_start_requested,
                                 memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&s_dev_session, s_usb.session, memory_order_release);
    atomic_store_explicit(&s_dev_start_requested, true, memory_order_release);
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_dev_task_live, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) {
        atomic_store_explicit(&s_dev_start_requested, false,
                              memory_order_release);
        return;
    }
    if (s_dev_queue == NULL) {
        s_dev_queue = xQueueCreate(USB_DEV_QUEUE_DEPTH,
                                   sizeof(usb_dev_msg_t));
    }
    if (s_dev_queue == NULL) {
        atomic_store_explicit(&s_dev_task_live, false, memory_order_release);
        atomic_store_explicit(&s_dev_start_requested, false,
                              memory_order_release);
        ESP_LOGE(TAG, "USB device queue create failed");
        return;
    }
    /* If a host session is still up, request its teardown first; the
     * device task waits for the supervisor to finish it. */
    if (atomic_load_explicit(&s_host_running, memory_order_acquire) ||
            atomic_load_explicit(&s_worker_busy, memory_order_acquire)) {
        usb_request_teardown();
    }
    if (xTaskCreate(usb_device_task, "usb_device", USB_DEVICE_STACK,
                    NULL, USB_DEVICE_PRIO, NULL) != pdPASS) {
        atomic_store_explicit(&s_dev_task_live, false, memory_order_release);
        atomic_store_explicit(&s_dev_start_requested, false,
                              memory_order_release);
        ESP_LOGE(TAG, "USB device task create failed");
    }
}

/* LVGL poll / page exit: end the CDC session. */
static void usb_device_stop_request(void)
{
    atomic_store_explicit(&s_dev_stop_requested, true, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* ui_async landing functions (LVGL thread)                            */
/* ------------------------------------------------------------------ */

/* Sink paragraph reflects the real CDC session state: never claim the PC
 * connection before the device stack is actually installed. */
static void usb_sink_label_refresh(void)
{
    if (s_usb.sink_lbl == NULL) {
        return;
    }
    if (atomic_load_explicit(&s_dev_configured, memory_order_acquire)) {
        lv_label_set_text(s_usb.sink_lbl,
                          "Device mode: CDC session active\n\n"
                          "The PC enumerated this board as a USB serial port\n"
                          "(VID 0x303A). Commands: ping, hello, status,\n"
                          "led on, led off");
    } else if (atomic_load_explicit(&s_dev_running, memory_order_acquire)) {
        lv_label_set_text(s_usb.sink_lbl,
                          "Device mode: CDC running, waiting for the PC\n"
                          "to enumerate...");
    } else if (atomic_load_explicit(&s_dev_task_live, memory_order_acquire)) {
        lv_label_set_text(s_usb.sink_lbl,
                          "Device mode: starting CDC stack\n"
                          "(host-side teardown must finish first)");
    } else {
        lv_label_set_text(s_usb.sink_lbl,
                          "Device mode\n\n"
                          "The peer host powers this device.\n"
                          "CDC session stopped.");
    }
}

static void usb_handle_landing(const usb_landing_t *landing)
{
    if (!s_usb.active || landing->session != s_usb.session ||
            landing->seq <= s_usb.last_event_seq) {
        return;
    }
    s_usb.last_event_seq = landing->seq;
    switch (landing->event) {
    case USB_LANDING_MOUNTED:
        s_usb.state = USB_PAGE_MOUNTED;
        usb_visibility(true, false, false, true, true);
        lv_label_set_text(s_usb.info_lbl, "USB drive mounted:" USB_MOUNT_PATH);
        usb_render_file_list(landing->files);
        usb_set_status_icon(1);
        ui_toast("USB drive mounted");
        break;
    case USB_LANDING_TEARDOWN_DONE:
        usb_set_status_icon(0);
        /* Back to detached; the same poll tick may re-detect the role. */
        s_usb.state = USB_PAGE_IDLE;
        usb_visibility(true, false, false, false, false);
        lv_label_set_text(s_usb.info_lbl, "Checking Type-C...");
        break;
    case USB_LANDING_UNPLUGGED:
        if (s_usb.state == USB_PAGE_MOUNTED) {
            ui_toast("USB drive removed");
        }
        lv_label_set_text(s_usb.info_lbl, "USB drive removed, cleaning...");
        usb_visibility(true, false, false, false, false);
        /* The MSC callback already queued teardown. Let the supervisor start
         * it after the driver callback has returned. */
        break;
    case USB_LANDING_DEV_STARTED:
        if (s_usb.state == USB_PAGE_SINK) {
            usb_sink_label_refresh();
        }
        break;
    case USB_LANDING_DEV_CONFIGURED:
        if (s_usb.state == USB_PAGE_SINK) {
            usb_sink_label_refresh();
            usb_set_status_icon(2);
        }
        break;
    case USB_LANDING_DEV_STOPPED:
        if (s_usb.state == USB_PAGE_SINK) {
            usb_sink_label_refresh();
        }
        break;
    case USB_LANDING_NONE:
    default:
        break;
    }
}

static void usb_apply_landing(void *arg)
{
    usb_landing_t *landing = arg;
    usb_handle_landing(landing);
    usb_file_list_free(landing->files);
    free(landing);
}

static void usb_consume_mailbox(void)
{
    usb_landing_t landing = usb_mailbox_take();
    if (landing.event != USB_LANDING_NONE) {
        usb_handle_landing(&landing);
    }
    usb_file_list_free(landing.files);
}

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

static void mount_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_usb.active || s_usb.state != USB_PAGE_SOURCE ||
            s_supervisor_task == NULL ||
            atomic_load_explicit(&s_worker_busy, memory_order_acquire) ||
            atomic_load_explicit(&s_host_running, memory_order_acquire) ||
            atomic_load_explicit(&s_teardown_requested,
                                 memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_worker_busy, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) {
        return;
    }
    s_usb.state = USB_PAGE_MOUNTING;
    atomic_store_explicit(&s_teardown_requested, false, memory_order_release);
    usb_visibility(true, false, false, false, false);
    lv_label_set_text(s_usb.info_lbl, "Mounting USB drive...");
    if (xTaskCreate(usb_mount_task, "usb_mount", USB_WORKER_STACK,
                    (void *)(uintptr_t)s_usb.session, USB_WORKER_PRIO,
                    NULL) != pdPASS) {
        atomic_store_explicit(&s_worker_busy, false, memory_order_release);
        s_usb.state = USB_PAGE_SOURCE;
        usb_visibility(true, false, true, false, false);
        lv_label_set_text(s_usb.info_lbl, "Mount task create failed");
    }
}

static void unmount_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_usb.active ||
            atomic_load_explicit(&s_worker_busy, memory_order_acquire)) {
        return;
    }
    if (s_usb.state != USB_PAGE_MOUNTED) {
        return;
    }
    lv_label_set_text(s_usb.info_lbl, "Unmounting...");
    usb_visibility(true, false, false, false, false);
    usb_request_teardown();
}

/* ------------------------------------------------------------------ */
/* Type-C status poll (LVGL thread; ~1-2 ms I2C reads)                 */
/* ------------------------------------------------------------------ */

static const char *type_c_role_zh(bsp_type_c_role_t role)
{
    switch (role) {
    case BSP_TYPE_C_ROLE_SOURCE: return "Source (host, can mount drives)";
    case BSP_TYPE_C_ROLE_SINK:   return "Sink (device)";
    case BSP_TYPE_C_ROLE_DRP:    return "DRP (negotiating)";
    default:                     return "Disabled";
    }
}

static void usb_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_usb.active) {
        return;
    }
    usb_consume_mailbox();

    if (s_supervisor_task == NULL) {
        lv_label_set_text(s_usb.info_lbl,
                          "USB cleanup service unavailable\nMount disabled to avoid host-stack residue");
        usb_visibility(true, false, false, false, false);
        usb_set_status_icon(0);
        return;
    }
    if (atomic_load_explicit(&s_teardown_failed, memory_order_acquire)) {
        lv_label_set_text(s_usb.info_lbl,
                          "USB cleanup retries exhausted\nHost state kept, re-enter page to retry");
        usb_visibility(true, false, false, false, false);
        usb_set_status_icon(0);
        return;
    }

    /* Review item 7: the USB host owns the FUSB303B while running; the
     * poll pauses until teardown completes. */
    if (atomic_load_explicit(&s_host_running, memory_order_acquire) ||
            atomic_load_explicit(&s_worker_busy, memory_order_acquire) ||
            s_usb.state == USB_PAGE_MOUNTED ||
            s_usb.state == USB_PAGE_MOUNTING) {
        return;
    }

    bsp_type_c_status_t st = {0};
    if (bsp_type_c_get_status(&st, false) != ESP_OK) {
        lv_label_set_text(s_usb.info_lbl, "Type-C status read failed");
        usb_visibility(true, false, false, false, false);
        usb_set_status_icon(0);
        return;
    }

    if (!st.attached) {
        if (s_usb.state != USB_PAGE_IDLE) {
            s_usb.state = USB_PAGE_IDLE;
            usb_visibility(true, false, false, false, false);
        }
        /* Cable gone: end any CDC session so DRP is free again. */
        usb_device_stop_request();
        lv_label_set_text(s_usb.info_lbl,
                          "No connection\n\nInsert a drive or host to auto-detect");
        usb_set_status_icon(0);
        return;
    }

    const char *orientation = st.orientation == 1 ? "CC1" :
                              st.orientation == 2 ? "CC2" : "Unknown";
    lv_label_set_text_fmt(s_usb.info_lbl,
                          "Status: connected\nRole: %s\nVBUS: %s\nOrientation: %s",
                          type_c_role_zh(st.role),
                          st.vbus_ok ? "OK" : "Fault",
                          orientation);

    if (st.role == BSP_TYPE_C_ROLE_SOURCE) {
        /* Role left Sink: any CDC session must release the Sink lock
         * (idempotent when no session is running). */
        usb_device_stop_request();
        if (s_usb.state != USB_PAGE_SOURCE) {
            s_usb.state = USB_PAGE_SOURCE;
            usb_visibility(true, false, true, false, false);
        }
        usb_set_status_icon(1);
    } else if (st.role == BSP_TYPE_C_ROLE_SINK) {
        if (st.vbus_ok) {
            /* Peer is actually powering us: a real CDC session is
             * meaningful. */
            usb_device_start_request();
        } else {
            /* Attached as Sink but nobody is powering us: release any
             * Sink lock so the port can renegotiate as a Source. */
            usb_device_stop_request();
        }
        if (s_usb.state != USB_PAGE_SINK) {
            s_usb.state = USB_PAGE_SINK;
            usb_visibility(true, true, false, false, false);
        }
        usb_sink_label_refresh();
        usb_set_status_icon(2);
    } else {
        /* DRP toggling while attached: keep the info text, no actions. */
        usb_device_stop_request();
        if (s_usb.state != USB_PAGE_IDLE) {
            s_usb.state = USB_PAGE_IDLE;
            usb_visibility(true, false, false, false, false);
        }
        usb_set_status_icon(0);
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void usb_delete_cb(lv_event_t *event)
{
    (void)event;
    s_usb.active = false;
    if (s_usb.poll_timer != NULL) {
        lv_timer_delete(s_usb.poll_timer);
        s_usb.poll_timer = NULL;
    }
    usb_set_status_icon(0);
    /* Never leak host state on exit: if the stack is up, tear it down in
     * a worker task; a mount still in flight observes the request flag. */
    if (atomic_load_explicit(&s_host_running, memory_order_acquire) ||
            atomic_load_explicit(&s_worker_busy, memory_order_acquire)) {
        usb_request_teardown();
    }
    /* End any CDC session too: the device task observes both the stop
     * flag and the session liveness. */
    usb_device_stop_request();
    unsigned int expected = s_usb.session;
    atomic_compare_exchange_strong_explicit(
        &s_live_session, &expected, 0, memory_order_acq_rel,
        memory_order_acquire);
    usb_landing_t abandoned = usb_mailbox_take();
    usb_file_list_free(abandoned.files);
}

lv_obj_t *app_usb_create(void)
{
    memset(&s_usb, 0, sizeof(s_usb));
    s_usb.active = true;
    s_usb.state = USB_PAGE_IDLE;
    s_usb.session = atomic_fetch_add_explicit(&s_session_seq, 1,
                                               memory_order_relaxed) + 1;
    usb_file_list_t *abandoned_files = NULL;
    portENTER_CRITICAL(&s_landing_lock);
    abandoned_files = s_landing_mailbox.files;
    s_landing_mailbox = (usb_landing_t){ 0 };
    portEXIT_CRITICAL(&s_landing_lock);
    usb_file_list_free(abandoned_files);
    atomic_store_explicit(&s_event_seq, 0, memory_order_relaxed);
    atomic_store_explicit(&s_live_session, s_usb.session,
                          memory_order_release);
    const bool supervisor_ready = usb_ensure_supervisor();
    if (!atomic_load_explicit(&s_host_running, memory_order_acquire) &&
            !atomic_load_explicit(&s_worker_busy, memory_order_acquire)) {
        atomic_store_explicit(&s_teardown_requested, false,
                              memory_order_release);
    }
    /* Recovery for exhausted teardown retries: the failure latches across
     * page exits, so re-entering the page gets exactly ONE automatic retry
     * (queue_teardown_session resets attempts/failed). A second failure
     * keeps the latch and the poll message - no unbounded loop. */
    if (supervisor_ready &&
            atomic_load_explicit(&s_teardown_failed, memory_order_acquire) &&
            s_teardown_retry_session != s_usb.session) {
        s_teardown_retry_session = s_usb.session;
        ESP_LOGI(TAG, "retrying exhausted USB teardown once");
        usb_request_teardown_session(s_usb.session);
    }

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("USB OTG", &content);
    lv_obj_add_event_cb(root, usb_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_text_font(content, ui_font_body(), 0);

    s_usb.info_lbl = lv_label_create(content);
    /* Width stays clear of the unmount button (x=330) shown when mounted. */
    lv_obj_set_width(s_usb.info_lbl, 292);
    lv_obj_set_pos(s_usb.info_lbl, 2, 4);
    lv_label_set_text(s_usb.info_lbl, "Checking Type-C...");

    s_usb.sink_lbl = lv_label_create(content);
    lv_obj_set_width(s_usb.sink_lbl, 428);
    lv_obj_set_pos(s_usb.sink_lbl, 0, 110);
    lv_obj_set_style_text_color(s_usb.sink_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_usb.sink_lbl,
                      "Device mode\n\n"
                      "The peer host powers this device.\n"
                      "The CDC session starts when the Type-C role is Sink.");

    s_usb.mount_btn = lv_button_create(content);
    lv_obj_set_size(s_usb.mount_btn, 208, 64);
    lv_obj_set_pos(s_usb.mount_btn, 110, 120);
    lv_obj_add_event_cb(s_usb.mount_btn, mount_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mount_lbl = lv_label_create(s_usb.mount_btn);
    lv_label_set_text(mount_lbl, "Mount USB drive");
    lv_obj_center(mount_lbl);

    s_usb.unmount_btn = lv_button_create(content);
    lv_obj_set_size(s_usb.unmount_btn, 120, UI_TOUCH_MIN);
    lv_obj_set_pos(s_usb.unmount_btn, 308, 0);
    lv_obj_add_event_cb(s_usb.unmount_btn, unmount_btn_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *unmount_lbl = lv_label_create(s_usb.unmount_btn);
    lv_label_set_text(unmount_lbl, "Unmount");
    lv_obj_center(unmount_lbl);

    s_usb.list = lv_list_create(content);
    lv_obj_set_size(s_usb.list, 428, 284);
    lv_obj_set_pos(s_usb.list, 0, 56);
    lv_obj_set_style_bg_color(s_usb.list, lv_color_hex(UI_COLOR_SURFACE), 0);

    usb_visibility(true, false, false, false, false);

    if (!supervisor_ready) {
        lv_label_set_text(s_usb.info_lbl,
                          "USB cleanup service failed to start\nRe-enter this page");
    }

    s_usb.poll_timer = lv_timer_create(usb_poll_cb, USB_POLL_MS, NULL);
    return root;
}
