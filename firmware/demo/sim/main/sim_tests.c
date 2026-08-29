/*
 * Candis-S31 simulator - stub test suites.
 *
 * Registers the same 41 test cases (id/name/domain/flags/timeout copied
 * from firmware/demo/main/tests/test_*.c on 2026-08-29) with simulated
 * run functions, so the real test center, run view and result library can
 * be reviewed on the host. The stubs drive the full test_ctx_t contract:
 * progress stages, operator asks, an interactive canvas for the touch
 * test, cooperative cancellation. Results are simulated and MUST NOT be
 * read as hardware evidence.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/svc_storage.h"
#include "test_registry.h"

/* ------------------------------------------------------------------ */
/* Generic simulated runner                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *question;   /* INTERACTIVE tests: operator question */
    const char *evidence;   /* PASS evidence string */
    int steps;              /* progress steps */
    int step_ms;            /* delay per step */
} sim_test_profile_t;

static void sim_run_generic(const test_ctx_t *ctx, test_result_t *out,
                            const sim_test_profile_t *profile)
{
    for (int i = 1; i <= profile->steps; ++i) {
        if (ctx->cancel_requested(ctx)) {
            out->st = TEST_ST_SKIP;
            snprintf(out->evidence, sizeof(out->evidence), "cancelled");
            return;
        }
        ctx->progress(ctx, i * 100 / profile->steps, "simulating");
        vTaskDelay(pdMS_TO_TICKS(profile->step_ms));
    }
    if (profile->question != NULL) {
        bool timed_out = false;
        const bool yes = ctx->ask_operator(ctx, profile->question,
                                           30000, &timed_out);
        if (timed_out) {
            out->st = TEST_ST_SKIP;
            snprintf(out->evidence, sizeof(out->evidence),
                     "operator timeout");
            return;
        }
        if (!yes) {
            out->st = TEST_ST_FAIL;
            snprintf(out->evidence, sizeof(out->evidence),
                     "operator answered NO");
            return;
        }
    }
    out->st = TEST_ST_PASS;
    snprintf(out->evidence, sizeof(out->evidence), "%s",
             profile->evidence);
}

#define SIM_DEF(sym, q, ev, steps, ms)                                   \
    static const sim_test_profile_t sym = { q, ev, steps, ms };          \
    static void sym##_run(const test_ctx_t *ctx, test_result_t *out)     \
    {                                                                    \
        sim_run_generic(ctx, out, &sym);                                 \
    }

/* ------------------------------------------------------------------ */
/* Interactive canvas for touch.corners (mirrors the real quadrant UI)  */
/* ------------------------------------------------------------------ */

static void sim_touch_canvas_build(lv_obj_t *parent, void *user)
{
    (void)user;
    static const char *const corner_text[4] = { "TL", "TR", "BL", "BR" };
    static const lv_color_t corner_colors[4] = {
        { .red = 255, .green = 0, .blue = 0 },
        { .red = 0, .green = 255, .blue = 0 },
        { .red = 0, .green = 0, .blue = 255 },
        { .red = 255, .green = 255, .blue = 0 },
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_set_size(box, 96, 96);
        lv_obj_set_style_bg_color(box, corner_colors[i], 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 12, 0);
        lv_obj_align(box, (lv_align_t[]){
            LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT,
            LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_RIGHT }[i], 0, 0);
        lv_obj_t *label = lv_label_create(box);
        lv_label_set_text(label, corner_text[i]);
        lv_obj_center(label);
    }
    lv_obj_t *hint = lv_label_create(parent);
    lv_label_set_text(hint, "Simulated touch canvas\n(operator ask follows)");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint);
}

static const sim_test_profile_t sim_touch_corners_profile = {
    NULL, "4 corners hit (simulated)", 2, 300,
};

static void sim_run_touch_corners(const test_ctx_t *ctx, test_result_t *out)
{
    ctx->request_canvas(ctx, sim_touch_canvas_build, NULL);
    ctx->progress(ctx, 20, "canvas up");
    vTaskDelay(pdMS_TO_TICKS(1500));
    ctx->request_canvas(ctx, NULL, NULL);
    sim_run_generic(ctx, out, &sim_touch_corners_profile);
}

/* ------------------------------------------------------------------ */
/* Per-test profiles                                                   */
/* ------------------------------------------------------------------ */

SIM_DEF(sim_display_quadrant_profile, "4 quadrants colored correctly?",
        "operator confirmed", 3, 400)
