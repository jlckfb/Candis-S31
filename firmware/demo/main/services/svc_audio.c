/*
 * Candis-S31 watch demo - audio service implementation.
 *
 * One audio task (prio 5, stack 8192, command queue depth 6) owns the
 * ES8389 codec; recording and playback are mutually exclusive. All codec,
 * I2C and SD I/O happens on this task, never on the LVGL thread. Public
 * API calls send a command and wait for a bounded acknowledgement. The
 * response object is reference-counted between caller and audio task, so a
 * caller timeout cannot leave the task writing into a dead stack frame.
 * Streaming runs inline inside the command handler.
 * Codec ownership: svc_audio_start() creates the BSP speaker/microphone
 * handles once, on the main task, before the network service runs. The I2S
 * TX+RX DMA descriptors (~12.8 KB of MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)
 * are therefore allocated while that pool is still contiguous; lazy
 * per-recording init raced against WiFi/BLE allocations and lost 10/10
 * times on the 2026-08-21 board log. record_run/play_run reuse the
 * persistent handles and fail fast when they are absent; per-stream
 * esp_codec_dev_open()/close() is unchanged and never reallocates DMA.
 *
 * Record path discipline (inherited from the factory firmware, verified on
 * EVT1 boards):
 *  - 16 kHz / 16-bit / stereo, mclk_multiple 256 (BSP policy: use_mclk=false,
 *    no_dac_ref=true on both logical codec devices).
 *  - Open order: speaker first, PA power domain off, then microphone, then
 *    set_out_vol (0 while recording), set_in_gain, then the ES8389 reg 0x72
 *    input route. set_in_gain rewrites 0x72 whole (upper nibble falls back
 *    to the differential default), so the route is re-applied after every
 *    gain change while recording.
 *  - An esp_codec_dev_open() failure still requires esp_codec_dev_close():
 *    open can leave partial state that a retry would inherit.
 *  - The first two RX blocks after open are pipeline garbage: discarded.
 *  - Recording runs with the PA domain off (anti-feedback, lower noise) and
 *    restores it when the capture ends.
 *  - No sample-rate switch / re-open while recording.
 *
 * Routes: LEFT/RIGHT select the MIC1P/MIC2P single-ended input into ADC ch0
 * (0x72 high nibble 0x50 / 0x60) and zero the unused channel in software so
 * the saved WAV is genuinely single-mic; STEREO keeps the BSP default
 * differential pair; DENOISE records both channels and applies basic noise
 * reduction (first-order high-pass DC blocker + noise gate, int16 fixed
 * point). This is honest "Basic NR"; esp-sr S31 NS is a follow-up.
 *
 * Playback accepts PCM16 WAV (1/2 channels, 8/16/44.1/48 kHz), re-opens the
 * speaker at the file rate and mixes everything to mono-duplicated stereo
 * (this board has a single speaker), the factory-verified wav_play path.
 *
 * Event note: on an internal failure the service emits SVC_AUDIO_EV_ERROR
 * and cleans up; PLAY_DONE is additionally emitted on natural EOF and on
 * caller-requested stop, RECORD_DONE only after a successful WAV save.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "svc_audio.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "services/svc_storage.h"

static const char *TAG = "svc_audio";

#define AUDIO_TASK_STACK        8192
#define AUDIO_TASK_PRIO         5
#define AUDIO_QUEUE_DEPTH       6
#define AUDIO_QUEUE_TIMEOUT_MS  500U
#define AUDIO_ACK_TIMEOUT_MS    3000U

/* Record format: 16 kHz / 16-bit / stereo, the factory-verified rate. */
#define REC_SAMPLE_RATE         16000U
#define REC_FRAME_BYTES         4U                    /* 2 ch x 16 bit */
#define REC_IO_BYTES            2048U                 /* factory block size */
#define REC_DISCARD_BLOCKS      2U                    /* pipeline garbage */
#define REC_MAX_SECONDS         60U
#define REC_MAX_BYTES           (REC_MAX_SECONDS * REC_SAMPLE_RATE * REC_FRAME_BYTES)
#define REC_LEVEL_PERIOD_BYTES  (REC_SAMPLE_RATE * REC_FRAME_BYTES / 5U) /* ~5 events/s */
#define REC_DIR_NAME            "recordings"

#define PLAY_IO_BYTES           2048U
#define PLAY_PAUSE_POLL_MS      20U
#define WAV_HEADER_BYTES        44U

/* ES8389 sits at 7-bit 0x10 on the main I2C bus (8-bit write address 0x20
 * inside esp_codec_dev). The input route is patched raw, exactly like the
 * factory's codec_write_input_route(): the esp_codec_dev es8389 driver has
 * no input-select API and set_fs()/gain calls rewrite 0x72 whole. */
#define ES8389_I2C_ADDRESS      0x10
#define ROUTE_REG               0x72
#define ROUTE_MIC1_SINGLE       0x50  /* bits[6:4]=0b101: MIC1P single-ended -> ADC ch0 */
#define ROUTE_MIC2_SINGLE       0x60  /* bits[6:4]=0b110: MIC2P single-ended -> ADC ch0 */
#define ROUTE_STEREO_DEFAULT    0x10  /* bits[6:4]=0b001: differential MIC1P-MIC1N */

/* Basic noise reduction (honest scope: NOT esp-sr NS). A first-order
 * high-pass removes DC offset/drift, then a per-group noise gate zeroes
 * steady low-level noise. Fixed point only; tuned conservatively so speech
 * onsets survive the gate. */
