/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "bsp/esp-bsp.h"

typedef struct {
    esp_err_t register_error;
    esp_err_t info_error;
    esp_err_t unregister_error;
    unsigned register_calls;
    unsigned info_calls;
    unsigned unregister_calls;
    bool registered;
    const char *partition_label;
} spiffs_mock_t;

static const char *TAG = "candis_storage_test";
static spiffs_mock_t s_spiffs;

#define TEST_CHECK(condition) do {                                      \
        if (!(condition)) {                                             \
            ESP_LOGE(TAG, "check failed: %s (%s:%d)", #condition,      \
                     __FILE__, __LINE__);                               \
            abort();                                                    \
        }                                                               \
    } while (0)

static bool labels_equal(const char *left, const char *right)
{
    return left == NULL || right == NULL ?
           left == right : strcmp(left, right) == 0;
}

static void reset_spiffs_mock(esp_err_t register_error,
                              esp_err_t info_error,
                              esp_err_t unregister_error)
{
    s_spiffs = (spiffs_mock_t) {
        .register_error = register_error,
        .info_error = info_error,
        .unregister_error = unregister_error,
    };
}

esp_err_t __wrap_esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf)
{
    TEST_CHECK(conf != NULL);
    ++s_spiffs.register_calls;
    if (s_spiffs.register_error != ESP_OK) {
        return s_spiffs.register_error;
    }
    if (s_spiffs.registered) {
        return ESP_ERR_INVALID_STATE;
    }
    s_spiffs.registered = true;
    s_spiffs.partition_label = conf->partition_label;
    return ESP_OK;
}

esp_err_t __wrap_esp_spiffs_info(const char *partition_label,
                                 size_t *total_bytes,
                                 size_t *used_bytes)
{
    ++s_spiffs.info_calls;
    TEST_CHECK(labels_equal(partition_label, s_spiffs.partition_label));
    TEST_CHECK(total_bytes != NULL);
    TEST_CHECK(used_bytes != NULL);
    if (!s_spiffs.registered) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_spiffs.info_error != ESP_OK) {
        return s_spiffs.info_error;
    }
    *total_bytes = 4096;
    *used_bytes = 1024;
    return ESP_OK;
}

esp_err_t __wrap_esp_vfs_spiffs_unregister(const char *partition_label)
{
    ++s_spiffs.unregister_calls;
    TEST_CHECK(labels_equal(partition_label, s_spiffs.partition_label));
    if (!s_spiffs.registered) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_spiffs.unregister_error != ESP_OK) {
        return s_spiffs.unregister_error;
    }
    s_spiffs.registered = false;
    return ESP_OK;
}

static void test_register_failure_stops_mount(void)
{
    reset_spiffs_mock(ESP_ERR_NO_MEM, ESP_OK, ESP_OK);

    TEST_CHECK(bsp_spiffs_mount() == ESP_ERR_NO_MEM);
    TEST_CHECK(s_spiffs.register_calls == 1);
    TEST_CHECK(s_spiffs.info_calls == 0);
    TEST_CHECK(s_spiffs.unregister_calls == 0);
    TEST_CHECK(!s_spiffs.registered);
}

static void test_successful_mount_stays_registered(void)
{
    reset_spiffs_mock(ESP_OK, ESP_OK, ESP_OK);

    TEST_CHECK(bsp_spiffs_mount() == ESP_OK);
    TEST_CHECK(s_spiffs.register_calls == 1);
    TEST_CHECK(s_spiffs.info_calls == 1);
    TEST_CHECK(s_spiffs.unregister_calls == 0);
    TEST_CHECK(s_spiffs.registered);

    TEST_CHECK(bsp_spiffs_unmount() == ESP_OK);
    TEST_CHECK(s_spiffs.unregister_calls == 1);
    TEST_CHECK(!s_spiffs.registered);
}

static void test_info_failure_rolls_back_mount(void)
{
    reset_spiffs_mock(ESP_OK, ESP_ERR_INVALID_STATE, ESP_OK);

    TEST_CHECK(bsp_spiffs_mount() == ESP_ERR_INVALID_STATE);
    TEST_CHECK(s_spiffs.register_calls == 1);
    TEST_CHECK(s_spiffs.info_calls == 1);
    TEST_CHECK(s_spiffs.unregister_calls == 1);
    TEST_CHECK(!s_spiffs.registered);
}

static void test_rollback_failure_preserves_info_error(void)
{
    reset_spiffs_mock(ESP_OK, ESP_ERR_INVALID_STATE, ESP_FAIL);

    TEST_CHECK(bsp_spiffs_mount() == ESP_ERR_INVALID_STATE);
    TEST_CHECK(s_spiffs.register_calls == 1);
    TEST_CHECK(s_spiffs.info_calls == 1);
    TEST_CHECK(s_spiffs.unregister_calls == 1);
    TEST_CHECK(s_spiffs.registered);
}

static void test_mount_can_retry_after_rollback(void)
{
    reset_spiffs_mock(ESP_OK, ESP_ERR_INVALID_STATE, ESP_OK);

    TEST_CHECK(bsp_spiffs_mount() == ESP_ERR_INVALID_STATE);
    TEST_CHECK(!s_spiffs.registered);

    s_spiffs.info_error = ESP_OK;
    TEST_CHECK(bsp_spiffs_mount() == ESP_OK);
    TEST_CHECK(s_spiffs.register_calls == 2);
    TEST_CHECK(s_spiffs.info_calls == 2);
    TEST_CHECK(s_spiffs.unregister_calls == 1);
    TEST_CHECK(s_spiffs.registered);

    TEST_CHECK(bsp_spiffs_unmount() == ESP_OK);
    TEST_CHECK(s_spiffs.unregister_calls == 2);
    TEST_CHECK(!s_spiffs.registered);
}

static void test_unmount_preserves_unregister_error(void)
{
    reset_spiffs_mock(ESP_OK, ESP_OK, ESP_OK);

    TEST_CHECK(bsp_spiffs_mount() == ESP_OK);
    s_spiffs.unregister_error = ESP_ERR_TIMEOUT;
    TEST_CHECK(bsp_spiffs_unmount() == ESP_ERR_TIMEOUT);
    TEST_CHECK(s_spiffs.unregister_calls == 1);
    TEST_CHECK(s_spiffs.registered);
}

void app_main(void)
{
    test_register_failure_stops_mount();
    test_successful_mount_stays_registered();
    test_info_failure_rolls_back_mount();
    test_rollback_failure_preserves_info_error();
    test_mount_can_retry_after_rollback();
    test_unmount_preserves_unregister_error();
    ESP_LOGI(TAG, "SPIFFS lifecycle tests passed");
}
