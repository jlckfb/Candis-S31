/*
 * Candis-S31 watch demo - USB OTG app.
 *
 * While the page is open a 200 ms lv_timer polls bsp_type_c_get_status()
 * (clear_interrupts=false) and renders the FUSB303B role in Chinese.
 * Role Source offers "挂载 U 盘": a worker task runs the factory-verified
 * sequence (bsp_usb_host_start -> msc_host_install -> wait for
 * MSC_DEVICE_CONNECTED -> install_device -> vfs_register on /usb0), then
 * the file list is shown. While the host stack runs, Type-C polling is
 * suspended (the host owns the FUSB303B, review item 7) and removal is
 * detected through the MSC event callback. Unmount/exit/removal run the
 * full teardown (vfs unregister -> device uninstall -> driver uninstall ->
 * bsp_usb_host_stop) in a worker task, never on the LVGL thread. Role Sink
 * shows the Device-mode explainer page (C2 enumerates as CDC on a PC).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

#define USB_POLL_MS          200
#define USB_WORKER_STACK     6144
#define USB_WORKER_PRIO      4
#define USB_ENUM_TIMEOUT_MS  10000
#define USB_MOUNT_PATH       "/usb0"
#define USB_LIST_MAX_ROWS    200

typedef enum {
    USB_PAGE_IDLE = 0,   /* detached or role unresolved */
    USB_PAGE_SOURCE,     /* attached as Source, not mounted */
    USB_PAGE_SINK,       /* attached as Sink (device mode) */
    USB_PAGE_MOUNTING,
    USB_PAGE_MOUNTED,
} usb_page_state_t;

static struct {
    bool active;
    volatile usb_page_state_t state;
    lv_obj_t *info_lbl;
    lv_obj_t *sink_lbl;
    lv_obj_t *mount_btn;
    lv_obj_t *unmount_btn;
    lv_obj_t *list;
    lv_timer_t *poll_timer;
    int last_status_icon; /* last value sent to ui_status_set_usb() */
} s_usb;

/* Host/worker bookkeeping shared with the MSC task and worker tasks. */
static volatile bool s_host_running;      /* FUSB303B owned by USB host */
static volatile bool s_worker_busy;       /* mount or teardown task live */
static volatile bool s_teardown_requested;
static volatile bool s_msc_connected;
static volatile bool s_msc_disconnected;
static volatile uint8_t s_msc_address;
static msc_host_device_handle_t s_msc_device; /* worker-task owned */
static msc_host_vfs_handle_t s_msc_vfs;       /* worker-task owned */

static const char *TAG = "app_usb";

static void usb_on_unplugged(void *arg);

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

static void usb_load_file_list(void)
{
    lv_obj_clean(s_usb.list);
    DIR *dir = opendir(USB_MOUNT_PATH);
    if (dir == NULL) {
        lv_obj_t *note = lv_label_create(s_usb.list);
        lv_label_set_text(note, "目录读取失败");
        return;
    }
    int rows = 0;
    while (rows < USB_LIST_MAX_ROWS) {
        const struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), USB_MOUNT_PATH "/%s", entry->d_name);
        struct stat st;
        bool is_dir = false;
        uint64_t size = 0;
        if (stat(path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (uint64_t)st.st_size;
        }
        char text[160];
        if (is_dir) {
            snprintf(text, sizeof(text), "%.159s", entry->d_name);
        } else if (size >= UINT64_C(1048576)) {
            snprintf(text, sizeof(text), "%.130s  (%.1f MB)", entry->d_name,
                     (double)size / 1048576.0);
        } else if (size >= UINT64_C(1024)) {
            snprintf(text, sizeof(text), "%.130s  (%.1f KB)", entry->d_name,
                     (double)size / 1024.0);
        } else {
            snprintf(text, sizeof(text), "%.130s  (%" PRIu64 " B)",
                     entry->d_name, size);
        }
        lv_list_add_button(s_usb.list,
                           is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE, text);
        ++rows;
    }
    closedir(dir);
    if (rows == 0) {
        lv_obj_t *note = lv_label_create(s_usb.list);
        lv_label_set_text(note, "(空 U 盘)");
    }
}