#define DENOISE_HP_COEF_Q15     32604  /* R = 0.995 -> cutoff ~13 Hz @ 16 kHz */
#define DENOISE_GATE_THRESHOLD  260    /* ~ -42 dBFS group peak */
#define DENOISE_GROUP_FRAMES    64     /* 4 ms gate window per channel */

typedef enum {
    AUDIO_MODE_IDLE = 0,
    AUDIO_MODE_RECORDING,
    AUDIO_MODE_SAVING,   /* capture done, WAV write in progress; still busy */
    AUDIO_MODE_PLAYING,
} audio_mode_t;

typedef enum {
    CMD_RECORD_START = 0,
    CMD_RECORD_STOP,
    CMD_SET_GAIN,
    CMD_PLAY_START,
    CMD_PLAY_STOP,
    CMD_PLAY_PAUSE,
    CMD_SET_VOLUME,
} audio_cmd_type_t;

typedef struct {
    SemaphoreHandle_t done;
    atomic_uint refs;
    atomic_bool cancelled;
    atomic_int result;
} audio_ack_t;

typedef struct {
    audio_cmd_type_t type;
    svc_audio_route_t route;
    int gain_db;
    int volume;
    bool pause_on;
    char path[96];
    svc_audio_cb_t cb;
    void *user;
    /* The command and caller each own one reference after a successful
     * enqueue. A caller timeout releases only its reference; the audio task
     * may safely complete the response later and releases the final one. */
    audio_ack_t *ack;
} audio_cmd_t;

typedef struct {
    int32_t hp_acc;    /* Q15 high-pass accumulator */
    int16_t hp_prev;   /* previous raw sample */
} denoise_channel_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_started;

/* Created once by svc_audio_start(); NULL until then and after a failed
 * start. The BSP caches the same instances in its own statics, so these are
 * borrowed pointers - never deleted from this file. */
static esp_codec_dev_handle_t s_speaker_dev;
static esp_codec_dev_handle_t s_mic_dev;

static atomic_int s_mode = ATOMIC_VAR_INIT(AUDIO_MODE_IDLE);

/* Single-task IO buffers: recording and playback never overlap, so one
 * static pair is enough and keeps the 8 KB task stack shallow. */
static uint8_t s_io[PLAY_IO_BYTES > REC_IO_BYTES ? PLAY_IO_BYTES : REC_IO_BYTES];
static int16_t s_expand[PLAY_IO_BYTES / sizeof(int16_t) * 2];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static esp_codec_dev_sample_info_t audio_format(uint32_t sample_rate)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = sample_rate,
        .mclk_multiple = 256,
    };
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static audio_mode_t audio_mode_load(void)
{
    return (audio_mode_t)atomic_load_explicit(&s_mode, memory_order_acquire);
}

static void audio_mode_store(audio_mode_t mode)
{
    atomic_store_explicit(&s_mode, mode, memory_order_release);
}

static audio_ack_t *audio_ack_create(void)
{
    audio_ack_t *ack = pvPortMalloc(sizeof(*ack));
    if (ack == NULL) {
        return NULL;
    }
    ack->done = xSemaphoreCreateBinary();
    if (ack->done == NULL) {
        vPortFree(ack);
        return NULL;
    }
    atomic_init(&ack->refs, 2U);
    atomic_init(&ack->cancelled, false);
    atomic_init(&ack->result, ESP_ERR_TIMEOUT);
    return ack;
}

static void audio_ack_release(audio_ack_t *ack)
{
    if (ack != NULL && atomic_fetch_sub_explicit(&ack->refs, 1U,
                                                  memory_order_acq_rel) == 1U) {
        vSemaphoreDelete(ack->done);
        vPortFree(ack);
    }
}

static bool audio_cmd_cancelled(const audio_cmd_t *cmd)
{
    return cmd->ack != NULL &&
           atomic_load_explicit(&cmd->ack->cancelled, memory_order_acquire);
}

/* Called exactly once by the audio task for every dequeued command. */
static void audio_ack_complete(audio_cmd_t *cmd, esp_err_t result)
{
    if (cmd->ack == NULL) {
        return;
    }
    atomic_store_explicit(&cmd->ack->result, result, memory_order_release);
    xSemaphoreGive(cmd->ack->done);
    audio_ack_release(cmd->ack);
    cmd->ack = NULL;
}

/* ES8389 PGA accepts 0..36.5 dB in ~3 dB steps; the UI contract is 0..36
 * dB in 3 dB steps, so clamp and snap down to the step grid. */
static int sanitize_gain_db(int gain_db)
{
    int gain = clamp_int(gain_db, 0, 36);
    return (gain / 3) * 3;
}

static void emit_event(svc_audio_cb_t cb, void *user, svc_audio_event_t type,
                       int value, const char *path, esp_err_t err)
{
    if (cb == NULL) {
        return;
    }
    svc_audio_event_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.value = value;
    msg.err = err;
    if (path != NULL) {
        snprintf(msg.path, sizeof(msg.path), "%s", path);
    }
    cb(&msg, user);
}

/* Factory-verified raw route write: read-modify-write 0x72, preserving the
 * lower nibble (the PGA1 gain bits the driver just programmed). */
