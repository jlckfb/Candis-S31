/*
 * Candis-S31 watch demo - breakout (brick breaker) game.
 *
 * 428x300 board in the lower screen area, lv_timer driven at 60 Hz
 * (16 ms). Ball, paddle and bricks are all self-drawn in the board's
 * LV_EVENT_DRAW_MAIN handler, so one lv_obj_invalidate() per step marks
 * the only dirty area - the FULL-frame render pipeline still composites
 * the whole screen, local semantics is the contract (same discipline as
 * snake/2048).
 *
 * Input: touch drag on the board moves the paddle (LV_EVENT_PRESSING,
 * indev point), tap launches a served ball. 3 lives, score, levels with
 * a growing number of brick rows; clearing the top level wins. Game
 * over / win show an overlay with a restart button; the top-left button
 * (>= 56 px) leaves the game like the other games do.
 *
 * The serve angle uses esp_random() straight from the hardware RNG, so
 * no seeding bookkeeping is needed at all.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_random.h"

#include "demo_apps.h"
#include "ui/ui_manager.h"

#define BRK_BOARD_W      428
#define BRK_BOARD_H      300
#define BRK_BOARD_X      ((460 - BRK_BOARD_W) / 2)
#define BRK_BOARD_Y      88
#define BRK_STEP_MS      16   /* 60 Hz */

#define BRK_COLS         8
#define BRK_MAX_ROWS     6
#define BRK_BRICK_W      50
#define BRK_BRICK_H      14
#define BRK_BRICK_GAP_X  2
#define BRK_BRICK_PITCH  16   /* brick height + vertical gap */
#define BRK_FIELD_W      (BRK_COLS * BRK_BRICK_W + \
                          (BRK_COLS - 1) * BRK_BRICK_GAP_X)
#define BRK_FIELD_X      ((BRK_BOARD_W - BRK_FIELD_W) / 2)
#define BRK_FIELD_Y      8

#define BRK_PADDLE_W     64
#define BRK_PADDLE_H     8
#define BRK_PADDLE_Y     (BRK_BOARD_H - 20)

#define BRK_BALL_SIZE    8
#define BRK_LIVES        3
#define BRK_MAX_LEVEL    6
#define BRK_SPEED_BASE   4.0f /* px per frame at 60 Hz */
#define BRK_SPEED_STEP   0.5f /* per level */
#define BRK_SPEED_MAX    6.0f

typedef enum {
    BRK_SERVE = 0,   /* ball rides the paddle, tap to launch */
    BRK_RUNNING,
    BRK_OVER,
    BRK_WON,
} brk_phase_t;

typedef struct {
    bool active;
    lv_obj_t *board;
    lv_obj_t *lbl_score;
    lv_obj_t *lbl_level;
    lv_obj_t *lbl_lives;
    lv_obj_t *overlay;
    lv_obj_t *overlay_lbl;
    lv_timer_t *timer;

    brk_phase_t phase;
    int score;
    int level;
    int lives;
    int bricks_left;
    uint8_t bricks[BRK_MAX_ROWS][BRK_COLS];

    int paddle_x;            /* board-local left edge */
    float ball_x, ball_y;    /* board-local top-left */
    float vx, vy;
} brk_state_t;

static brk_state_t s;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static float brk_speed(void)
{
    const float speed =
        BRK_SPEED_BASE + BRK_SPEED_STEP * (float)(s.level - 1);
    return speed > BRK_SPEED_MAX ? BRK_SPEED_MAX : speed;
}

/* Brick wall for s.level: 3 rows on level 1, one more per level, capped
 * at BRK_MAX_ROWS. */
static void brk_build_level(void)
{
    int rows = s.level + 2;
    if (rows > BRK_MAX_ROWS) {
        rows = BRK_MAX_ROWS;
    }
    s.bricks_left = 0;
    for (int r = 0; r < BRK_MAX_ROWS; ++r) {
        for (int c = 0; c < BRK_COLS; ++c) {
            s.bricks[r][c] = (uint8_t)(r < rows ? 1 : 0);
            if (r < rows) {
                ++s.bricks_left;
            }
        }
    }
}

static void brk_hud_refresh(void)
{
    lv_label_set_text_fmt(s.lbl_score, "Score %d", s.score);
    lv_label_set_text_fmt(s.lbl_level, "Level %d/%d", s.level,
                          BRK_MAX_LEVEL);
    lv_label_set_text_fmt(s.lbl_lives, "Lives %d", s.lives);
}

static void brk_overlay_show(bool win)
{
    lv_label_set_text_fmt(s.overlay_lbl,
                          win ? "You win!\nFinal score %d" :
                                "Game over\nScore %d",
                          s.score);
    lv_obj_remove_flag(s.overlay, LV_OBJ_FLAG_HIDDEN);
}

