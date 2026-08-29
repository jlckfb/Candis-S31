#include <stdio.h>
#include <stdlib.h>
#ifndef __EMSCRIPTEN__
#include <unistd.h>
#else
#include <emscripten.h>
#endif

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_mousewheel.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include "bsp/esp-bsp.h"
#include "sim_ui.h"

#define SIM_WIDTH 460
#define SIM_HEIGHT 460

#ifdef __EMSCRIPTEN__
static void sim_web_step(void *arg)
{
    (void)arg;
    (void)lv_timer_handler();
}
#endif

#ifdef __EMSCRIPTEN__
/* Copy a URL query parameter into a static buffer ("" when absent). */
static void sim_url_param(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    EM_ASM({
        const params = new URLSearchParams(window.location.search);
        const value = params.get(UTF8ToString($0));
        if (value !== null) {
            stringToUTF8(value, $1, $2);
        }
    }, key, out, cap);
}
#endif

static const char *sim_screen_arg(void)
{
#ifdef __EMSCRIPTEN__
    static char s_screen[32];
    sim_url_param("screen", s_screen, sizeof(s_screen));
    return s_screen[0] == '\0' ? NULL : s_screen;
#else
    return getenv("CANDIS_SIM_SCREEN");
#endif
}

static const char *sim_state_arg(void)
{
#ifdef __EMSCRIPTEN__
    static char s_state[32];
    sim_url_param("state", s_state, sizeof(s_state));
    return s_state[0] == '\0' ? NULL : s_state;
#else
    return getenv("CANDIS_SIM_STATE");
#endif
}

int main(void)
{
    lv_init();

    lv_display_t *display = lv_sdl_window_create(SIM_WIDTH, SIM_HEIGHT);
    if (display == NULL) {
        fprintf(stderr, "failed to create the LVGL SDL window\n");
        return EXIT_FAILURE;
    }
    lv_sdl_window_set_resizeable(display, false);
    lv_sdl_window_set_zoom(display, 1.25f);

    lv_indev_t *mouse = lv_sdl_mouse_create();
    (void)lv_sdl_mousewheel_create();
    (void)lv_sdl_keyboard_create();
    sim_bsp_set_input_dev(mouse);

    sim_ui_init(sim_screen_arg(), sim_state_arg());

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(sim_web_step, NULL, 0, true);
#else
    for (;;) {
        const uint32_t wait_ms = lv_timer_handler();
        if (lv_display_get_default() == NULL) {
            break;
        }
        const uint32_t delay_ms = wait_ms == LV_NO_TIMER_READY ? 5 : wait_ms;
        usleep((useconds_t)(delay_ms * 1000U));
    }

    lv_sdl_quit();
#endif
    return EXIT_SUCCESS;
}
