/*
 * Candis-S31 OV5640 RAW8 visual validation.
 *
 * The sensor is initialized through the board's existing esp_video path so
 * the vendored OV5640 register table remains the single timing source. The
 * esp_video DVP controller is then released and replaced with the public
 * ESP-IDF esp_driver_cam controller configured for true 8-bit RAW capture.
 * No upstream component source is modified.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "camera_visual.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/cam_types.h"
#include "sdkconfig.h"

static const char *TAG = "camera_raw_test";

#define RAW_WIDTH               320U
#define RAW_HEIGHT              240U
#define RAW_IMAGE_BYTES         (RAW_WIDTH * RAW_HEIGHT)
#define RAW_STRIDE_BYTES        (RAW_WIDTH * 2U)
#define RAW_FRAME_BYTES         (RAW_IMAGE_BYTES * 2U)
#define RAW_CAPTURE_BUFFERS     4U
#define RAW_AUTO_FRAMES         12U
#define RAW_RECEIVE_TIMEOUT_MS  3000U
#define RAW_SENSOR_ADDR         0x3cU
#define RAW_TASK_STACK          8192U
#define RAW_TASK_PRIORITY       4U

typedef struct {
    esp_cam_ctlr_handle_t controller;
    QueueHandle_t free_buffers;
    QueueHandle_t finished_frames;
    uint8_t *frames[RAW_CAPTURE_BUFFERS];
    volatile uint32_t get_requests;
    volatile uint32_t finished_transactions;
    bool enabled;
    bool started;
    bool board_started;
    bool esp_video_released;
} raw_session_t;

static uint32_t frame_hash(const uint8_t *data, size_t length,
                           uint8_t *minimum, uint8_t *maximum)
{
    uint32_t hash = 2166136261U;
    uint8_t low = UINT8_MAX;
    uint8_t high = 0;
    for (size_t offset = 0; offset < length; offset += 64U) {
        const uint8_t value = data[offset];
        if (value < low) {
            low = value;
        }
        if (value > high) {
            high = value;
        }
        hash ^= value;
        hash *= 16777619U;
    }
    *minimum = low;
    *maximum = high;
    return hash;
}

static bool IRAM_ATTR raw_get_new_transaction(esp_cam_ctlr_handle_t handle,
                                               esp_cam_ctlr_trans_t *trans,
                                               void *user_data)
{
    (void)handle;
    raw_session_t *session = user_data;
    ++session->get_requests;
    void *buffer = NULL;
    BaseType_t task_woken = pdFALSE;
    if (xQueueReceiveFromISR(session->free_buffers, &buffer,
                             &task_woken) == pdTRUE) {
        trans->buffer = buffer;
        trans->buflen = RAW_FRAME_BYTES;
    }
    return task_woken == pdTRUE;
}

static bool IRAM_ATTR raw_transaction_finished(esp_cam_ctlr_handle_t handle,
                                                esp_cam_ctlr_trans_t *trans,
                                                void *user_data)
{
    (void)handle;
    raw_session_t *session = user_data;
    ++session->finished_transactions;
    BaseType_t task_woken = pdFALSE;
    if (xQueueSendFromISR(session->finished_frames, trans,
                          &task_woken) != pdTRUE) {
        void *buffer = trans->buffer;
        (void)xQueueSendFromISR(session->free_buffers, &buffer, &task_woken);
    }
    return task_woken == pdTRUE;
}

static esp_err_t sensor_write_raw8(uint8_t *format_ctrl, uint8_t *format_mux,
                                   uint8_t *system_ctrl)
{
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RAW_SENSOR_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t device = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &device),
                        TAG, "add OV5640 SCCB device failed");

    static const uint8_t raw_regs[][3] = {
        {0x38, 0x08, 0x01}, /* DVP output width: 320 */
        {0x38, 0x09, 0x40},
        {0x38, 0x0a, 0x00}, /* DVP output height: 240 */
        {0x38, 0x0b, 0xf0},
        {0x30, 0x34, 0x1a}, /* SC PLL CTRL0: 10-bit output mode */
        {0x43, 0x00, 0x00},
        {0x50, 0x1f, 0x03}, /* esp32-camera sensor_fmt_raw: RAW after DPC */
        {0x30, 0x08, 0x02}, /* DVP stream on / leave software power-down */
    };
    esp_err_t error = ESP_OK;
    for (size_t index = 0; index < sizeof(raw_regs) / sizeof(raw_regs[0]);
            ++index) {
        error = i2c_master_transmit(device, raw_regs[index],
                                    sizeof(raw_regs[index]), 100);
        if (error != ESP_OK) {
            break;
        }
    }

    if (error == ESP_OK) {
        const uint8_t ctrl_addr[] = {0x43, 0x00};
        error = i2c_master_transmit_receive(device, ctrl_addr,
                                            sizeof(ctrl_addr),
                                            format_ctrl, 1, 100);
    }
    if (error == ESP_OK) {
        const uint8_t mux_addr[] = {0x50, 0x1f};
        error = i2c_master_transmit_receive(device, mux_addr,
                                            sizeof(mux_addr),
                                            format_mux, 1, 100);
    }
    if (error == ESP_OK) {
        const uint8_t system_addr[] = {0x30, 0x08};
        error = i2c_master_transmit_receive(device, system_addr,
                                            sizeof(system_addr),
                                            system_ctrl, 1, 100);
    }

    const esp_err_t remove_error = i2c_master_bus_rm_device(device);
    if (error == ESP_OK) {
        error = remove_error;
    }
    return error;
}