static void brk_serve(void)
{
    s.phase = BRK_SERVE;
    s.ball_x = (float)(s.paddle_x + BRK_PADDLE_W / 2 - BRK_BALL_SIZE / 2);
    s.ball_y = (float)(BRK_PADDLE_Y - BRK_BALL_SIZE - 1);
    s.vx = 0.0f;
    s.vy = 0.0f;
}

static void brk_launch(void)
{
    /* Upward serve with a random horizontal component. */
    const float speed = brk_speed();
    const float r = (float)(esp_random() % 1000) / 1000.0f; /* 0..1 */
    s.vx = (r - 0.5f) * speed * 1.2f;
    float vy2 = speed * speed - s.vx * s.vx;
    if (vy2 < 1.0f) {
        /* Keep a real upward component even at extreme angles. */
        vy2 = 1.0f;
        s.vx = s.vx > 0.0f ? 1.0f : -1.0f;
    }
    s.vy = -sqrtf(vy2);
    if (s.vy > -2.0f) {
        s.vy = -2.0f; /* never a nearly-horizontal serve */
    }
    s.phase = BRK_RUNNING;
}

static void brk_reset(bool full)
{
    if (full) {
        s.score = 0;
        s.level = 1;
        s.lives = BRK_LIVES;
    }
    s.paddle_x = (BRK_BOARD_W - BRK_PADDLE_W) / 2;
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_HIDDEN);
    brk_build_level();
    brk_serve();
    brk_hud_refresh();
}

/* ------------------------------------------------------------------ */
/* Step + draw                                                         */
/* ------------------------------------------------------------------ */

static void brk_brick_area(int row, int col, lv_area_t *out, int bx1, int by1)
{
    out->x1 = bx1 + BRK_FIELD_X + col * (BRK_BRICK_W + BRK_BRICK_GAP_X);
    out->y1 = by1 + BRK_FIELD_Y + row * BRK_BRICK_PITCH;
    out->x2 = out->x1 + BRK_BRICK_W - 1;
    out->y2 = out->y1 + BRK_BRICK_H - 1;
}

static void brk_next_level(void)
{
    s.score += 100;
    ++s.level;
    if (s.level > BRK_MAX_LEVEL) {
        lv_timer_pause(s.timer);
        s.phase = BRK_WON;
        brk_overlay_show(true);
        brk_hud_refresh();
        return;
    }
    brk_build_level();
    brk_serve();
    brk_hud_refresh();
}

static void brk_lose_ball(void)
{
    --s.lives;
    brk_hud_refresh();
    if (s.lives <= 0) {
        lv_timer_pause(s.timer);
        s.phase = BRK_OVER;
        brk_overlay_show(false);
        return;
    }
    brk_serve();
}

/* AABB overlap test, ball vs rect (all board-local). */
static bool brk_overlaps(float bx, float by, int rx, int ry, int rw, int rh)
{
    return bx < (float)(rx + rw) && bx + BRK_BALL_SIZE > (float)rx &&
           by < (float)(ry + rh) && by + BRK_BALL_SIZE > (float)ry;
}