/* ------------------------------------------------------------------ */
/* MSC worker tasks (never LVGL thread)                                */
/* ------------------------------------------------------------------ */

static void usb_msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        s_msc_address = event->device.address;
        s_msc_connected = true;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        s_msc_connected = false;
        s_msc_disconnected = true;
        if (s_usb.state == USB_PAGE_MOUNTED) {
            ui_async(usb_on_unplugged, NULL);
        }
    }
}

/* Full teardown sequence; runs in a worker task only. */
static void usb_teardown_steps(void)
{
    if (s_msc_vfs != NULL) {
        msc_host_vfs_handle_t vfs = s_msc_vfs;
        s_msc_vfs = NULL;
        const esp_err_t err = msc_host_vfs_unregister(vfs);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vfs unregister: %s", esp_err_to_name(err));
        }
    }
    if (s_msc_device != NULL) {
        msc_host_device_handle_t dev = s_msc_device;
        s_msc_device = NULL;
        const esp_err_t err = msc_host_uninstall_device(dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "device uninstall: %s", esp_err_to_name(err));
        }
    }
    if (s_host_running) {
        const esp_err_t drv_err = msc_host_uninstall();
        if (drv_err != ESP_OK && drv_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "msc uninstall: %s", esp_err_to_name(drv_err));
        }
        const esp_err_t stop_err = bsp_usb_host_stop();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "usb host stop: %s", esp_err_to_name(stop_err));
        }
        s_host_running = false;
    }
    s_msc_connected = false;
    s_msc_disconnected = false;
}

static void usb_on_mount_result(void *arg);
static void usb_on_teardown_done(void *arg);

static void usb_mount_task(void *arg)
{
    (void)arg;
    esp_err_t err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (err == ESP_OK) {
        s_host_running = true;
        s_msc_connected = false;
        s_msc_disconnected = false;
        s_msc_address = 0;
        const msc_host_driver_config_t cfg = {
            .create_backround_task = true,
            .task_priority = 5,
            .stack_size = 4096,
            .core_id = tskNO_AFFINITY,
            .callback = usb_msc_event_cb,
        };
        err = msc_host_install(&cfg);
    }
    if (err == ESP_OK) {
        const int64_t deadline =
            esp_timer_get_time() + USB_ENUM_TIMEOUT_MS * INT64_C(1000);
        while (!s_msc_connected && !s_msc_disconnected &&
                esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_msc_connected) {
            err = s_msc_disconnected ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
        }
    }
    if (err == ESP_OK) {
        err = msc_host_install_device(s_msc_address, &s_msc_device);
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
        if (err != ESP_OK) {
            msc_host_uninstall_device(s_msc_device);
            s_msc_device = NULL;
        }
    }

    if (err != ESP_OK || s_teardown_requested) {
        /* Failed mount or the page closed mid-flight: clean up here so no
         * host state leaks, then report the teardown completion. */
        usb_teardown_steps();
        ui_async(usb_on_teardown_done, NULL);
    } else {
        ESP_LOGI(TAG, "USB drive mounted at %s", USB_MOUNT_PATH);
        ui_async(usb_on_mount_result, NULL);
    }
    vTaskDelete(NULL);
}

static void usb_teardown_task(void *arg)
{
    (void)arg;
    usb_teardown_steps();
    ui_async(usb_on_teardown_done, NULL);
    vTaskDelete(NULL);
}

/* Kick a teardown exactly once; safe from LVGL thread. */
static void usb_request_teardown(void)
{
    s_teardown_requested = true;
    if (s_worker_busy) {
        /* A mount task is still running; it observes the flag and tears
         * down itself before reporting. */
        return;
    }
    s_worker_busy = true;
    if (xTaskCreate(usb_teardown_task, "usb_teardown", USB_WORKER_STACK,
                    NULL, USB_WORKER_PRIO, NULL) != pdPASS) {
        s_worker_busy = false;
        ESP_LOGE(TAG, "teardown task create failed");
    }
}