static esp_err_t keep_sensor_running_after_video_release(void)
{
    /* esp_video deinit resets these two GPIOs. Hold their active levels so
     * deleting the upstream sensor handle cannot pulse reset or power-down. */
    ESP_RETURN_ON_ERROR(gpio_hold_en(BSP_CAMERA_PWDN), TAG,
                        "hold camera PWDN failed");
    ESP_RETURN_ON_ERROR(gpio_hold_en(BSP_CAMERA_RST), TAG,
                        "hold camera RESET failed");

    const esp_err_t deinit_error =
        esp_video_deinit_with_flags(ESP_VIDEO_INIT_FLAGS_DVP);

    gpio_set_level(BSP_CAMERA_PWDN, 0);
    gpio_set_level(BSP_CAMERA_RST, 1);
    const esp_err_t pwdn_release = gpio_hold_dis(BSP_CAMERA_PWDN);
    const esp_err_t reset_release = gpio_hold_dis(BSP_CAMERA_RST);
    const esp_err_t pwdn_output = gpio_set_direction(BSP_CAMERA_PWDN,
                                                     GPIO_MODE_OUTPUT);
    const esp_err_t reset_output = gpio_set_direction(BSP_CAMERA_RST,
                                                      GPIO_MODE_OUTPUT);
    if (deinit_error != ESP_OK) {
        return deinit_error;
    }
    if (pwdn_release != ESP_OK) {
        return pwdn_release;
    }
    if (reset_release != ESP_OK) {
        return reset_release;
    }
    if (pwdn_output != ESP_OK) {
        return pwdn_output;
    }
    return reset_output;
}

