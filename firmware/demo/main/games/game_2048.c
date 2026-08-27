/*
 * Candis-S31 watch demo - 2048 game.
 *
 * Classic 4x4 board. Input: full-screen swipe (LV_EVENT_GESTURE, the basic
 * single-pointer gesture built into the LVGL indev) plus four virtual
 * direction keys. Only the board area is invalidated on a move; the board
 * is 325 px tall, inside the local-update budget. The best score is kept
 * in a file-static so it survives leaving and re-entering the game.
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

#define G2048_SIZE      4
#define G2048_CELLS     (G2048_SIZE * G2048_SIZE)
#define G2048_CELL      75
#define G2048_GAP       5
#define G2048_BOARD_PX  (G2048_SIZE * G2048_CELL + (G2048_SIZE + 1) * G2048_GAP)
#define G2048_BOARD_X   ((460 - G2048_BOARD_PX) / 2)
#define G2048_BOARD_Y   64
#define G2048_WIN_EXP   11 /* 2^11 = 2048 */

typedef struct {
    bool active;
    lv_obj_t *cell_bg[G2048_CELLS];
    lv_obj_t *cell_lbl[G2048_CELLS];
    lv_obj_t *lbl_score;
    lv_obj_t *lbl_best;
    uint8_t grid[G2048_CELLS]; /* exponent of two, 0 = empty */
    int score;
    bool won_shown;
    bool over;
} g2048_state_t;

static g2048_state_t s;
static int s_best; /* persists across game sessions */

static const uint32_t s_tile_color[G2048_WIN_EXP + 1] = {
    0x4A4A55, 0x5A5A66, 0xC77A2E, 0xC96A20, 0xD55A28, 0xD94A2A,
    0xC9B458, 0xC9B040, 0xC9AC30, 0xC9A820, 0xE0C020, 0x30D158,
};

/* ------------------------------------------------------------------ */
/* Core logic                                                          */
/* ------------------------------------------------------------------ */

/* Cell indices of one line, ordered from the edge the tiles move toward. */
static void g2048_line_indices(lv_dir_t dir, int line, int idx[4])
{
    for (int i = 0; i < 4; ++i) {
        switch (dir) {
        case LV_DIR_LEFT:
            idx[i] = line * 4 + i;
            break;
        case LV_DIR_RIGHT:
            idx[i] = line * 4 + (3 - i);
            break;
        case LV_DIR_TOP:
            idx[i] = i * 4 + line;
            break;
        default: /* LV_DIR_BOTTOM */
            idx[i] = (3 - i) * 4 + line;
            break;
        }
    }
}

static void g2048_spawn(void)
{
    int empty[G2048_CELLS];
    int count = 0;
    for (int i = 0; i < G2048_CELLS; ++i) {
        if (s.grid[i] == 0) {
            empty[count++] = i;
        }
    }
    if (count == 0) {
        return;
    }
    const int pick = empty[rand() % count];
    s.grid[pick] = (rand() % 10 == 0) ? 2 : 1; /* 10% four, 90% two */
}

static bool g2048_is_over(void)
{
    for (int i = 0; i < G2048_CELLS; ++i) {
        if (s.grid[i] == 0) {
            return false;
        }
    }
    for (int r = 0; r < G2048_SIZE; ++r) {
        for (int c = 0; c < G2048_SIZE; ++c) {
            const uint8_t v = s.grid[r * 4 + c];
            if (c + 1 < G2048_SIZE && s.grid[r * 4 + c + 1] == v) {
                return false;
            }
            if (r + 1 < G2048_SIZE && s.grid[(r + 1) * 4 + c] == v) {
                return false;
            }
        }
    }
    return true;
}

static void g2048_render(void)
{
    for (int i = 0; i < G2048_CELLS; ++i) {
        const uint8_t exp = s.grid[i];
        if (exp == 0) {
            lv_obj_set_style_bg_color(s.cell_bg[i], lv_color_hex(0x14141A), 0);
            lv_label_set_text(s.cell_lbl[i], "");
        } else {
            const int ci = exp - 1 < G2048_WIN_EXP + 1 ? exp - 1 : G2048_WIN_EXP;
            lv_obj_set_style_bg_color(s.cell_bg[i],
                                      lv_color_hex(s_tile_color[ci]), 0);
            const lv_font_t *font = exp <= 6 ? ui_font_mid() :
                                    exp <= 11 ? ui_font_title() :
                                    exp <= 14 ? ui_font_body() :
                                                &lv_font_montserrat_16;
            lv_obj_set_style_text_font(s.cell_lbl[i], font, 0);
            lv_label_set_text_fmt(s.cell_lbl[i], "%d", 1 << exp);
        }
    }
    lv_label_set_text_fmt(s.lbl_score, "Score %d", s.score);
    lv_label_set_text_fmt(s.lbl_best, "Best %d", s_best);
}