static void brk_step_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s.active || s.phase != BRK_RUNNING) {
        return;
    }

    /* Sub-step so a 6 px/frame ball never tunnels through the 14 px
     * bricks (max ~2 px per sub-step). */
    const int steps = (int)(brk_speed() / 3.0f) + 1;
    const float fx = s.vx / (float)steps;
    const float fy = s.vy / (float)steps;

    for (int step = 0; step < steps && s.phase == BRK_RUNNING; ++step) {
        s.ball_x += fx;
        s.ball_y += fy;

        /* Walls. */
        if (s.ball_x < 0.0f) {
            s.ball_x = 0.0f;
            s.vx = -s.vx;
        } else if (s.ball_x > BRK_BOARD_W - BRK_BALL_SIZE) {
            s.ball_x = (float)(BRK_BOARD_W - BRK_BALL_SIZE);
            s.vx = -s.vx;
        }
        if (s.ball_y < 0.0f) {
            s.ball_y = 0.0f;
            s.vy = -s.vy;
        }

        /* Lost below the board. */
        if (s.ball_y > BRK_BOARD_H) {
            brk_lose_ball();
            break;
        }

        /* Paddle: reflect only on the way down, angle follows the hit
         * position relative to the paddle center. */
        if (s.vy > 0.0f &&
                brk_overlaps(s.ball_x, s.ball_y, s.paddle_x, BRK_PADDLE_Y,
                             BRK_PADDLE_W, BRK_PADDLE_H)) {
            const float center = (float)(s.paddle_x + BRK_PADDLE_W / 2);
            float rel = ((s.ball_x + BRK_BALL_SIZE / 2.0f) - center) /
                        (float)(BRK_PADDLE_W / 2);
            if (rel > 1.0f) {
                rel = 1.0f;
            } else if (rel < -1.0f) {
                rel = -1.0f;
            }
            s.vx = rel * brk_speed() * 0.75f;
            float vy2 = brk_speed() * brk_speed() - s.vx * s.vx;
            if (vy2 < 1.0f) {
                vy2 = 1.0f;
            }
            s.vy = -sqrtf(vy2);
            s.ball_y = (float)(BRK_PADDLE_Y - BRK_BALL_SIZE - 1);
        }

        /* Bricks: reflect on the axis of least penetration, at most one
         * brick per sub-step. */
        bool brick_hit = false;
        for (int r = 0;
                r < BRK_MAX_ROWS && s.phase == BRK_RUNNING && !brick_hit;
                ++r) {
            for (int c = 0; c < BRK_COLS; ++c) {
                if (!s.bricks[r][c]) {
                    continue;
                }
                const int rx = BRK_FIELD_X +
                               c * (BRK_BRICK_W + BRK_BRICK_GAP_X);
                const int ry = BRK_FIELD_Y + r * BRK_BRICK_PITCH;
                if (!brk_overlaps(s.ball_x, s.ball_y, rx, ry,
                                  BRK_BRICK_W, BRK_BRICK_H)) {
                    continue;
                }
                s.bricks[r][c] = 0;
                --s.bricks_left;
                s.score += 10;

                const float overlap_x =
                    (s.ball_x + BRK_BALL_SIZE / 2.0f < rx + BRK_BRICK_W / 2.0f)
                        ? (s.ball_x + BRK_BALL_SIZE) - rx
                        : (rx + BRK_BRICK_W) - s.ball_x;
                const float overlap_y =
                    (s.ball_y + BRK_BALL_SIZE / 2.0f < ry + BRK_BRICK_H / 2.0f)
                        ? (s.ball_y + BRK_BALL_SIZE) - ry
                        : (ry + BRK_BRICK_H) - s.ball_y;
                if (overlap_x < overlap_y) {
                    s.vx = -s.vx;
                } else {
                    s.vy = -s.vy;
                }
                lv_label_set_text_fmt(s.lbl_score, "Score %d", s.score);
                brick_hit = true;
                if (s.bricks_left == 0) {
                    brk_next_level();
                }
                break;
            }
        }
    }

    lv_obj_invalidate(s.board); /* the only dirty area per step */
}

static void brk_draw_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t board;
    lv_obj_get_coords(lv_event_get_target_obj(event), &board);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 3;

    /* Bricks: one dimmer tier per row, anchored on the accent hue. */
    static const uint32_t row_colors[BRK_MAX_ROWS] = {
        UI_COLOR_ACCENT, 0x3E86D6, 0x346FB0, 0x2B5A8E,
        0x244870, 0x1D3A5A,
    };
    lv_area_t area;
    for (int r = 0; r < BRK_MAX_ROWS; ++r) {
        for (int c = 0; c < BRK_COLS; ++c) {
            if (!s.bricks[r][c]) {
                continue;
            }
            brk_brick_area(r, c, &area, board.x1, board.y1);
            dsc.bg_color = lv_color_hex(row_colors[r]);
            lv_draw_rect(layer, &dsc, &area);
        }
    }

    /* Paddle. */
    dsc.radius = 4;
    dsc.bg_color = lv_color_hex(UI_COLOR_TEXT);
    area.x1 = board.x1 + s.paddle_x;
    area.y1 = board.y1 + BRK_PADDLE_Y;
    area.x2 = area.x1 + BRK_PADDLE_W - 1;
    area.y2 = area.y1 + BRK_PADDLE_H - 1;
    lv_draw_rect(layer, &dsc, &area);

    /* Ball. */
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.bg_color = lv_color_hex(UI_COLOR_WARN);
    area.x1 = board.x1 + (int)s.ball_x;
    area.y1 = board.y1 + (int)s.ball_y;
    area.x2 = area.x1 + BRK_BALL_SIZE - 1;
    area.y2 = area.y1 + BRK_BALL_SIZE - 1;
    lv_draw_rect(layer, &dsc, &area);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Touch drag on the board: move the paddle to keep it under the finger. */
