/*
 * Candis-S31 watch demo - snake game.
 *
 * 20x20 grid, lv_timer driven (~6 fps start, faster with every food).
 * Input: full-screen swipe (LV_EVENT_GESTURE) plus four virtual keys.
 * The board is a single object painted in its LV_EVENT_DRAW_MAIN handler,
 * so one step invalidates exactly the 320x320 board area (<= 360 rows,
 * inside the 60 fps dirty-region budget) instead of hundreds of widgets.
 * All game state lives on the LVGL thread; no extra task is created.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_random.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

#define SNAKE_GRID       20
#define SNAKE_CELLS      (SNAKE_GRID * SNAKE_GRID)
#define SNAKE_CELL_PX    16
#define SNAKE_BOARD_PX   (SNAKE_GRID * SNAKE_CELL_PX)
#define SNAKE_BOARD_X    ((460 - SNAKE_BOARD_PX) / 2)
#define SNAKE_BOARD_Y    68
#define SNAKE_START_MS   160 /* ~6 fps */
#define SNAKE_MIN_MS     90  /* ~11 fps cap */
#define SNAKE_SPEEDUP_MS 4   /* per eaten food */

typedef struct {
    int8_t x;
    int8_t y;
} snake_pt_t;

typedef struct {
    bool active;
    lv_obj_t *board;
    lv_obj_t *lbl_score;
    lv_timer_t *timer;
    snake_pt_t body[SNAKE_CELLS]; /* ring buffer of occupied cells */
    int head;                     /* ring index of the head */
    int length;
    snake_pt_t food;
    lv_dir_t dir;                 /* direction applied on the last step */
    lv_dir_t pending;             /* buffered direction request */
    int score;
    int period_ms;
    bool over;
} snake_state_t;

static snake_state_t s;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool snake_dir_is_opposite(lv_dir_t a, lv_dir_t b)
{
    return (a == LV_DIR_LEFT && b == LV_DIR_RIGHT) ||
           (a == LV_DIR_RIGHT && b == LV_DIR_LEFT) ||
           (a == LV_DIR_TOP && b == LV_DIR_BOTTOM) ||
           (a == LV_DIR_BOTTOM && b == LV_DIR_TOP);
}

static void snake_dir_delta(lv_dir_t dir, int8_t *dx, int8_t *dy)
{
    switch (dir) {
    case LV_DIR_LEFT:
        *dx = -1;
        *dy = 0;
        break;
    case LV_DIR_RIGHT:
        *dx = 1;
        *dy = 0;
        break;
    case LV_DIR_TOP:
        *dx = 0;
        *dy = -1;
        break;
    default: /* LV_DIR_BOTTOM */
        *dx = 0;
        *dy = 1;
        break;
    }
}

static void snake_spawn_food(void)
{
    /* Rejection sampling; the board is never full while food is missing. */
    for (int attempt = 0; attempt < 512; ++attempt) {
        const snake_pt_t cand = {
            .x = (int8_t)(rand() % SNAKE_GRID),
            .y = (int8_t)(rand() % SNAKE_GRID),
        };
        bool hit = false;
        for (int i = 0; i < s.length; ++i) {
            const int idx = (s.head - i + SNAKE_CELLS) % SNAKE_CELLS;
            if (s.body[idx].x == cand.x && s.body[idx].y == cand.y) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            s.food = cand;
            return;
        }
    }
}

static void snake_reset(void)
{
    s.head = 2;
    s.length = 3;
    for (int i = 0; i < s.length; ++i) {
        /* head at ring index 2, body extends to the left */
        s.body[i].x = (int8_t)(SNAKE_GRID / 2 - (s.head - i));
        s.body[i].y = (int8_t)(SNAKE_GRID / 2);
    }
    s.dir = LV_DIR_RIGHT;
    s.pending = LV_DIR_RIGHT;
    s.score = 0;
    s.period_ms = SNAKE_START_MS;
    s.over = false;
    snake_spawn_food();
    lv_timer_set_period(s.timer, s.period_ms);
    lv_timer_resume(s.timer);
    lv_label_set_text_fmt(s.lbl_score, "Score %d", s.score);
    lv_obj_invalidate(s.board);
}

/* ------------------------------------------------------------------ */
/* Step + draw                                                         */
/* ------------------------------------------------------------------ */

static void snake_game_over(void)
{
    s.over = true;
    lv_timer_pause(s.timer);
    char msg[48];
    snprintf(msg, sizeof(msg), "Game over, score %d", s.score);
    ui_toast(msg);
}

static void snake_step_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active || s.over) {
        return;
    }

    if (!snake_dir_is_opposite(s.pending, s.dir)) {
        s.dir = s.pending;
    }
    int8_t dx, dy;
    snake_dir_delta(s.dir, &dx, &dy);
    const snake_pt_t *head = &s.body[s.head];
    const snake_pt_t next = {
        .x = (int8_t)(head->x + dx),
        .y = (int8_t)(head->y + dy),
    };

    if (next.x < 0 || next.x >= SNAKE_GRID || next.y < 0 ||
            next.y >= SNAKE_GRID) {
        snake_game_over(); /* hit the wall */
        return;
    }

    const bool eats = next.x == s.food.x && next.y == s.food.y;

    /* Self collision: the tail cell is vacated this tick unless growing. */
    for (int i = 0; i < s.length; ++i) {
        if (!eats && i == s.length - 1) {
            break;
        }
        const int idx = (s.head - i + SNAKE_CELLS) % SNAKE_CELLS;
        if (s.body[idx].x == next.x && s.body[idx].y == next.y) {
            snake_game_over();
            return;
        }
    }

    if (eats) {
        ++s.length;
        s.score += 10;
        lv_label_set_text_fmt(s.lbl_score, "Score %d", s.score);
        if (s.period_ms > SNAKE_MIN_MS) {
            s.period_ms -= SNAKE_SPEEDUP_MS;
            if (s.period_ms < SNAKE_MIN_MS) {
                s.period_ms = SNAKE_MIN_MS;
            }
            lv_timer_set_period(s.timer, s.period_ms);
        }
    }

    s.head = (s.head + 1) % SNAKE_CELLS;
    s.body[s.head] = next;
    if (eats) {
        snake_spawn_food();
    }
    lv_obj_invalidate(s.board); /* the only dirty area per step */
}

