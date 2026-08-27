/*
 * Candis-S31 watch demo - AUDIO domain test suite.
 *
 * Ports the factory audio commands onto the demo services (spec E.4):
 * speaker tone and record/playback stay INTERACTIVE with an operator
 * verdict; mic analysis and both loopbacks are fully automatic. All codec
 * access goes through svc_audio (single-owner discipline, spec C.9/F3).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "demo_board.h"
#include "services/svc_audio.h"
#include "services/svc_storage.h"
#include "test_registry.h"

static void set_result(test_result_t *out, test_status_t st,
                       const char *evidence)
{
    out->st = st;
    snprintf(out->evidence, sizeof(out->evidence), "%s", evidence);
}

/* ------------------------------------------------------------------ */
/* Speaker tone (factory speaker_test): 880 Hz / 2 s / settings volume */
/* ------------------------------------------------------------------ */

static void test_speaker_tone(const test_ctx_t *ctx, test_result_t *out)
{
    const int volume = demo_settings()->volume;
    ctx->progress(ctx, -1, "Playing 880 Hz tone");
    const esp_err_t err = svc_audio_tone_start(880, volume, 2000);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "audio busy");
        return;
    }
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "tone start failed: %s", esp_err_to_name(err));
        return;
    }
    /* The tone auto-stops after 2 s; let it finish before the verdict. */
    for (int waited = 0; waited < 2200 && !ctx->cancel_requested(ctx);
            waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ctx->cancel_requested(ctx)) {
        svc_audio_tone_stop();
        return; /* runner records SKIP "aborted" */
    }
    bool timed_out = false;
    const bool heard = ctx->ask_operator(ctx, "Did you hear the tone?",
                                         15000, &timed_out);
    if (heard) {
        out->st = TEST_ST_PASS;
        snprintf(out->evidence, sizeof(out->evidence),
                 "880 Hz 2 s at vol %d", volume);
    } else if (timed_out) {
        set_result(out, TEST_ST_SKIP, "no operator answer");
    } else {
        set_result(out, TEST_ST_FAIL, "operator heard no tone");
    }
}

/* ------------------------------------------------------------------ */
/* Mic analyze (factory microphone_test): 2 s capture, stats judgement */
/* ------------------------------------------------------------------ */

static void test_mic_analyze(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "Capturing 2 s - speak now");
    svc_audio_capture_stats_t stats;
    const esp_err_t err = svc_audio_capture_analyze(
        2000, SVC_AUDIO_ROUTE_STEREO, 24 /* factory default gain */, &stats);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "audio busy");
        return;
    }
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "capture failed: %s", esp_err_to_name(err));
        return;
    }
    const bool ok = stats.ch[0].live && stats.ch[1].live &&
                    !stats.ch[0].clipping && !stats.ch[1].clipping;
    out->st = ok ? TEST_ST_PASS : TEST_ST_FAIL;
    snprintf(out->evidence, sizeof(out->evidence),
             "pk %u/%u rms %lu/%lu dc %ld/%ld clip %lu/%lu",
             (unsigned)stats.ch[0].peak, (unsigned)stats.ch[1].peak,
             (unsigned long)stats.ch[0].ac_rms,
             (unsigned long)stats.ch[1].ac_rms,
             (long)stats.ch[0].dc, (long)stats.ch[1].dc,
             (unsigned long)stats.ch[0].clip_count,
             (unsigned long)stats.ch[1].clip_count);
}

/* ------------------------------------------------------------------ */
/* Codec loopback (factory codec_loopback): ES8389 DAC->ADC mix        */
/* ------------------------------------------------------------------ */

static void test_codec_loopback(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "DAC->ADC mix loopback");
    svc_audio_loopback_stats_t stats;
    const esp_err_t err = svc_audio_codec_loopback(&stats);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "audio busy");
        return;
    }
    out->st = err == ESP_OK ? TEST_ST_PASS : TEST_ST_FAIL;
    snprintf(out->evidence, sizeof(out->evidence),
             "peak %u nz %lu/%lu edges %lu%s",
             (unsigned)stats.peak, (unsigned long)stats.nonzero_samples,
             (unsigned long)stats.sample_count,
             (unsigned long)stats.asdout_edges,
             err == ESP_OK ? "" : " (fail)");
}

/* ------------------------------------------------------------------ */
/* I2S internal loopback (factory i2s_loopback): GPIO8 pattern verify  */
/* ------------------------------------------------------------------ */