static esp_err_t codec_write_input_route(uint8_t input_select)
{
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) {
        return result;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8389_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t device = NULL;
    result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &device);
    if (result == ESP_OK) {
        const uint8_t reg = ROUTE_REG;
        uint8_t current = 0;
        result = i2c_master_transmit_receive(device, &reg, sizeof(reg),
                                             &current, sizeof(current), 200);
        if (result == ESP_OK) {
            const uint8_t payload[] = {
                reg,
                (uint8_t)((current & 0x0F) | input_select),
            };
            result = i2c_master_transmit(device, payload, sizeof(payload), 200);
        }
        i2c_master_bus_rm_device(device);
    }
    return result;
}

static uint8_t route_register_value(svc_audio_route_t route)
{
    switch (route) {
    case SVC_AUDIO_ROUTE_LEFT:
        return ROUTE_MIC1_SINGLE;
    case SVC_AUDIO_ROUTE_RIGHT:
        return ROUTE_MIC2_SINGLE;
    case SVC_AUDIO_ROUTE_STEREO:
    case SVC_AUDIO_ROUTE_DENOISE:
    default:
        return ROUTE_STEREO_DEFAULT;
    }
}

/* ------------------------------------------------------------------ */
/* Basic noise reduction (fixed point)                                 */
/* ------------------------------------------------------------------ */

static int16_t denoise_hp_sample(denoise_channel_t *ch, int16_t sample)
{
    /* y[n] = x[n] - x[n-1] + R*y[n-1], R in Q15. The whole accumulation
     * runs in int64: diff<<15 alone can reach the int32 edges and adding
     * the Q15 history on top would overflow a 32-bit accumulator (UB). */
    int64_t acc = (int64_t)ch->hp_acc + (((int64_t)sample - ch->hp_prev) << 15);
    acc = (acc * DENOISE_HP_COEF_Q15) >> 15;
    if (acc > INT32_MAX) {
        acc = INT32_MAX;
    } else if (acc < INT32_MIN) {
        acc = INT32_MIN;
    }
    ch->hp_acc = (int32_t)acc;
    ch->hp_prev = sample;
    int32_t out = (int32_t)acc >> 15;
    if (out > INT16_MAX) {
        out = INT16_MAX;
    } else if (out < INT16_MIN) {
        out = INT16_MIN;
    }
    return (int16_t)out;
}

/* In-place high-pass + noise gate on interleaved stereo frames. */
static void denoise_frames(denoise_channel_t channels[2], int16_t *samples,
                           size_t frame_count)
{
    for (unsigned channel = 0; channel < 2; ++channel) {
        denoise_channel_t *ch = &channels[channel];
        size_t frame = 0;
        while (frame < frame_count) {
            size_t group_end = frame + DENOISE_GROUP_FRAMES;
            if (group_end > frame_count) {
                group_end = frame_count;
            }
            int32_t peak = 0;
            for (size_t i = frame; i < group_end; ++i) {
                const int16_t out =
                    denoise_hp_sample(ch, samples[i * 2 + channel]);
                samples[i * 2 + channel] = out;
                const int32_t absolute = out < 0 ? -(int32_t)out : (int32_t)out;
                if (absolute > peak) {
                    peak = absolute;
                }
            }
            if (peak < DENOISE_GATE_THRESHOLD) {
                for (size_t i = frame; i < group_end; ++i) {
                    samples[i * 2 + channel] = 0;
                }
            }
            frame = group_end;
        }
    }
}

/* ------------------------------------------------------------------ */
/* WAV helpers                                                         */
/* ------------------------------------------------------------------ */

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
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

static void wav_make_header(uint8_t header[WAV_HEADER_BYTES],
                            uint32_t data_size, uint16_t channels,
                            uint32_t sample_rate)
{
    const uint16_t frame_bytes = (uint16_t)(channels * sizeof(int16_t));
    memset(header, 0, WAV_HEADER_BYTES);
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, data_size + 36U);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);              /* PCM */
    write_le16(header + 22, channels);
    write_le32(header + 24, sample_rate);
    write_le32(header + 28, sample_rate * frame_bytes);
    write_le16(header + 32, frame_bytes);
    write_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_size);
}