SIM_DEF(sim_display_brightness_profile, "Brightness ramped smoothly?",
        "operator confirmed", 3, 300)
SIM_DEF(sim_display_motion_profile, NULL, "60.1 fps avg over 10 s", 8, 500)
SIM_DEF(sim_display_te_profile, NULL, "TE 16717 us avg, 598 edges", 3, 400)
SIM_DEF(sim_display_sleep_profile, NULL, "sleep/wake cycle ESP_OK", 4, 400)
SIM_DEF(sim_touch_latency_profile, "Draw grid then confirm?",
        "avg 2.4 ms p95 5.4 ms", 3, 400)
SIM_DEF(sim_camera_frames_profile, NULL, "5 frames 800x600 CRC vary", 5, 600)
SIM_DEF(sim_audio_tone_profile, "Did you hear the tone?",
        "operator confirmed tone", 2, 500)
SIM_DEF(sim_audio_mic_profile, NULL, "ch0 peak=14505 live=yes", 4, 500)
SIM_DEF(sim_audio_codec_lb_profile, NULL, "codec loopback 0 mismatch", 3, 400)
SIM_DEF(sim_audio_i2s_lb_profile, NULL, "pattern found @17", 3, 400)
SIM_DEF(sim_audio_record_profile, "Replay sounded right?",
        "operator confirmed playback", 4, 600)
SIM_DEF(sim_storage_rw_profile, NULL, "16 KiB write-verify ok", 3, 400)
SIM_DEF(sim_storage_usb_profile, "USB disk contents correct?",
        "operator confirmed", 3, 500)
SIM_DEF(sim_storage_typec_profile, NULL, "FUSB303 DRP no fault", 2, 300)
SIM_DEF(sim_storage_enum_profile, "CDC enumerated on PC?",
        "operator confirmed", 2, 500)
SIM_DEF(sim_net_wifi_scan_profile, NULL, "10 APs, strongest -40 dBm", 4, 400)
SIM_DEF(sim_net_wifi_conn_profile, "Connect to saved AP?",
        "DHCP 192.168.43.15", 5, 500)
SIM_DEF(sim_net_ble_smoke_profile, NULL, "NimBLE host up", 2, 300)
SIM_DEF(sim_net_ble_scan_profile, NULL, "8 advertisers in 10 s", 8, 500)
SIM_DEF(sim_sys_rtc_profile, NULL, "RTC 2026-08-29 12:00:00", 2, 200)
SIM_DEF(sim_sys_rtc_alarm_profile, NULL, "alarm fired @60 s (simulated)", 6, 500)
SIM_DEF(sim_sys_irq_profile, NULL, "shared IRQ serviced", 2, 300)
SIM_DEF(sim_sys_buttons_profile, "Press BOOT and PWR",
        "boot=yes power=yes", 2, 400)
SIM_DEF(sim_sys_led_profile, "Did RGB cycle red/green/blue?",
        "operator confirmed", 3, 500)
SIM_DEF(sim_sys_temp_profile, NULL, "46.5 C", 2, 200)
SIM_DEF(sim_mem_flash_profile, NULL, "16 MB JEDEC ef:4018", 2, 200)
SIM_DEF(sim_mem_psram_profile, NULL, "64 KiB pattern 0 mismatch", 3, 300)
SIM_DEF(sim_mem_alias_profile, NULL, "TRUE_32MB", 4, 400)
SIM_DEF(sim_mem_bw_profile, NULL, "104.6/83.9/64.1 MB/s", 6, 500)
SIM_DEF(sim_accel_jpeg_profile, NULL, "800x600 92 fps", 4, 400)
SIM_DEF(sim_accel_cordic_profile, NULL, "1.38x vs software", 2, 300)
SIM_DEF(sim_accel_ppa_profile, NULL, "48 Mpps 0 mismatch", 3, 400)
SIM_DEF(sim_accel_bits_profile, NULL, "transpose 0 mismatch", 2, 300)
SIM_DEF(sim_accel_asrc_profile, NULL, "211 us/block 0 mismatch", 3, 400)
SIM_DEF(sim_power_pmic_profile, NULL, "TG28 ID ok", 2, 200)
SIM_DEF(sim_power_rails_profile, NULL, "all rails readable", 2, 300)
SIM_DEF(sim_power_charge_profile, NULL, "ceiling 200 mA (unverified)", 2, 200)
SIM_DEF(sim_power_screen_profile, "Did the screen wake?",
        "operator confirmed", 2, 400)
