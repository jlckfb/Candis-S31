/*
 * Candis-S31 watch demo - theme tokens (redesign spec section B).
 *
 * Single source of truth for the AMOLED true-black palette (B.1) and the
 * layout grid constants introduced by the redesign (B.3). The pre-existing
 * grid macros UI_STATUS_BAR_HEIGHT / UI_CONTENT_Y / UI_CONTENT_H /
 * UI_SCREEN_PAD / UI_GAP / UI_RADIUS_CARD / UI_TOUCH_MIN /
 * UI_TITLE_ROW_HEIGHT stay in ui_manager.h (identical values, legacy
 * users); new code includes this header instead of open-coding hex.
 *
 * The former UI_COLOR_* names were mechanically renamed to these UI_COL_*
 * tokens and the compatibility aliases removed (WS5 clean cutover).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/* ------------------------------------------------------------------ */
/* B.1 palette (hex, AMOLED true black)                                */
/* ------------------------------------------------------------------ */

#define UI_COL_BG          0x000000  /* global background (unlit) */
#define UI_COL_SURFACE     0x101014  /* card / row fill (level 1) */
#define UI_COL_SURFACE_2   0x181820  /* dialogs, pressed state, bar slot */
#define UI_COL_HAIRLINE    0x26262E  /* 1 px card outline (unified) */
#define UI_COL_ACCENT      0x00CFFF  /* primary accent, electric cyan */
#define UI_COL_ACCENT_DIM  0x0A2E3D  /* accent wash: chip/button fills */
#define UI_COL_VIOLET      0x8A7CFF  /* secondary accent, violet */
#define UI_COL_PASS        0x2FD971  /* PASS / charging / healthy */
#define UI_COL_PASS_DIM    0x0D2B1A  /* PASS badge fill */
#define UI_COL_FAIL        0xFF453A  /* FAIL / error */
#define UI_COL_FAIL_DIM    0x331416  /* FAIL badge fill */
#define UI_COL_WARN        0xFFB020  /* WARN / pending confirm */
#define UI_COL_WARN_DIM    0x33260E  /* WARN badge fill */
#define UI_COL_IDLE        0x55555F  /* NOT_RUN grey-blue */
#define UI_COL_IDLE_DIM    0x17171C  /* NOT RUN chip fill (B.4) */
#define UI_COL_TEXT        0xF2F2F5  /* primary text */
#define UI_COL_TEXT_DIM    0x9A9AA2  /* secondary text */
#define UI_COL_TEXT_WEAK   0x5A5A64  /* hints / placeholders */
#define UI_COL_TRACK       0x23232A  /* slider / arc / switch track (B.4) */

/* ------------------------------------------------------------------ */
/* B.3 layout grid (extensions; base grid lives in ui_manager.h)       */
/* ------------------------------------------------------------------ */

#define UI_TILE_RADIUS      16   /* small cards: menu tiles, domain badges */
#define UI_ROW_W            428  /* full-width list row */
#define UI_ROW_H            64
#define UI_ROW_RADIUS       14
#define UI_SECTION_W        428  /* group header flex item (forces wrap) */
#define UI_SECTION_H        36
#define UI_TILE_W           132  /* menu 3-column grid card */
#define UI_TILE_H           112
#define UI_DOMAIN_TILE_H    96   /* test-center badge wall card */
#define UI_CHIP_H           24   /* status badge */
#define UI_PROGRESS_H       8    /* test progress bar */
#define UI_PROGRESS_RADIUS  4
#define UI_ASK_BTN_H        56   /* operator confirm buttons */
#define UI_ACTION_ROW_H     64   /* test-center bottom action row */