typedef struct {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

static bool wav_sample_rate_supported(uint32_t sample_rate)
{
    /* Exact ratio-32 rows in the ES8389 coefficient table under the shared
     * use_mclk=false/no_dac_ref=true policy (see factory wav_parse). */
    return sample_rate == 8000U || sample_rate == 16000U ||
           sample_rate == 44100U || sample_rate == 48000U;
}

/* Ported from the factory firmware's wav_parse(). */
static esp_err_t wav_parse(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    if (fseek(file, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long file_size = ftell(file);
    if (file_size < (long)sizeof(header) || fseek(file, 0, SEEK_SET) != 0 ||
            fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_format = false;
    bool have_data = false;
    uint16_t audio_format = 0;
    uint32_t byte_rate = 0;
    memset(info, 0, sizeof(*info));

    for (unsigned chunk_index = 0;
            chunk_index < 64 && ftell(file) >= 0 &&
            ftell(file) + 8 <= file_size; ++chunk_index) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) !=
                sizeof(chunk_header)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t chunk_size = read_le32(chunk_header + 4);
        const long payload_offset = ftell(file);
        const uint64_t next_offset = (uint64_t)payload_offset + chunk_size +
                                     (chunk_size & 1U);
        if (payload_offset < 0 || next_offset > (uint64_t)file_size) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t format[16];
            if (chunk_size < sizeof(format) ||
                    fread(format, 1, sizeof(format), file) != sizeof(format)) {
                return ESP_ERR_INVALID_SIZE;
            }
            audio_format = read_le16(format);
            info->channels = read_le16(format + 2);
            info->sample_rate = read_le32(format + 4);
            byte_rate = read_le32(format + 8);
            info->block_align = read_le16(format + 12);
            info->bits_per_sample = read_le16(format + 14);
            have_format = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_offset = payload_offset;
            info->data_size = chunk_size;
            have_data = true;
        }

        if (have_format && have_data) {
            break;
        }
        if (fseek(file, (long)next_offset, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    }

    if (!have_format || !have_data || audio_format != 1U ||
            (info->channels != 1U && info->channels != 2U) ||
            info->bits_per_sample != 16U ||
            !wav_sample_rate_supported(info->sample_rate)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint16_t expected_align = (uint16_t)(info->channels * sizeof(int16_t));
    if (info->block_align != expected_align ||
            byte_rate != info->sample_rate * info->block_align ||
            info->data_size == 0 || info->data_size % info->block_align != 0 ||
            (uint64_t)info->data_offset + info->data_size > (uint64_t)file_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, info->data_offset, SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

/* Choose rec_HHMMSS.wav from the RTC, falling back to rec_001.wav..
 * rec_999.wav (first free slot) when the RTC time is invalid or the
 * timestamped name already exists. */
static esp_err_t record_build_path(char *out_path, size_t out_size)
{
    char dir[112];
    snprintf(dir, sizeof(dir), "%s/%s", svc_storage_mount_point(),
             REC_DIR_NAME);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", dir, strerror(errno));
        return ESP_FAIL;
    }

    char name[24];
    name[0] = '\0';
    bsp_rtc_time_t time;
    bsp_rtc_status_t status;
    if (bsp_rtc_get_time(&time, &status) == ESP_OK && status.time_valid) {
        snprintf(name, sizeof(name), "rec_%02u%02u%02u.wav",
                 time.hour, time.minute, time.second);
        char probe[140];
        snprintf(probe, sizeof(probe), "%s/%s", dir, name);
        struct stat st;
        if (stat(probe, &st) == 0) {
            name[0] = '\0'; /* same-second collision: fall through */
        }
    }
    if (name[0] == '\0') {
        for (unsigned index = 1; index <= 999; ++index) {
            snprintf(name, sizeof(name), "rec_%03u.wav", index);
            char probe[140];
            snprintf(probe, sizeof(probe), "%s/%s", dir, name);
            struct stat st;
            if (stat(probe, &st) != 0) {
                break;
            }
            if (index == 999) {
                ESP_LOGE(TAG, "recordings dir full (999 files)");
                return ESP_FAIL;
            }
        }
    }
    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_path, dir, dir_len);
    out_path[dir_len] = '/';
    memcpy(out_path + dir_len + 1, name, name_len + 1);
    return ESP_OK;
}

static esp_err_t record_save_wav(const uint8_t *pcm, uint32_t pcm_size,
                                 char *out_path, size_t out_size)
{
    svc_storage_lease_t lease = {0};
    esp_err_t result = svc_storage_lease_acquire(&lease);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "record save rejected: TF absent or removing");
        return result;
    }
    result = record_build_path(out_path, out_size);
    if (result != ESP_OK) {
        svc_storage_lease_release(&lease);
        return result;
    }
    uint8_t header[WAV_HEADER_BYTES];
    wav_make_header(header, pcm_size, 2, REC_SAMPLE_RATE);
    FILE *file = fopen(out_path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "open %s for write failed: %s", out_path,
                 strerror(errno));
        svc_storage_lease_release(&lease);
        return ESP_FAIL;
    }
    result = ESP_OK;
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
            fwrite(pcm, 1, pcm_size, file) != pcm_size || fflush(file) != 0 ||
            fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "write %s failed: %s", out_path, strerror(errno));
        result = ESP_FAIL;
    }
    if (fclose(file) != 0 && result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (result != ESP_OK) {
        unlink(out_path);
    }
    svc_storage_lease_release(&lease);
    return result;
}

/* Zero the channel the single-mic routes do not carry, so "left only"
 * really is left only inside the saved WAV. */
static void zero_channel(int16_t *samples, size_t sample_count,
                         unsigned channel)
{
    for (size_t index = channel; index < sample_count; index += 2) {
        samples[index] = 0;
    }
}

static void record_apply_route(svc_audio_route_t route, int16_t *samples,
                               size_t frame_count,
                               denoise_channel_t denoise[2])
{
    const size_t sample_count = frame_count * 2;
    switch (route) {
    case SVC_AUDIO_ROUTE_LEFT:
        zero_channel(samples, sample_count, 1);
        break;
    case SVC_AUDIO_ROUTE_RIGHT:
        zero_channel(samples, sample_count, 0);
        break;
    case SVC_AUDIO_ROUTE_DENOISE:
        denoise_frames(denoise, samples, frame_count);
        break;
    case SVC_AUDIO_ROUTE_STEREO:
    default:
        break; /* raw differential pair, untouched */
    }
}

/* Runs on the audio task after record_start was acked. Owns the codec
 * until the capture (and WAV save) is finished. */