static void test_i2s_loopback(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->progress(ctx, -1, "I2S internal loopback");
    svc_audio_i2s_loopback_stats_t stats;
    const esp_err_t err = svc_audio_i2s_loopback(&stats);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "audio busy");
        return;
    }
    if (err == ESP_OK) {
        out->st = TEST_ST_PASS;
        snprintf(out->evidence, sizeof(out->evidence),
                 "pattern @%lu, %lu B", (unsigned long)stats.pattern_offset,
                 (unsigned long)stats.bytes_read);
    } else {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "wr %lu rd %lu nz %lu found %d (%s)",
                 (unsigned long)stats.bytes_written,
                 (unsigned long)stats.bytes_read,
                 (unsigned long)stats.nonzero_bytes,
                 (int)stats.pattern_found, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------ */
/* Record & playback (factory audio_record_playback): 3 s WAV roundtrip */
/* ------------------------------------------------------------------ */

typedef struct {
    SemaphoreHandle_t done;
    svc_audio_event_t type;
    esp_err_t err;
    char path[96];
} audio_sync_t;

/* Runs on the audio task: only terminal events unblock the test. */
static void audio_sync_cb(const svc_audio_event_msg_t *ev, void *user)
{
    audio_sync_t *sync = (audio_sync_t *)user;
    if (ev->type == SVC_AUDIO_EV_RECORD_DONE ||
            ev->type == SVC_AUDIO_EV_PLAY_DONE ||
            ev->type == SVC_AUDIO_EV_ERROR) {
        sync->type = ev->type;
        sync->err = ev->err;
        snprintf(sync->path, sizeof(sync->path), "%s", ev->path);
        xSemaphoreGive(sync->done);
    }
}

static bool audio_sync_wait(audio_sync_t *sync, uint32_t timeout_ms)
{
    return xSemaphoreTake(sync->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* Best-effort cleanup of the test artifact (lease-guarded). */
static void delete_quiet(const char *path)
{
    svc_storage_lease_t lease = {0};
    if (svc_storage_lease_acquire(&lease) == ESP_OK) {
        unlink(path);
        svc_storage_lease_release(&lease);
    }
}

static void test_record_playback(const test_ctx_t *ctx, test_result_t *out)
{
    audio_sync_t sync;
    memset(&sync, 0, sizeof(sync));
    sync.done = xSemaphoreCreateBinary();
    if (sync.done == NULL) {
        set_result(out, TEST_ST_FAIL, "no memory for sync");
        return;
    }

    ctx->progress(ctx, 5, "Recording 3 s");
    esp_err_t err = svc_audio_record_start(SVC_AUDIO_ROUTE_STEREO,
                                           demo_settings()->mic_gain_db,
                                           audio_sync_cb, &sync);
    if (err == ESP_ERR_INVALID_STATE) {
        set_result(out, TEST_ST_SKIP, "audio busy");
        vSemaphoreDelete(sync.done);
        return;
    }
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "record start failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(sync.done);
        return;
    }
    for (int ms = 0; ms < 3000 && !ctx->cancel_requested(ctx); ms += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        ctx->progress(ctx, 5 + ms * 45 / 3000, "Recording 3 s");
    }
    svc_audio_record_stop();

    ctx->progress(ctx, 55, "Saving WAV");
    if (!audio_sync_wait(&sync, 15000)) {
        set_result(out, TEST_ST_FAIL, "record completion timeout");
        goto done;
    }
    if (sync.type != SVC_AUDIO_EV_RECORD_DONE) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "record failed: %s", esp_err_to_name(sync.err));
        goto done;
    }
    if (ctx->cancel_requested(ctx)) {
        delete_quiet(sync.path);
        goto done; /* NOT_RUN -> runner records SKIP "aborted" */
    }

    ctx->progress(ctx, 70, "Playing back");
    err = svc_audio_play(sync.path, demo_settings()->volume,
                         audio_sync_cb, &sync);
    if (err != ESP_OK) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "playback start failed: %s", esp_err_to_name(err));
        delete_quiet(sync.path);
        goto done;
    }
    if (!audio_sync_wait(&sync, 15000)) {
        svc_audio_play_stop();
        set_result(out, TEST_ST_FAIL, "playback completion timeout");
        delete_quiet(sync.path);
        goto done;
    }
    if (sync.type != SVC_AUDIO_EV_PLAY_DONE) {
        out->st = TEST_ST_FAIL;
        snprintf(out->evidence, sizeof(out->evidence),
                 "playback failed: %s", esp_err_to_name(sync.err));
        delete_quiet(sync.path);
        goto done;
    }
    delete_quiet(sync.path);

    ctx->progress(ctx, 90, "Confirm with operator");
    bool timed_out = false;
    const bool heard = ctx->ask_operator(
        ctx, "Did you hear the recording?", 15000, &timed_out);
    if (heard) {
        set_result(out, TEST_ST_PASS, "3 s WAV record+play confirmed");
    } else if (timed_out) {
        set_result(out, TEST_ST_SKIP, "no operator answer");
    } else {
        set_result(out, TEST_ST_FAIL, "operator heard no playback");
    }

done:
    vSemaphoreDelete(sync.done);
}

/* ------------------------------------------------------------------ */

void test_audio_register(void)
{
    static const test_case_t cases[] = {
        {
            .id = "audio.speaker_tone", .name = "Speaker tone",
            .domain = TEST_DOM_AUDIO, .flags = TEST_F_INTERACTIVE,
            .timeout_ms = 30000, .run = test_speaker_tone,
        },
        {
            .id = "audio.mic_analyze", .name = "Mic analyze",
            .domain = TEST_DOM_AUDIO, .flags = 0,
            .timeout_ms = 12000, .run = test_mic_analyze,
        },
        {
            .id = "audio.codec_loopback", .name = "Codec loopback",
            .domain = TEST_DOM_AUDIO, .flags = 0,
            .timeout_ms = 12000, .run = test_codec_loopback,
        },
        {
            .id = "audio.i2s_loopback", .name = "I2S int loopback",
            .domain = TEST_DOM_AUDIO, .flags = 0,
            .timeout_ms = 20000, .run = test_i2s_loopback,
        },
        {
            .id = "audio.record_playback", .name = "Record & play",
            .domain = TEST_DOM_AUDIO,
            .flags = TEST_F_INTERACTIVE | TEST_F_NEEDS_SD,
            .timeout_ms = 45000, .run = test_record_playback,
        },
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
            ++index) {
        test_register(&cases[index]);
    }
}
