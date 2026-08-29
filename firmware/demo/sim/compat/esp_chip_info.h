/*
 * Candis-S31 simulator - esp_chip_info shim.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Model enum values follow the IDF naming; only the S31 row is real. */
typedef enum {
    CHIP_ESP32 = 1,
    CHIP_ESP32S2,
    CHIP_ESP32S3,
    CHIP_ESP32C3,
    CHIP_ESP32S31 = 31,
} esp_chip_model_t;

typedef struct {
    esp_chip_model_t model;
    unsigned int features;
    unsigned char cores;
    unsigned char revision;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out_info);

#ifdef __cplusplus
}
#endif