static esp_err_t raw_session_start(raw_session_t *session)
{
    *session = (raw_session_t){0};

    ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG,
                        "BSP camera start failed");
    session->board_started = true;

    /* open() performs the lazy esp_video sensor format initialization. */
    const int video_fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (video_fd < 0) {
        ESP_LOGE(TAG, "open %s failed errno=%d", BSP_CAMERA_DEVICE, errno);
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t workaround_error = bsp_camera_apply_workaround();
    close(video_fd);
    ESP_RETURN_ON_ERROR(workaround_error, TAG,
                        "post-open sensor workaround failed");

    ESP_RETURN_ON_ERROR(keep_sensor_running_after_video_release(), TAG,
                        "release esp_video DVP failed");
    session->esp_video_released = true;

    uint8_t format_ctrl = 0xff;
    uint8_t format_mux = 0xff;
    uint8_t system_ctrl = 0xff;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t raw_error = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        raw_error = sensor_write_raw8(&format_ctrl, &format_mux, &system_ctrl);
        if (raw_error == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "RAW10 SCCB setup attempt=%u error=%s",
                 attempt, esp_err_to_name(raw_error));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(raw_error, TAG, "set OV5640 RAW10 failed");
    const bool sensor_ok = format_mux == 0x03 && system_ctrl == 0x02;
    ESP_LOGI(TAG,
             "CAMERA_RAW_SENSOR 4300=0x%02x 501f=0x%02x 3008=0x%02x status=%s",
             format_ctrl, format_mux, system_ctrl,
             sensor_ok ? "PASS" : "FAIL");
    if (!sensor_ok) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (format_ctrl != 0x00) {
        ESP_LOGW(TAG, "clone couples 0x501f=0x03 to 0x4300=0x%02x; "
                 "accept RAW mux and verify actual frame bytes", format_ctrl);
    }

    static const esp_cam_ctlr_dvp_pin_config_t pins = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            BSP_CAMERA_D1, BSP_CAMERA_D0, BSP_CAMERA_D2, BSP_CAMERA_D4,
            BSP_CAMERA_D3, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7,
        },
        .vsync_io = BSP_CAMERA_VSYNC,
        .de_io = BSP_CAMERA_HSYNC,
        .pclk_io = BSP_CAMERA_PCLK,
        .xclk_io = GPIO_NUM_NC,
    };
    const esp_cam_ctlr_dvp_config_t config = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = RAW_WIDTH,
        .v_res = RAW_HEIGHT,
        .input_data_color_type = CAM_CTLR_COLOR_RGB565,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .cam_data_width = 8,
        .bit_swap_en = false,
        .byte_swap_en = false,
        .bk_buffer_dis = true,
        .pin_dont_init = false,
        .pic_format_jpeg = false,
        .external_xtal = true,
        .dma_burst_size = 64,
        .xclk_freq = 0,
        .pin = &pins,
    };
    ESP_RETURN_ON_ERROR(esp_cam_new_dvp_ctlr(&config, &session->controller),
                        TAG, "create RAW10 DVP controller failed");

    session->free_buffers = xQueueCreate(RAW_CAPTURE_BUFFERS, sizeof(void *));
    session->finished_frames =
        xQueueCreate(RAW_CAPTURE_BUFFERS, sizeof(esp_cam_ctlr_trans_t));
    if (session->free_buffers == NULL || session->finished_frames == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < RAW_CAPTURE_BUFFERS; ++index) {
        session->frames[index] = heap_caps_aligned_alloc(
            64, RAW_FRAME_BYTES,
            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM |
                MALLOC_CAP_CACHE_ALIGNED);
        if (session->frames[index] == NULL) {
            return ESP_ERR_NO_MEM;
        }
        void *buffer = session->frames[index];
        if (xQueueSend(session->free_buffers, &buffer, 0) != pdTRUE) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    const esp_cam_ctlr_evt_cbs_t callbacks = {
        .on_get_new_trans = raw_get_new_transaction,
        .on_trans_finished = raw_transaction_finished,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(
                            session->controller, &callbacks, session),
                        TAG, "register RAW8 DVP callbacks failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(session->controller), TAG,
                        "enable RAW8 DVP controller failed");
    session->enabled = true;
    return ESP_OK;
}