static void brk_pressing_cb(lv_event_t *event)
{
    if (!s.active || s.phase == BRK_OVER || s.phase == BRK_WON) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t board;
    lv_obj_get_coords(s.board, &board);
    const int local_x = pt.x - board.x1;
    int paddle_x = local_x - BRK_PADDLE_W / 2;
    if (paddle_x < 0) {
        paddle_x = 0;
    } else if (paddle_x > BRK_BOARD_W - BRK_PADDLE_W) {
        paddle_x = BRK_BOARD_W - BRK_PADDLE_W;
    }
    if (paddle_x != s.paddle_x) {
        s.paddle_x = paddle_x;
        if (s.phase == BRK_SERVE) {
            s.ball_x =
                (float)(s.paddle_x + BRK_PADDLE_W / 2 - BRK_BALL_SIZE / 2);
        }
        lv_obj_invalidate(s.board);
    }
}

/* Tap: launch a served ball. */
static void brk_board_click_cb(lv_event_t *event)
{
    (void)event;
    if (!s.active) {
        return;
    }
    if (s.phase == BRK_SERVE) {
        brk_launch();
        lv_obj_invalidate(s.board);
    }
}

static void brk_restart_cb(lv_event_t *event)
{
    (void)event;
    if (!s.active) {
        return;
    }
    /* Works mid-game and from the terminal overlay alike. */
    lv_timer_resume(s.timer);
    brk_reset(true);
    lv_obj_invalidate(s.board);
}

static void brk_back_cb(lv_event_t *event)
{
    (void)event;
    ui_nav_back();
}

static void brk_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
    if (s.timer != NULL) {
        lv_timer_delete(s.timer);
        s.timer = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

lv_obj_t *game_breakout_create(void)
{
    s = (brk_state_t){ .active = true };

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, brk_root_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_pos(back, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(back, brk_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    s.lbl_score = lv_label_create(root);
    lv_label_set_text(s.lbl_score, "Score 0");
    lv_obj_set_pos(s.lbl_score, 72, 8);
    s.lbl_level = lv_label_create(root);
    lv_label_set_text(s.lbl_level, "Level 1/6");
    lv_obj_set_pos(s.lbl_level, 72, 32);
    lv_obj_set_style_text_color(s.lbl_level, lv_color_hex(UI_COLOR_TEXT_DIM),
                                0);

    lv_obj_t *restart = lv_button_create(root);
    lv_obj_set_size(restart, 100, 56);
    lv_obj_set_pos(restart, 356, 4);
    lv_obj_set_style_bg_color(restart, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_add_event_cb(restart, brk_restart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *restart_lbl = lv_label_create(restart);
    lv_label_set_text(restart_lbl, "Restart");
    lv_obj_center(restart_lbl);

    s.lbl_lives = lv_label_create(root);
    lv_label_set_text(s.lbl_lives, "Lives 3");
    lv_obj_set_pos(s.lbl_lives, 236, 22);
    lv_obj_set_style_text_color(s.lbl_lives, lv_color_hex(UI_COLOR_WARN), 0);

    s.board = lv_obj_create(root);
    lv_obj_set_size(s.board, BRK_BOARD_W, BRK_BOARD_H);
    lv_obj_set_pos(s.board, BRK_BOARD_X, BRK_BOARD_Y);
    lv_obj_set_style_bg_color(s.board, lv_color_hex(0x0A0A0E), 0);
    lv_obj_set_style_bg_opa(s.board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s.board, lv_color_hex(0x2A2A32), 0);
    lv_obj_set_style_border_width(s.board, 1, 0);
    lv_obj_set_style_radius(s.board, 4, 0);
    lv_obj_set_style_pad_all(s.board, 0, 0);
    lv_obj_remove_flag(s.board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.board, brk_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s.board, brk_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s.board, brk_board_click_cb, LV_EVENT_CLICKED, NULL);

    /* Game over / win overlay (hidden until a terminal phase). */
    s.overlay = lv_obj_create(s.board);
    lv_obj_set_size(s.overlay, BRK_BOARD_W, BRK_BOARD_H);
    lv_obj_center(s.overlay);
    lv_obj_set_style_bg_color(s.overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s.overlay, 0, 0);
    lv_obj_set_style_radius(s.overlay, 0, 0);
    lv_obj_set_style_pad_all(s.overlay, 0, 0);
    lv_obj_remove_flag(s.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_HIDDEN);

    s.overlay_lbl = lv_label_create(s.overlay);
    lv_label_set_text(s.overlay_lbl, "");
    lv_obj_set_style_text_align(s.overlay_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s.overlay_lbl);

    lv_obj_t *again = lv_button_create(s.overlay);
    lv_obj_set_size(again, 120, 56);
    lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(again, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_add_event_cb(again, brk_restart_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *again_lbl = lv_label_create(again);
    lv_label_set_text(again_lbl, "Play again");
    lv_obj_center(again_lbl);

    s.timer = lv_timer_create(brk_step_cb, BRK_STEP_MS, NULL);
    brk_reset(true);
    return root;
}
