/*
 * Candis-S31 simulator - misc ESP API shims + demo_board + NVS RAM store.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "bsp/esp-bsp.h"
#include "demo_board.h"
#include "nvs.h"

/* ---------------- esp_system ---------------- */

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    ESP_LOGW("sim", "esp_restart requested; exiting simulator process");
    exit(0);
}

/* ---------------- esp_chip_info ---------------- */

void esp_chip_info(esp_chip_info_t *out_info)
{
    if (out_info == NULL) {
        return;
    }
    out_info->model = CHIP_ESP32S31;
    out_info->features = 0;
    out_info->cores = 2;
    out_info->revision = 0;
}

/* ---------------- esp_idf_version ---------------- */

const char *esp_get_idf_version(void)
{
    return "v6.1-rc1 (sim)";
}

/* ---------------- esp_flash ---------------- */

esp_err_t esp_flash_get_size(void *chip, uint32_t *out_size_bytes)
{
    (void)chip;
    if (out_size_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_size_bytes = 16u * 1024u * 1024u; /* W25Q128 */
    return ESP_OK;
}

/* ---------------- BSP display/rtc extras ---------------- */

static int s_brightness = 80;

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent < 0 || brightness_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_brightness = brightness_percent;
    return ESP_OK;
}

int sim_display_get_brightness(void)
{
    return s_brightness;
}

esp_err_t bsp_rtc_set_time(const bsp_rtc_time_t *time)
{
    /* The sim RTC follows the host clock; setting it is acknowledged but
     * not applied (the host clock is the source of truth). */
    (void)time;
    return ESP_OK;
}

esp_err_t bsp_rtc_get_status(bsp_rtc_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    status->time_valid = true;
    return ESP_OK;
}

esp_err_t bsp_pmic_get_input_current_limit(uint16_t *out_ma)
{
    if (out_ma == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_ma = 2000;
    return ESP_OK;
}

/* ---------------- demo_board ---------------- */

static demo_settings_t s_settings = {
    .brightness = 80,
    .volume = 60,
    .mic_gain_db = 24,
    .screen_timeout_s = 30,
};

esp_err_t demo_board_init(void)
{
    return ESP_OK;
}

demo_settings_t *demo_settings(void)
{
    return &s_settings;
}

void demo_settings_save(void)
{
    /* Process-local settings; nothing to persist in the sim. */
}

/* ---------------- NVS RAM blob store ---------------- */

typedef struct nvs_entry {
    char ns[16];
    char key[16];
    uint8_t *data;
    size_t length;
    struct nvs_entry *next;
} nvs_entry_t;

static nvs_entry_t *s_nvs_entries;
static uint32_t s_nvs_next_handle = 1;

esp_err_t nvs_open(const char *ns_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (ns_name == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The handle doubles as the namespace pointer for this minimal shim. */
    char *ns_copy = strdup(ns_name);
    if (ns_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    (void)s_nvs_next_handle;
    *out_handle = (nvs_handle_t)(uintptr_t)ns_copy;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    free((void *)(uintptr_t)handle);
}

static nvs_entry_t *nvs_find(const char *ns, const char *key)
{
    for (nvs_entry_t *e = s_nvs_entries; e != NULL; e = e->next) {
        if (strcmp(e->ns, ns) == 0 && strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    const char *ns = (const char *)(uintptr_t)handle;
    if (ns == NULL || key == NULL || (value == NULL && length > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_entry_t *e = nvs_find(ns, key);
    if (e == NULL) {
        e = calloc(1, sizeof(*e));
        if (e == NULL) {
            return ESP_ERR_NO_MEM;
        }
        snprintf(e->ns, sizeof(e->ns), "%s", ns);
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->next = s_nvs_entries;
        s_nvs_entries = e;
    }
    uint8_t *copy = realloc(e->data, length);
    if (copy == NULL && length > 0) {
        return ESP_ERR_NO_MEM;
    }
    e->data = copy;
    e->length = length;
    if (length > 0) {
        memcpy(e->data, value, length);
    }
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    const char *ns = (const char *)(uintptr_t)handle;
    if (ns == NULL || key == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_entry_t *e = nvs_find(ns, key);
    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out_value == NULL || *length < e->length) {
        *length = e->length;
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(out_value, e->data, e->length);
    *length = e->length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key,
                      const char *value)
{
    return nvs_set_blob(handle, key, value,
                        value != NULL ? strlen(value) + 1 : 0);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value,
                      size_t *length)
{
    return nvs_get_blob(handle, key, out_value, length);
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    const char *ns = (const char *)(uintptr_t)handle;
    nvs_entry_t **prev = &s_nvs_entries;
    for (nvs_entry_t *e = s_nvs_entries; e != NULL; e = e->next) {
        if (strcmp(e->ns, ns) == 0 && strcmp(e->key, key) == 0) {
            *prev = e->next;
            free(e->data);
            free(e);
            return ESP_OK;
        }
        prev = &e->next;
    }
    return ESP_ERR_NOT_FOUND;
}