static void record_run(svc_audio_route_t route, int gain_db,
                       svc_audio_cb_t cb, void *user, uint8_t *pcm)
{
    /* The audio task only exists once svc_audio_start() brought the codec
     * up, so these borrows are non-NULL; the guard is cheap insurance
     * against future reordering. */
    esp_codec_dev_handle_t speaker = s_speaker_dev;
    esp_codec_dev_handle_t microphone = s_mic_dev;
    if (speaker == NULL || microphone == NULL) {
        ESP_LOGE(TAG, "record: codec unavailable");
        emit_event(cb, user, SVC_AUDIO_EV_ERROR, 0, NULL,
                   ESP_ERR_INVALID_STATE);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }

    esp_codec_dev_sample_info_t format = audio_format(REC_SAMPLE_RATE);
    bool speaker_open_attempted = false;
    bool mic_open_attempted = false;
    bool pa_off = false;
    denoise_channel_t denoise[2] = {{0}, {0}};
    uint32_t filled = 0;

    /* Iron open order: speaker -> PA domain off -> mic -> out vol -> in
     * gain -> input route (see the header comment). */
    int result = esp_codec_dev_open(speaker, &format);
    speaker_open_attempted = true;
    if (result == ESP_CODEC_DEV_OK) {
        result = bsp_power_domain_set(BSP_POWER_AUDIO_PA, false);
        pa_off = (result == ESP_OK);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_open(microphone, &format);
        mic_open_attempted = true;
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_out_vol(speaker, 0);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = esp_codec_dev_set_in_gain(microphone, (float)gain_db);
    }
    if (result == ESP_CODEC_DEV_OK) {
        result = codec_write_input_route(route_register_value(route));
    }
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "record open chain failed: %s", esp_err_to_name(result));
        goto cleanup;
    }

    /* Discard the first two pipeline-fill blocks. */
    for (unsigned index = 0;
            index < REC_DISCARD_BLOCKS && result == ESP_CODEC_DEV_OK;
            ++index) {
        result = esp_codec_dev_read(microphone, s_io, REC_IO_BYTES);
    }

    uint32_t window_peak = 0;
    uint32_t since_level_bytes = 0;
    bool stop_requested = false;

    while (result == ESP_CODEC_DEV_OK && !stop_requested &&
            filled < REC_MAX_BYTES) {
        /* Service pending commands between blocks (~64 ms each). */
        audio_cmd_t cmd;
        while (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
            esp_err_t ack = ESP_ERR_INVALID_STATE;
            if (audio_cmd_cancelled(&cmd)) {
                ack = ESP_ERR_TIMEOUT;
            } else if (cmd.type == CMD_RECORD_STOP) {
                stop_requested = true;
                ack = ESP_OK;
            } else if (cmd.type == CMD_SET_GAIN) {
                const int gain = sanitize_gain_db(cmd.gain_db);
                if (esp_codec_dev_set_in_gain(microphone, (float)gain) ==
                        ESP_CODEC_DEV_OK) {
                    /* set_in_gain rewrote 0x72; restore the route, and
                     * propagate a restore failure so the caller knows the
                     * recording fell back to the default route. */
                    ack = codec_write_input_route(route_register_value(route));
                } else {
                    ack = ESP_FAIL;
                }
            }
            audio_ack_complete(&cmd, ack);
        }
        if (stop_requested) {
            break;
        }

        const uint32_t remaining = REC_MAX_BYTES - filled;
        const uint32_t chunk = remaining < REC_IO_BYTES ? remaining :
                               REC_IO_BYTES;
        result = esp_codec_dev_read(microphone, s_io, chunk);
        if (result != ESP_CODEC_DEV_OK) {
            break;
        }

        int16_t *samples = (int16_t *)s_io;
        record_apply_route(route, samples, chunk / REC_FRAME_BYTES, denoise);

        for (size_t index = 0; index < chunk / sizeof(int16_t); ++index) {
            const int32_t absolute = samples[index] < 0 ?
                                     -(int32_t)samples[index] :
                                     (int32_t)samples[index];
            if ((uint32_t)absolute > window_peak) {
                window_peak = (uint32_t)absolute;
            }
        }
        memcpy(pcm + filled, s_io, chunk);
        filled += chunk;
        since_level_bytes += chunk;

        if (since_level_bytes >= REC_LEVEL_PERIOD_BYTES) {
            since_level_bytes = 0;
            const int level = (int)(window_peak * 100U / 32767U);
            emit_event(cb, user, SVC_AUDIO_EV_LEVEL,
                       level > 100 ? 100 : level, NULL, ESP_OK);
            window_peak = 0;
        }
    }

cleanup:
    if (mic_open_attempted) {
        esp_codec_dev_close(microphone);
    }
    if (speaker_open_attempted) {
        esp_codec_dev_close(speaker);
    }
    if (pa_off) {
        bsp_power_domain_set(BSP_POWER_AUDIO_PA, true);
    }

    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "capture failed: %s", esp_err_to_name(result));
        emit_event(cb, user, SVC_AUDIO_EV_ERROR, 0, NULL, result);
        heap_caps_free(pcm);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }

    /* Save phase: the task stays busy (mode SAVING) so new record/play
     * requests are rejected while the WAV is written to the card. */
    audio_mode_store(AUDIO_MODE_SAVING);
    char path[sizeof(((svc_audio_event_msg_t *)0)->path)];
    const esp_err_t save_result = record_save_wav(pcm, filled, path,
                                                  sizeof(path));
    heap_caps_free(pcm);
    if (save_result != ESP_OK) {
        emit_event(cb, user, SVC_AUDIO_EV_ERROR, 0, NULL, save_result);
    } else {
        emit_event(cb, user, SVC_AUDIO_EV_RECORD_DONE,
                   (int)(filled / (REC_SAMPLE_RATE * REC_FRAME_BYTES)),
                   path, ESP_OK);
    }
    audio_mode_store(AUDIO_MODE_IDLE);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char path[96];
    int volume;
    svc_audio_cb_t cb;
    void *user;
} play_ctx_t;