static bool raw_session_stop(raw_session_t *session)
{
    bool ok = true;
    if (session->started) {
        if (esp_cam_ctlr_stop(session->controller) != ESP_OK) {
            ok = false;
        }
        session->started = false;
    }
    if (session->enabled) {
        if (esp_cam_ctlr_disable(session->controller) != ESP_OK) {
            ok = false;
        }
        session->enabled = false;
    }
    if (session->controller != NULL) {
        if (esp_cam_ctlr_del(session->controller) != ESP_OK) {
            ok = false;
        }
        session->controller = NULL;
    }
    for (size_t index = 0; index < RAW_CAPTURE_BUFFERS; ++index) {
        heap_caps_free(session->frames[index]);
        session->frames[index] = NULL;
    }
    if (session->free_buffers != NULL) {
        vQueueDelete(session->free_buffers);
        session->free_buffers = NULL;
    }
    if (session->finished_frames != NULL) {
        vQueueDelete(session->finished_frames);
        session->finished_frames = NULL;
    }

    if (session->board_started) {
        const esp_err_t stop_error = bsp_camera_stop();
        /* esp_video was deliberately released before the direct controller
         * was created; bsp_camera_stop still stops XCLK and powers the rail. */
        if (stop_error != ESP_OK && session->esp_video_released) {
            ESP_LOGW(TAG, "expected BSP esp_video re-deinit result: %s",
                     esp_err_to_name(stop_error));
        } else if (stop_error != ESP_OK) {
            ok = false;
        }
        session->board_started = false;
    }
    return ok;
}

