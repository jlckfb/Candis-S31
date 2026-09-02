/*
 * Candis-S31 watch demo - one-shot page walk for navigation latency data.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Launch the background walk task (one-shot, self-deleting). */
void ui_page_walk_start(void);

#ifdef __cplusplus
}
#endif