static void play_teardown(FILE **file, esp_codec_dev_handle_t *speaker,
                          bool *open_attempted)
{
    if (*open_attempted) {
        esp_codec_dev_close(*speaker);
        *open_attempted = false;
    }
    if (*file != NULL) {
        fclose(*file);
        *file = NULL;
    }
}

/* Runs on the audio task after play_start was acked. */
static void play_run(play_ctx_t *ctx)
{
    svc_storage_lease_t lease = {0};
    esp_err_t lease_result = svc_storage_lease_acquire(&lease);
    if (lease_result != ESP_OK) {
        ESP_LOGW(TAG, "play rejected: TF absent or removing");
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL,
                   lease_result);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }
    FILE *file = fopen(ctx->path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "open %s failed: %s", ctx->path, strerror(errno));
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL,
                   ESP_ERR_NOT_FOUND);
        svc_storage_lease_release(&lease);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }
    wav_info_t info;
    esp_err_t result = wav_parse(file, &info);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "unsupported WAV %s: %s", ctx->path,
                 esp_err_to_name(result));
        fclose(file);
        svc_storage_lease_release(&lease);
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL, result);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }

    /* See record_run: borrowed, non-NULL once the service is running. */
    esp_codec_dev_handle_t speaker = s_speaker_dev;
    if (speaker == NULL) {
        ESP_LOGE(TAG, "play: codec unavailable");
        fclose(file);
        svc_storage_lease_release(&lease);
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL,
                   ESP_ERR_INVALID_STATE);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }

    /* The PA domain was left off by boot (or by a recording); make sure
     * the amplifier rail is up before unmuting. */
    bsp_power_domain_set(BSP_POWER_AUDIO_PA, true);

    esp_codec_dev_sample_info_t format = audio_format(info.sample_rate);
    bool open_attempted = false;
    int codec_result = esp_codec_dev_open(speaker, &format);
    open_attempted = true;
    if (codec_result == ESP_CODEC_DEV_OK) {
        codec_result = esp_codec_dev_set_out_vol(speaker, ctx->volume);
    }
    if (codec_result != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open/volume failed: %s",
                 esp_err_to_name(codec_result));
        play_teardown(&file, speaker, &open_attempted);
        svc_storage_lease_release(&lease);
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL,
                   codec_result);
        audio_mode_store(AUDIO_MODE_IDLE);
        return;
    }

    uint32_t remaining = info.data_size;
    uint32_t consumed = 0;
    uint32_t next_progress_bytes = info.sample_rate * info.block_align;
    bool stop_requested = false;
    bool paused = false;
    result = ESP_OK;

    while (remaining > 0 && result == ESP_OK && !stop_requested) {
        audio_cmd_t cmd;
        while (xQueueReceive(s_queue, &cmd, 0) == pdTRUE) {
            esp_err_t ack = ESP_ERR_INVALID_STATE;
            if (audio_cmd_cancelled(&cmd)) {
                ack = ESP_ERR_TIMEOUT;
            } else if (cmd.type == CMD_PLAY_STOP) {
                stop_requested = true;
                ack = ESP_OK;
            } else if (cmd.type == CMD_PLAY_PAUSE) {
                paused = cmd.pause_on;
                ack = ESP_OK;
            } else if (cmd.type == CMD_SET_VOLUME) {
                ctx->volume = clamp_int(cmd.volume, 0, 100);
                ack = esp_codec_dev_set_out_vol(speaker, ctx->volume) ==
                      ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
            }
            audio_ack_complete(&cmd, ack);
        }
        if (stop_requested) {
            break;
        }
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(PLAY_PAUSE_POLL_MS));
            continue;
        }

        const size_t request = remaining < PLAY_IO_BYTES ? remaining :
                               PLAY_IO_BYTES;
        const size_t bytes_read = fread(s_io, 1, request, file);
        if (bytes_read != request) {
            ESP_LOGE(TAG, "short read at data byte %u: %s",
                     (unsigned)(info.data_size - remaining), strerror(errno));
            result = ESP_FAIL;
            break;
        }

        /* Single speaker on this board: mono input is duplicated to both
         * DAC channels, stereo input is mixed to mono first - the factory
         * WAV_PLAY_MIX path, verified on hardware. */
        const int16_t *input = (const int16_t *)s_io;
        size_t output_samples = 0;
        if (info.channels == 1U) {
            const size_t samples = bytes_read / sizeof(int16_t);
            for (size_t index = 0; index < samples; ++index) {
                s_expand[index * 2] = input[index];
                s_expand[index * 2 + 1] = input[index];
            }
            output_samples = samples * 2;
        } else {
            const size_t frame_count = bytes_read / info.block_align;
            for (size_t frame = 0; frame < frame_count; ++frame) {
                const int32_t sum = (int32_t)input[frame * 2] +
                                    input[frame * 2 + 1];
                const int16_t sample = (int16_t)(sum / 2);
                s_expand[frame * 2] = sample;
                s_expand[frame * 2 + 1] = sample;
            }
            output_samples = frame_count * 2;
        }
        codec_result = esp_codec_dev_write(speaker, s_expand,
                                           (int)(output_samples *
                                                 sizeof(int16_t)));
        if (codec_result != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "speaker write failed: %s",
                     esp_err_to_name(codec_result));
            result = codec_result;
            break;
        }
        remaining -= bytes_read;
        consumed += bytes_read;

        if (consumed >= next_progress_bytes) {
            next_progress_bytes += info.sample_rate * info.block_align;
            emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_PLAY_PROGRESS,
                       (int)(consumed / (info.sample_rate *
                                         info.block_align)),
                       NULL, ESP_OK);
        }
    }

    if (result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100)); /* drain the DMA tail */
    }
    play_teardown(&file, speaker, &open_attempted);
    svc_storage_lease_release(&lease);

    if (result != ESP_OK) {
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_ERROR, 0, NULL, result);
    } else {
        /* Natural EOF and caller-requested stop both end here. */
        emit_event(ctx->cb, ctx->user, SVC_AUDIO_EV_PLAY_DONE,
                   (int)(consumed / (info.sample_rate * info.block_align)),
                   ctx->path, ESP_OK);
    }
    audio_mode_store(AUDIO_MODE_IDLE);
}

