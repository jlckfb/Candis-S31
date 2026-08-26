/*
 * Candis-S31 player demo - lightweight MJPEG AVI parser.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AVI_CHUNK_UNKNOWN = 0,
    AVI_CHUNK_VIDEO,
    AVI_CHUNK_AUDIO,
} avi_chunk_type_t;

typedef struct {
    FILE *file;
    uint32_t movi_start;
    uint32_t movi_size;
    uint32_t next_pos;

    /* Video stream info. */
    uint32_t video_width;
    uint32_t video_height;
    uint32_t video_fps;       /**< frames per second (scaled, may be approximate) */
    uint32_t video_frame_count;

    /* Audio stream info. */
    uint32_t audio_sample_rate;
    uint16_t audio_channels;
    uint16_t audio_bits;
    bool has_audio;
} avi_handle_t;

typedef struct {
    avi_chunk_type_t type;
    uint32_t offset;
    uint32_t size;
} avi_chunk_t;

/**
 * Open an AVI file and parse headers.
 * Returns true on success; caller must call avi_close().
 */
bool avi_open(avi_handle_t *avi, const char *path);

/** Close AVI file. */
void avi_close(avi_handle_t *avi);

/** Seek to the start of the movi data. */
bool avi_seek_movi_start(avi_handle_t *avi);

/** Read the next chunk descriptor. Returns false at EOF or on error. */
bool avi_next_chunk(avi_handle_t *avi, avi_chunk_t *chunk);

/** Read chunk data into a pre-allocated buffer. */
bool avi_read_chunk_data(avi_handle_t *avi, const avi_chunk_t *chunk,
                         uint8_t *buf, uint32_t buf_size);

#ifdef __cplusplus
}
#endif

/** Seek to a video chunk near the given percent (0..100) of movi data. */
bool avi_seek_video_near_percent(avi_handle_t *avi, int percent, avi_chunk_t *out_chunk);