static void snake_cell_area(const snake_pt_t *pt, const lv_area_t *board,
                            lv_area_t *out)
{
    out->x1 = board->x1 + pt->x * SNAKE_CELL_PX + 1;
    out->y1 = board->y1 + pt->y * SNAKE_CELL_PX + 1;
    out->x2 = out->x1 + SNAKE_CELL_PX - 3;
    out->y2 = out->y1 + SNAKE_CELL_PX - 3;
}

static void snake_draw_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t board;
    lv_obj_get_coords(lv_event_get_target_obj(event), &board);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 4;
    dsc.bg_opa = LV_OPA_COVER;

    lv_area_t area;
    snake_cell_area(&s.food, &board, &area);
    dsc.bg_color = lv_color_hex(UI_COL_WARN);
    lv_draw_rect(layer, &dsc, &area);

    for (int i = 0; i < s.length; ++i) {
        const int idx = (s.head - i + SNAKE_CELLS) % SNAKE_CELLS;
        snake_cell_area(&s.body[idx], &board, &area);
        dsc.bg_color = lv_color_hex(i == 0 ? UI_COL_PASS : 0x2A9D44);
        lv_draw_rect(layer, &dsc, &area);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void snake_set_dir(lv_dir_t dir)
{
    if (!s.active) {
        return;
    }
    if (s.over) {
        return;
    }
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_TOP ||
            dir == LV_DIR_BOTTOM) {
        s.pending = dir;
    }
}

static void snake_gesture_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    snake_set_dir(lv_indev_get_gesture_dir(indev));
}

static void snake_key_cb(lv_event_t *event)
{
    snake_set_dir((lv_dir_t)(intptr_t)lv_event_get_user_data(event));
}

static void snake_restart_cb(lv_event_t *event)
{
    (void)event;
    if (s.active) {
        snake_reset();
    }
}

static void snake_board_click_cb(lv_event_t *event)
{
    (void)event;
    if (s.active && s.over) {
        snake_reset(); /* tap the board to play again */
    }
}

static void snake_back_cb(lv_event_t *event)
{
    (void)event;
    ui_nav_back();
}

static void snake_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.timer) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *game_snake_create(void)
{
    static bool s_rng_seeded;
    s = (snake_state_t){ .active = true };
    if (!s_rng_seeded) {
        /* Seed once per process from the hardware RNG; re-entries keep
         * drawing from the same sequence (the old per-create timer seed
         * was low entropy and restarted the sequence on every visit). */
        s_rng_seeded = true;
        srand((unsigned)esp_random());
    }

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, snake_root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(root, snake_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_add_event_cb(back, snake_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    s.lbl_score = lv_label_create(root);
    lv_label_set_text(s.lbl_score, "Score 0");
    lv_obj_set_pos(s.lbl_score, 72, 22);

    lv_obj_t *restart = lv_button_create(root);
    lv_obj_set_size(restart, 100, 56);
    lv_obj_set_pos(restart, 356, 4);
    lv_obj_set_style_bg_color(restart, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_add_event_cb(restart, snake_restart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_lbl = lv_label_create(restart);
    lv_label_set_text(restart_lbl, "Restart");
    lv_obj_center(restart_lbl);

    s.board = lv_obj_create(root);
    lv_obj_set_size(s.board, SNAKE_BOARD_PX, SNAKE_BOARD_PX);
    lv_obj_set_pos(s.board, SNAKE_BOARD_X, SNAKE_BOARD_Y);
    lv_obj_set_style_bg_color(s.board, lv_color_hex(0x0A0A0E), 0);
    lv_obj_set_style_bg_opa(s.board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s.board, lv_color_hex(0x2A2A32), 0);
    lv_obj_set_style_border_width(s.board, 1, 0);
    lv_obj_set_style_radius(s.board, 4, 0);
    lv_obj_set_style_pad_all(s.board, 0, 0);
    lv_obj_remove_flag(s.board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.board, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s.board, snake_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s.board, snake_board_click_cb, LV_EVENT_CLICKED, NULL);

    static const struct {
        const char *symbol;
        lv_dir_t dir;
    } keys[4] = {
        { LV_SYMBOL_LEFT, LV_DIR_LEFT },
        { LV_SYMBOL_UP, LV_DIR_TOP },
        { LV_SYMBOL_DOWN, LV_DIR_BOTTOM },
        { LV_SYMBOL_RIGHT, LV_DIR_RIGHT },
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *btn = lv_button_create(root);
        lv_obj_set_size(btn, 60, 56);
        lv_obj_set_pos(btn, 98 + i * 68, 400);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_add_event_cb(btn, snake_key_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)keys[i].dir);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, keys[i].symbol);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_center(label);
    }

    s.timer = lv_timer_create(snake_step_cb, SNAKE_START_MS, NULL);
    snake_reset();
    return root;
}