/* ------------------------------------------------------------------ */
/* Task and command plumbing                                           */
/* ------------------------------------------------------------------ */

static void record_start_command(audio_cmd_t *cmd)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (audio_mode_load() == AUDIO_MODE_IDLE) {
        if (!svc_storage_mounted()) {
            result = ESP_ERR_INVALID_STATE; /* contract: no SD card */
        } else {
            uint8_t *pcm = heap_caps_malloc(REC_MAX_BYTES,
                                            MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT);
            if (pcm == NULL) {
                ESP_LOGE(TAG, "PSRAM alloc (%u B) failed",
                         (unsigned)REC_MAX_BYTES);
                result = ESP_ERR_NO_MEM;
            } else {
                const int gain = sanitize_gain_db(cmd->gain_db);
                if (audio_cmd_cancelled(cmd)) {
                    heap_caps_free(pcm);
                    audio_ack_complete(cmd, ESP_ERR_TIMEOUT);
                    return;
                }
                audio_mode_store(AUDIO_MODE_RECORDING);
                audio_ack_complete(cmd, ESP_OK);
                record_run(cmd->route, gain, cmd->cb, cmd->user, pcm);
                return;
            }
        }
    }
    audio_ack_complete(cmd, result);
}

static void play_start_command(audio_cmd_t *cmd)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (audio_mode_load() == AUDIO_MODE_IDLE) {
        if (!svc_storage_mounted()) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            play_ctx_t ctx;
            memset(&ctx, 0, sizeof(ctx));
            snprintf(ctx.path, sizeof(ctx.path), "%s", cmd->path);
            ctx.volume = clamp_int(cmd->volume, 0, 100);
            ctx.cb = cmd->cb;
            ctx.user = cmd->user;
            if (audio_cmd_cancelled(cmd)) {
                audio_ack_complete(cmd, ESP_ERR_TIMEOUT);
                return;
            }
            audio_mode_store(AUDIO_MODE_PLAYING);
            audio_ack_complete(cmd, ESP_OK);
            play_run(&ctx);
            return;
        }
    }
    audio_ack_complete(cmd, result);
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (audio_cmd_cancelled(&cmd)) {
            audio_ack_complete(&cmd, ESP_ERR_TIMEOUT);
            continue;
        }
        esp_err_t result = ESP_ERR_INVALID_STATE;
        switch (cmd.type) {
        case CMD_RECORD_START:
            record_start_command(&cmd);
            continue; /* ack handled inside */
        case CMD_PLAY_START:
            play_start_command(&cmd);
            continue;
        default:
            break; /* idle-time stop/gain/volume requests: invalid state */
        }
        audio_ack_complete(&cmd, result);
    }
}

static esp_err_t send_command(audio_cmd_t *cmd)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        /* The task can never ack its own command: refuse the deadlock. */
        return ESP_ERR_INVALID_STATE;
    }
    audio_ack_t *ack = audio_ack_create();
    if (ack == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cmd->ack = ack;
    if (xQueueSend(s_queue, cmd, pdMS_TO_TICKS(AUDIO_QUEUE_TIMEOUT_MS)) !=
            pdTRUE) {
        /* The task never received its ownership reference. */
        audio_ack_release(ack);
        audio_ack_release(ack);
        return ESP_ERR_TIMEOUT;
    }
    const BaseType_t completed =
        xSemaphoreTake(ack->done, pdMS_TO_TICKS(AUDIO_ACK_TIMEOUT_MS));
    if (completed != pdTRUE) {
        /* Prevent a command that is still waiting in the queue from
         * becoming a late, invisible record/play operation after the UI
         * has already recovered from the timeout. */
        atomic_store_explicit(&ack->cancelled, true, memory_order_release);
    }
    const esp_err_t result = completed == pdTRUE ?
        (esp_err_t)atomic_load_explicit(&ack->result, memory_order_acquire) :
        ESP_ERR_TIMEOUT;
    audio_ack_release(ack);
    return result;
}

/* Fire-and-forget variant for teardown paths that must not block on the
 * audio task (page deletion while the task sits in a slow SD transfer
 * would hold the LVGL thread for up to the ack timeout). The command is
 * enqueued without an ack object: audio_ack_complete() and the
 * cancellation check treat ack == NULL as "nobody is waiting", so the
 * running record/play loops service it exactly like an acked stop. */