/* ------------------------------------------------------------------ */
/* ui_async landing functions (LVGL thread)                            */
/* ------------------------------------------------------------------ */

static void usb_on_mount_result(void *arg)
{
    (void)arg;
    s_worker_busy = false;
    if (!s_usb.active) {
        return;
    }
    s_usb.state = USB_PAGE_MOUNTED;
    s_teardown_requested = false;
    usb_visibility(true, false, false, true, true);
    lv_label_set_text(s_usb.info_lbl, "U 盘已挂载:" USB_MOUNT_PATH);
    usb_load_file_list();
    usb_set_status_icon(1);
    ui_toast("U 盘已挂载");
}

static void usb_on_teardown_done(void *arg)
{
    (void)arg;
    s_worker_busy = false;
    s_teardown_requested = false;
    usb_set_status_icon(0);
    if (!s_usb.active) {
        return;
    }
    /* Back to the detached look; the next poll tick re-detects the role. */
    s_usb.state = USB_PAGE_IDLE;
    usb_visibility(true, false, false, false, false);
    lv_label_set_text(s_usb.info_lbl, "正在检测 Type-C 状态…");
}

static void usb_on_unplugged(void *arg)
{
    (void)arg;
    if (!s_usb.active) {
        return;
    }
    if (s_usb.state == USB_PAGE_MOUNTED) {
        ui_toast("U 盘已拔出");
    }
    usb_request_teardown();
}

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

static void mount_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_usb.active || s_usb.state != USB_PAGE_SOURCE || s_worker_busy) {
        return;
    }
    s_usb.state = USB_PAGE_MOUNTING;
    s_worker_busy = true;
    usb_visibility(true, false, false, false, false);
    lv_label_set_text(s_usb.info_lbl, "正在挂载 U 盘…");
    if (xTaskCreate(usb_mount_task, "usb_mount", USB_WORKER_STACK, NULL,
                    USB_WORKER_PRIO, NULL) != pdPASS) {
        s_worker_busy = false;
        s_usb.state = USB_PAGE_SOURCE;
        usb_visibility(true, false, true, false, false);
        lv_label_set_text(s_usb.info_lbl, "挂载任务创建失败");
    }
}

static void unmount_btn_cb(lv_event_t *event)
{
    (void)event;
    if (!s_usb.active || s_worker_busy) {
        return;
    }
    if (s_usb.state != USB_PAGE_MOUNTED) {
        return;
    }
    lv_label_set_text(s_usb.info_lbl, "正在卸载 U 盘…");
    usb_visibility(true, false, false, false, false);
    usb_request_teardown();
}

/* ------------------------------------------------------------------ */
/* Type-C status poll (LVGL thread; ~1-2 ms I2C reads)                 */
/* ------------------------------------------------------------------ */

static const char *type_c_role_zh(bsp_type_c_role_t role)
{
    switch (role) {
    case BSP_TYPE_C_ROLE_SOURCE: return "Source(主机,可挂载 U 盘)";
    case BSP_TYPE_C_ROLE_SINK:   return "Sink(设备模式)";
    case BSP_TYPE_C_ROLE_DRP:    return "DRP(角色协商中)";
    default:                     return "已禁用";
    }
}