static bool g2048_move(lv_dir_t dir)
{
    if (!s.active || s.over) {
        return false;
    }
    bool moved = false;
    int gained = 0;

    for (int line = 0; line < G2048_SIZE; ++line) {
        int idx[4];
        g2048_line_indices(dir, line, idx);

        uint8_t vals[4] = { 0 };
        int n = 0;
        for (int i = 0; i < 4; ++i) {
            if (s.grid[idx[i]] != 0) {
                vals[n++] = s.grid[idx[i]];
            }
        }
        for (int i = 0; i + 1 < n; ++i) {
            if (vals[i] == vals[i + 1]) {
                vals[i] = (uint8_t)(vals[i] + 1);
                gained += 1 << vals[i];
                if (vals[i] == G2048_WIN_EXP && !s.won_shown) {
                    s.won_shown = true;
                    ui_toast("2048 reached!");
                }
                for (int j = i + 1; j + 1 < n; ++j) {
                    vals[j] = vals[j + 1];
                }
                --n;
            }
        }
        for (int i = 0; i < 4; ++i) {
            const uint8_t next = i < n ? vals[i] : 0;
            if (s.grid[idx[i]] != next) {
                s.grid[idx[i]] = next;
                moved = true;
            }
        }
    }

    if (moved) {
        s.score += gained;
        if (s.score > s_best) {
            s_best = s.score;
        }
        g2048_spawn();
        g2048_render(); /* invalidates only the changed cell areas */
        if (g2048_is_over()) {
            s.over = true;
            char msg[48];
            snprintf(msg, sizeof(msg), "Game over, score %d", s.score);
            ui_toast(msg);
        }
    }
    return moved;
}

static void g2048_new_game(void)
{
    if (!s.active) {
        return;
    }
    for (int i = 0; i < G2048_CELLS; ++i) {
        s.grid[i] = 0;
    }
    s.score = 0;
    s.won_shown = false;
    s.over = false;
    g2048_spawn();
    g2048_spawn();
    g2048_render();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void g2048_gesture_cb(lv_event_t *event)
{
    if (!s.active) {
        return;
    }
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev) {
        return;
    }
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_TOP ||
            dir == LV_DIR_BOTTOM) {
        g2048_move(dir);
    }
}

static void g2048_key_cb(lv_event_t *event)
{
    g2048_move((lv_dir_t)(intptr_t)lv_event_get_user_data(event));
}

static void g2048_new_cb(lv_event_t *event)
{
    (void)event;
    g2048_new_game();
}

static void g2048_back_cb(lv_event_t *event)
{
    (void)event;
    ui_nav_back();
}

static void g2048_root_delete_cb(lv_event_t *event)
{
    (void)event;
    s.active = false;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static lv_obj_t *header_button(lv_obj_t *parent, const char *text,
                               lv_coord_t x, lv_coord_t width,
                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, width, 56);
    lv_obj_set_pos(btn, x, 4);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

lv_obj_t *game_2048_create(void)
{
    static bool s_rng_seeded;
    s = (g2048_state_t){ .active = true };
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
    lv_obj_add_event_cb(root, g2048_root_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(root, g2048_gesture_cb, LV_EVENT_GESTURE, NULL);

    header_button(root, LV_SYMBOL_LEFT, 4, 56, g2048_back_cb);
    header_button(root, "New game", 356, 100, g2048_new_cb);

    s.lbl_score = lv_label_create(root);
    lv_obj_set_pos(s.lbl_score, 72, 8);
    s.lbl_best = lv_label_create(root);
    lv_obj_set_pos(s.lbl_best, 72, 30);
    lv_obj_set_style_text_color(s.lbl_best, lv_color_hex(UI_COL_TEXT_DIM), 0);

    lv_obj_t *board = lv_obj_create(root);
    lv_obj_set_size(board, G2048_BOARD_PX, G2048_BOARD_PX);
    lv_obj_set_pos(board, G2048_BOARD_X, G2048_BOARD_Y);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(board, 10, 0);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_set_style_pad_all(board, 0, 0);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    /* Swipes that start on the board bubble up to the root gesture cb. */
    lv_obj_add_flag(board, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < G2048_CELLS; ++i) {
        const int r = i / G2048_SIZE, c = i % G2048_SIZE;
        lv_obj_t *cell = lv_obj_create(board);
        lv_obj_set_size(cell, G2048_CELL, G2048_CELL);
        lv_obj_set_pos(cell, G2048_GAP + c * (G2048_CELL + G2048_GAP),
                       G2048_GAP + r * (G2048_CELL + G2048_GAP));
        lv_obj_set_style_bg_color(cell, lv_color_hex(0x14141A), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        s.cell_bg[i] = cell;

        lv_obj_t *label = lv_label_create(cell);
        lv_obj_set_style_text_font(label, ui_font_mid(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_COL_TEXT), 0);
        lv_obj_set_width(label, G2048_CELL - 8);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_center(label);
        s.cell_lbl[i] = label;
    }

    /* Virtual direction keys under the board. */
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
        lv_obj_add_event_cb(btn, g2048_key_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)keys[i].dir);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, keys[i].symbol);
        lv_obj_center(label);
    }

    g2048_new_game();
    return root;
}