static esp_err_t send_command_nowait(audio_cmd_t *cmd)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_task) {
        /* The task can never ack its own command: refuse the deadlock. */
        return ESP_ERR_INVALID_STATE;
    }
    cmd->ack = NULL;
    if (xQueueSend(s_queue, cmd, pdMS_TO_TICKS(AUDIO_QUEUE_TIMEOUT_MS)) !=
            pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Bring the codec up for the whole service lifetime, then start the task.
 *
 * The I2S TX+RX DMA descriptors (~12.8 KB of MALLOC_CAP_INTERNAL|
 * MALLOC_CAP_DMA) are allocated here on the main task, before
 * svc_net_start() runs (demo_main.c starts audio before net) and before
 * WiFi/BLE churn and UI navigation fragment the internal DMA pool
 * (2026-08-21 board log: lazy first-record init failed
 * i2s_alloc_dma_desc 10/10 when even 960 B contiguous was gone).
 * Tradeoff, accepted for the demo: AUDIO_3V3_SW (TG28 ALDO3) and both I2S
 * channels stay powered for the whole uptime. Deep sleep reboots the chip
 * on wake, and svc_power restarts the system if the pre-sleep
 * bsp_power_safe_state() reports a failure after tearing rails - both
 * paths re-run the boot chain and reach this function again, so no
 * pre-sleep audio deinit is needed. Sleep attempts refused BEFORE
 * safe-state (VBUS/preflight/alarm/EXT1 checks) return to the running UI
 * with the handles still valid.
 *
 * Cleanup contract: bsp_audio_deinit() internally performs
 * bsp_audio_codec_deinit() on both cached instances and then frees the
 * shared data interface and both I2S channels (bsp_audio.c). Failure paths
 * therefore call it exactly once and never add a codec deinit on top,
 * which would double-free.
 */
esp_err_t svc_audio_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* Idempotent; returns the real I2S/DMA error and cleans up after
     * itself on failure. The codec constructors below reuse its channels. */
    esp_err_t error = bsp_audio_init(NULL);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(error));
        return error;
    }

    s_speaker_dev = bsp_audio_codec_speaker_init();
    s_mic_dev = bsp_audio_codec_microphone_init();
    if (s_speaker_dev == NULL || s_mic_dev == NULL) {
        ESP_LOGE(TAG, "codec init failed: speaker=%p mic=%p",
                 s_speaker_dev, s_mic_dev);
        bsp_audio_deinit(); /* full single rollback, clears BSP statics */
        s_speaker_dev = NULL;
        s_mic_dev = NULL;
        return ESP_FAIL;
    }

    s_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_cmd_t));
    if (s_queue == NULL) {
        bsp_audio_deinit();
        s_speaker_dev = NULL;
        s_mic_dev = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "svc_audio", AUDIO_TASK_STACK, NULL,
                    AUDIO_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        bsp_audio_deinit();
        s_speaker_dev = NULL;
        s_mic_dev = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "audio task started (prio %d, stack %d, queue %d)",
             AUDIO_TASK_PRIO, AUDIO_TASK_STACK, AUDIO_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t svc_audio_record_start(svc_audio_route_t route, int gain_db,
                                 svc_audio_cb_t cb, void *user)
{
    if (route < SVC_AUDIO_ROUTE_LEFT || route > SVC_AUDIO_ROUTE_DENOISE ||
            cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_RECORD_START;
    cmd.route = route;
    cmd.gain_db = sanitize_gain_db(gain_db);
    cmd.cb = cb;
    cmd.user = user;
    return send_command(&cmd);
}

esp_err_t svc_audio_record_stop(void)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_RECORD_STOP;
    return send_command(&cmd);
}

/* Fire-and-forget stop for page teardown: never waits on the audio task
 * (see send_command_nowait). The caller must have invalidated its audio
 * event token, because completion events can now arrive after the page
 * is gone. */
esp_err_t svc_audio_record_stop_async(void)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_RECORD_STOP;
    return send_command_nowait(&cmd);
}

bool svc_audio_is_recording(void)
{
    const audio_mode_t mode = audio_mode_load();
    return mode == AUDIO_MODE_RECORDING || mode == AUDIO_MODE_SAVING;
}

esp_err_t svc_audio_record_set_gain(int gain_db)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_GAIN;
    cmd.gain_db = sanitize_gain_db(gain_db);
    return send_command(&cmd);
}

esp_err_t svc_audio_play(const char *path, int volume,
                         svc_audio_cb_t cb, void *user)
{
    if (path == NULL || path[0] == '\0' || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= 96) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PLAY_START;
    snprintf(cmd.path, sizeof(cmd.path), "%s", path);
    cmd.volume = clamp_int(volume, 0, 100);
    cmd.cb = cb;
    cmd.user = user;
    return send_command(&cmd);
}

esp_err_t svc_audio_play_stop(void)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PLAY_STOP;
    return send_command(&cmd);
}

/* Fire-and-forget stop for page teardown (see svc_audio_record_stop_async). */
esp_err_t svc_audio_play_stop_async(void)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PLAY_STOP;
    return send_command_nowait(&cmd);
}

bool svc_audio_is_playing(void)
{
    return audio_mode_load() == AUDIO_MODE_PLAYING;
}

esp_err_t svc_audio_set_volume(int volume)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_SET_VOLUME;
    cmd.volume = clamp_int(volume, 0, 100);
    return send_command(&cmd);
}

/* Pause/resume keeps the file and codec open and simply stops feeding
 * samples. Declared in services/svc_audio.h. */
esp_err_t svc_audio_play_pause(bool pause)
{
    audio_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PLAY_PAUSE;
    cmd.pause_on = pause;
    return send_command(&cmd);
}