static void usb_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_usb.active) {
        return;
    }
    /* Review item 7: the USB host owns the FUSB303B while running; the
     * poll pauses until teardown completes. */
    if (s_host_running || s_worker_busy ||
            s_usb.state == USB_PAGE_MOUNTED ||
            s_usb.state == USB_PAGE_MOUNTING) {
        return;
    }

    bsp_type_c_status_t st = {0};
    if (bsp_type_c_get_status(&st, false) != ESP_OK) {
        lv_label_set_text(s_usb.info_lbl, "Type-C 状态读取失败");
        usb_visibility(true, false, false, false, false);
        usb_set_status_icon(0);
        return;
    }

    if (!st.attached) {
        if (s_usb.state != USB_PAGE_IDLE) {
            s_usb.state = USB_PAGE_IDLE;
            usb_visibility(true, false, false, false, false);
        }
        lv_label_set_text(s_usb.info_lbl,
                          "未检测到连接\n\n插入 U 盘或连接主机后自动识别");
        usb_set_status_icon(0);
        return;
    }

    const char *orientation = st.orientation == 1 ? "CC1" :
                              st.orientation == 2 ? "CC2" : "未知";
    lv_label_set_text_fmt(s_usb.info_lbl,
                          "连接状态:已连接\n角色:%s\nVBUS:%s\n方向:%s",
                          type_c_role_zh(st.role),
                          st.vbus_ok ? "正常" : "异常",
                          orientation);

    if (st.role == BSP_TYPE_C_ROLE_SOURCE) {
        if (s_usb.state != USB_PAGE_SOURCE) {
            s_usb.state = USB_PAGE_SOURCE;
            usb_visibility(true, false, true, false, false);
        }
        usb_set_status_icon(1);
    } else if (st.role == BSP_TYPE_C_ROLE_SINK) {
        if (s_usb.state != USB_PAGE_SINK) {
            s_usb.state = USB_PAGE_SINK;
            usb_visibility(true, true, false, false, false);
        }
        usb_set_status_icon(2);
    } else {
        /* DRP toggling while attached: keep the info text, no actions. */
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
    if (s_host_running || s_worker_busy) {
        usb_request_teardown();
    }
}

lv_obj_t *app_usb_create(void)
{
    memset(&s_usb, 0, sizeof(s_usb));
    s_usb.active = true;
    s_usb.state = USB_PAGE_IDLE;
    s_teardown_requested = false;

    lv_obj_t *content = NULL;
    lv_obj_t *root = ui_app_scaffold("USB OTG", &content);
    lv_obj_add_event_cb(root, usb_delete_cb, LV_EVENT_DELETE, NULL);

    s_usb.info_lbl = lv_label_create(content);
    /* Width stays clear of the unmount button (x=330) shown when mounted. */
    lv_obj_set_width(s_usb.info_lbl, 320);
    lv_obj_set_pos(s_usb.info_lbl, 2, 4);
    lv_label_set_text(s_usb.info_lbl, "正在检测 Type-C 状态…");

    s_usb.sink_lbl = lv_label_create(content);
    lv_obj_set_width(s_usb.sink_lbl, 448);
    lv_obj_set_pos(s_usb.sink_lbl, 2, 110);
    lv_obj_set_style_text_color(s_usb.sink_lbl,
                                lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_label_set_text(s_usb.sink_lbl,
                      "设备模式(Device)\n\n"
                      "当前由对端主机为本设备供电。\n"
                      "将 Type-C 口连接 PC 后,本机可枚举为\n"
                      "USB CDC 串口设备。");

    s_usb.mount_btn = lv_button_create(content);
    lv_obj_set_size(s_usb.mount_btn, 200, 48);
    lv_obj_set_pos(s_usb.mount_btn, 128, 120);
    lv_obj_add_event_cb(s_usb.mount_btn, mount_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mount_lbl = lv_label_create(s_usb.mount_btn);
    lv_label_set_text(mount_lbl, "挂载 U 盘");
    lv_obj_center(mount_lbl);

    s_usb.unmount_btn = lv_button_create(content);
    lv_obj_set_size(s_usb.unmount_btn, 120, 40);
    lv_obj_set_pos(s_usb.unmount_btn, 330, 0);
    lv_obj_add_event_cb(s_usb.unmount_btn, unmount_btn_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *unmount_lbl = lv_label_create(s_usb.unmount_btn);
    lv_label_set_text(unmount_lbl, "卸载");
    lv_obj_center(unmount_lbl);

    s_usb.list = lv_list_create(content);
    lv_obj_set_size(s_usb.list, 448, 300);
    lv_obj_set_pos(s_usb.list, 0, 48);
    lv_obj_set_style_bg_color(s_usb.list, lv_color_hex(UI_COLOR_SURFACE), 0);

    usb_visibility(true, false, false, false, false);

    s_usb.poll_timer = lv_timer_create(usb_poll_cb, USB_POLL_MS, NULL);
    return root;
}