SIM_DEF(sim_power_deep_profile, "RTC kept time across sleep?",
        "operator confirmed", 2, 400)

/* SD-gated tests report SKIP when the sim card is absent, mirroring the
 * real precondition checks (spec C.5/C.3). */
static void sim_run_needs_sd(const test_ctx_t *ctx, test_result_t *out,
                             void (*inner)(const test_ctx_t *,
                                           test_result_t *))
{
    if (!svc_storage_mounted()) {
        out->st = TEST_ST_SKIP;
        snprintf(out->evidence, sizeof(out->evidence), "no SD card");
        return;
    }
    inner(ctx, out);
}

#define SIM_DEF_SD(sym, base)                                            \
    static void sym(const test_ctx_t *ctx, test_result_t *out)           \
    {                                                                    \
        sim_run_needs_sd(ctx, out, base);                                \
    }

SIM_DEF_SD(sim_run_storage_rw_gated, sim_storage_rw_profile_run)

/* ------------------------------------------------------------------ */
/* Registration (metadata mirrors firmware tables; keep in sync)        */
/* ------------------------------------------------------------------ */

#define SIM_TC(id_, name_, dom_, flags_, tmo_, fn_)                      \
    { id_, name_, dom_, flags_, tmo_, fn_ }

void test_register_all(void)
{
    static const test_case_t cases[] = {
        /* display_touch */
        SIM_TC("display.quadrant", "Quadrant colors", TEST_DOM_DISPLAY_TOUCH,
               TEST_F_INTERACTIVE, 120000, sim_display_quadrant_profile_run),
        SIM_TC("display.brightness_ramp", "Brightness ramp",
               TEST_DOM_DISPLAY_TOUCH, TEST_F_INTERACTIVE, 60000,
               sim_display_brightness_profile_run),
        SIM_TC("display.motion_fps", "Motion FPS", TEST_DOM_DISPLAY_TOUCH,
               TEST_F_LONG, 30000, sim_display_motion_profile_run),
        SIM_TC("display.te_stats", "TE timing", TEST_DOM_DISPLAY_TOUCH,
               0, 15000, sim_display_te_profile_run),
        SIM_TC("display.sleep_cycle", "Panel sleep cycle",
               TEST_DOM_DISPLAY_TOUCH, 0, 60000,
               sim_display_sleep_profile_run),
        SIM_TC("touch.corners", "Touch 4 corners", TEST_DOM_DISPLAY_TOUCH,
               TEST_F_INTERACTIVE, 120000, sim_run_touch_corners),
        SIM_TC("touch.draw_latency", "Draw + latency", TEST_DOM_DISPLAY_TOUCH,
               TEST_F_INTERACTIVE, 120000, sim_touch_latency_profile_run),
        /* camera */
        SIM_TC("camera.frames", "Frame capture x5", TEST_DOM_CAMERA,
               0, 30000, sim_camera_frames_profile_run),
        /* audio */
        SIM_TC("audio.speaker_tone", "Speaker tone", TEST_DOM_AUDIO,
               TEST_F_INTERACTIVE, 30000, sim_audio_tone_profile_run),
        SIM_TC("audio.mic_analyze", "Mic analyze", TEST_DOM_AUDIO,
               0, 12000, sim_audio_mic_profile_run),
        SIM_TC("audio.codec_loopback", "Codec loopback", TEST_DOM_AUDIO,
               0, 12000, sim_audio_codec_lb_profile_run),
        SIM_TC("audio.i2s_loopback", "I2S int loopback", TEST_DOM_AUDIO,
               0, 20000, sim_audio_i2s_lb_profile_run),
        SIM_TC("audio.record_playback", "Record & play", TEST_DOM_AUDIO,
               TEST_F_INTERACTIVE | TEST_F_NEEDS_SD, 45000,
               sim_audio_record_profile_run),
        /* storage */
        SIM_TC("storage.sd_rw", "TF write-verify", TEST_DOM_STORAGE,
               TEST_F_NEEDS_SD, 10000, sim_run_storage_rw_gated),
        SIM_TC("storage.usb_msc_rw", "USB disk verify", TEST_DOM_STORAGE,
               TEST_F_INTERACTIVE | TEST_F_NEEDS_USB_DISK, 45000,
               sim_storage_usb_profile_run),
        SIM_TC("storage.typec_status", "Type-C status", TEST_DOM_STORAGE,
               0, 8000, sim_storage_typec_profile_run),
        SIM_TC("storage.usb_enum", "USB device enum", TEST_DOM_STORAGE,
               TEST_F_INTERACTIVE, 30000, sim_storage_enum_profile_run),
        /* network */
        SIM_TC("net.wifi_scan", "WiFi scan", TEST_DOM_NETWORK,
               0, 20000, sim_net_wifi_scan_profile_run),
        SIM_TC("net.wifi_connect", "WiFi connect", TEST_DOM_NETWORK,
               TEST_F_INTERACTIVE, 60000, sim_net_wifi_conn_profile_run),
        SIM_TC("net.ble_smoke", "BLE ctrl smoke", TEST_DOM_NETWORK,
               0, 10000, sim_net_ble_smoke_profile_run),
        SIM_TC("net.ble_scan", "BLE scan 10s", TEST_DOM_NETWORK,
               TEST_F_LONG, 20000, sim_net_ble_scan_profile_run),
        /* system */
        SIM_TC("sys.rtc_read", "RTC read", TEST_DOM_SYSTEM,
               0, 5000, sim_sys_rtc_profile_run),
        SIM_TC("sys.rtc_alarm", "RTC alarm 1min", TEST_DOM_SYSTEM,
               TEST_F_LONG | TEST_F_NO_RUNALL, 90000,
               sim_sys_rtc_alarm_profile_run),
        SIM_TC("sys.shared_irq", "Shared IRQ line", TEST_DOM_SYSTEM,
               0, 10000, sim_sys_irq_profile_run),
        SIM_TC("sys.buttons", "BOOT+PWR keys", TEST_DOM_SYSTEM,
               TEST_F_INTERACTIVE, 90000, sim_sys_buttons_profile_run),
        SIM_TC("sys.led_rgb", "RGB LED colors", TEST_DOM_SYSTEM,
               TEST_F_INTERACTIVE, 60000, sim_sys_led_profile_run),
        SIM_TC("sys.temp", "SoC temperature", TEST_DOM_SYSTEM,
               0, 10000, sim_sys_temp_profile_run),
        /* memory */
        SIM_TC("mem.flash_size", "Flash 16MB check", TEST_DOM_MEMORY,
               0, 5000, sim_mem_flash_profile_run),
        SIM_TC("mem.psram_pattern", "PSRAM 64KiB R/W", TEST_DOM_MEMORY,
               0, 10000, sim_mem_psram_profile_run),
        SIM_TC("mem.psram_alias_probe", "PSRAM alias probe", TEST_DOM_MEMORY,
               0, 30000, sim_mem_alias_profile_run),
        SIM_TC("mem.bandwidth", "Bandwidth bench", TEST_DOM_MEMORY,
               TEST_F_LONG, 60000, sim_mem_bw_profile_run),
        /* accel */
        SIM_TC("accel.jpeg", "HW JPEG encode", TEST_DOM_ACCEL,
               0, 30000, sim_accel_jpeg_profile_run),
        SIM_TC("accel.cordic", "CORDIC sin/cos", TEST_DOM_ACCEL,
               0, 15000, sim_accel_cordic_profile_run),
        SIM_TC("accel.ppa", "PPA rotate 90", TEST_DOM_ACCEL,
               0, 30000, sim_accel_ppa_profile_run),
        SIM_TC("accel.bitscrambler", "BitScrambler xpose", TEST_DOM_ACCEL,
               0, 30000, sim_accel_bits_profile_run),
        SIM_TC("accel.asrc", "ASRC 16k to 48k", TEST_DOM_ACCEL,
               0, 30000, sim_accel_asrc_profile_run),
        /* power */
        SIM_TC("power.pmic_status", "PMIC status", TEST_DOM_POWER,
               0, 5000, sim_power_pmic_profile_run),
        SIM_TC("power.rails_dump", "Rails snapshot", TEST_DOM_POWER,
               0, 10000, sim_power_rails_profile_run),
        SIM_TC("power.charge_check", "Charge grading", TEST_DOM_POWER,
               0, 5000, sim_power_charge_profile_run),
        SIM_TC("power.screen_off_wake", "Screen off & wake", TEST_DOM_POWER,
               TEST_F_INTERACTIVE, 80000, sim_power_screen_profile_run),
        SIM_TC("power.deep_sleep", "Deep sleep 1min", TEST_DOM_POWER,
               TEST_F_INTERACTIVE | TEST_F_NO_RUNALL, 60000,
               sim_power_deep_profile_run),
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        (void)test_register(&cases[i]);
    }
}