static esp_err_t raw_receive(raw_session_t *session, uint32_t frame_number,
                             uint32_t *hash)
{
    esp_cam_ctlr_trans_t transaction = {0};
    if (xQueueReceive(session->finished_frames, &transaction,
                      pdMS_TO_TICKS(RAW_RECEIVE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG,
                 "CAMERA_RAW_FRAME frame=%u reason=timeout get=%u done=%u status=FAIL",
                 (unsigned)frame_number, (unsigned)session->get_requests,
                 (unsigned)session->finished_transactions);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_OK;


    uint8_t minimum = 0;
    uint8_t maximum = 0;
    if (transaction.received_size != RAW_FRAME_BYTES) {
        ESP_LOGE(TAG, "CAMERA_RAW_FRAME frame=%u bytes=%u expected=%u status=FAIL",
                 (unsigned)frame_number, (unsigned)transaction.received_size,
                 (unsigned)RAW_FRAME_BYTES);
        result = ESP_ERR_INVALID_SIZE;
    } else {
        *hash = frame_hash(transaction.buffer + 1U, RAW_FRAME_BYTES - 1U,
                           &minimum, &maximum);
        if (minimum == maximum) {
            ESP_LOGE(TAG,
                     "CAMERA_RAW_FRAME frame=%u bytes=%u hash=0x%08x range=%u..%u status=FAIL",
                     (unsigned)frame_number,
                     (unsigned)transaction.received_size,
                     (unsigned)*hash, minimum, maximum);
            result = ESP_ERR_INVALID_RESPONSE;
        } else {
            if (!camera_visual_publish_raw8_lane1(transaction.buffer,
                                                  RAW_FRAME_BYTES,
                                                  RAW_STRIDE_BYTES)) {
                ESP_LOGW(TAG, "CAMERA_RAW_FRAME frame=%u visual_drop=1",
                         (unsigned)frame_number);
            }
            ESP_LOGI(TAG,
                     "CAMERA_RAW_FRAME frame=%u bytes=%u hash=0x%08x range=%u..%u status=PASS",
                     (unsigned)frame_number,
                     (unsigned)transaction.received_size,
                     (unsigned)*hash, minimum, maximum);
        }
    }

    void *buffer = transaction.buffer;
    if (xQueueSend(session->free_buffers, &buffer, 0) != pdTRUE) {
        ESP_LOGE(TAG, "CAMERA_RAW_FRAME frame=%u reason=recycle status=FAIL",
                 (unsigned)frame_number);
        result = ESP_ERR_INVALID_STATE;
    }
    return result;
}

static void camera_raw_task(void *arg)
{
    (void)arg;
    raw_session_t session;
    const esp_err_t start_error = raw_session_start(&session);
    if (start_error != ESP_OK) {
        (void)raw_session_stop(&session);
        ESP_LOGE(TAG, "CAMERA_RAW_SUMMARY status=FAIL stage=start error=%s",
                 esp_err_to_name(start_error));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "CAMERA_RAW_STAGE visual_init begin");
    const esp_err_t visual_error = camera_visual_start();
    ESP_LOGI(TAG, "CAMERA_RAW_STAGE visual_init end result=%s",
             esp_err_to_name(visual_error));
    if (visual_error != ESP_OK) {
        (void)raw_session_stop(&session);
        ESP_LOGE(TAG,
                 "CAMERA_RAW_SUMMARY status=FAIL stage=display_init error=%s",
                 esp_err_to_name(visual_error));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CAMERA_RAW_BOARD_INIT status=PASS display=on");

    const esp_err_t capture_error = esp_cam_ctlr_start(session.controller);
    if (capture_error != ESP_OK) {
        camera_visual_set_error("RAW10 camera capture could not start");
        (void)raw_session_stop(&session);
        ESP_LOGE(TAG,
                 "CAMERA_RAW_SUMMARY status=FAIL stage=capture_start error=%s",
                 esp_err_to_name(capture_error));
        vTaskDelete(NULL);
        return;
    }
    session.started = true;


    camera_visual_set_streaming(true);
    bool auto_ok = true;
    bool motion = false;
    uint32_t previous_hash = 0;
    for (uint32_t frame = 1; frame <= RAW_AUTO_FRAMES; ++frame) {
        uint32_t hash = 0;
        const esp_err_t error = raw_receive(&session, frame, &hash);
        if (error != ESP_OK) {
            auto_ok = false;
            break;
        }
        if (frame > 1 && hash != previous_hash) {
            motion = true;
        }
        previous_hash = hash;
    }
    auto_ok = auto_ok && motion;
    camera_visual_set_auto_state(auto_ok, 1, RAW_AUTO_FRAMES);
    ESP_LOGI(TAG, "CAMERA_RAW_AUTO status=%s frames=%u motion=%d",
             auto_ok ? "PASS" : "FAIL", RAW_AUTO_FRAMES, motion);

    if (auto_ok) {
        uint32_t frame_number = RAW_AUTO_FRAMES;
        while (camera_visual_get_decision() == CAMERA_VISUAL_PENDING) {
            uint32_t hash;
            if (raw_receive(&session, ++frame_number, &hash) != ESP_OK) {
                auto_ok = false;
                camera_visual_set_error("RAW10 live preview failed");
                break;
            }
        }
    } else {
        camera_visual_set_error("RAW10 automatic checks failed");
    }

    camera_visual_set_streaming(false);
    const camera_visual_decision_t decision = camera_visual_get_decision();
    const bool stop_ok = raw_session_stop(&session);
    const bool final_pass = auto_ok && stop_ok &&
                            decision == CAMERA_VISUAL_PASS;
    ESP_LOGI(TAG,
             "CAMERA_RAW_SUMMARY status=%s visual=%s bayer=%u auto=%s stop=%s",
             final_pass ? "PASS" : "FAIL",
             decision == CAMERA_VISUAL_PASS ? "pass" :
             decision == CAMERA_VISUAL_FAIL ? "fail" : "missing",
             (unsigned)camera_visual_get_bayer(),
             auto_ok ? "PASS" : "FAIL", stop_ok ? "PASS" : "FAIL");
    vTaskDelete(NULL);
}

static void idle_forever(void)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "CAMERA_RAW_BEGIN source=github:espressif/esp32-camera "
             "format=RAW10_DPC dma_bytes=%u image_bytes=%u "
             "visual_confirmation=required",
             (unsigned)RAW_FRAME_BYTES, (unsigned)RAW_IMAGE_BYTES);

    ESP_LOGI(TAG, "CAMERA_RAW_STAGE board_init begin");
    const esp_err_t board_error = bsp_board_init();
    ESP_LOGI(TAG, "CAMERA_RAW_STAGE board_init end result=%s",
             esp_err_to_name(board_error));
    if (board_error != ESP_OK) {
        ESP_LOGE(TAG, "CAMERA_RAW_SUMMARY status=FAIL stage=board_init error=%s",
                 esp_err_to_name(board_error));
        idle_forever();
    }

    if (xTaskCreate(camera_raw_task, "camera_raw", RAW_TASK_STACK,
                    NULL, RAW_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "RAW10 camera task could not start");
        ESP_LOGE(TAG, "CAMERA_RAW_SUMMARY status=FAIL stage=task_create");
        idle_forever();
    }
}
