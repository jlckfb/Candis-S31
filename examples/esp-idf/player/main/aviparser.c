/*
 * Candis-S31 player demo - lightweight MJPEG AVI parser.
 *
 * Only supports:
 *   - One MJPG video stream (FOURCC 'MJPG' or 'mjpg').
 *   - One PCM audio stream (FOURCC '01wb' chunks, WAVEFORMATEX format).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aviparser.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "aviparser";

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
                            ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool read_chunk_header(FILE *f, uint32_t *id, uint32_t *size)
{
    uint8_t buf[8];
    if (fread(buf, 1, 8, f) != 8) {
        return false;
    }
    *id = read_u32(buf);
    *size = read_u32(buf + 4);
    return true;
}

static bool skip_chunk(FILE *f, uint32_t size)
{
    uint32_t padded = (size + 1) & ~1U;
    if (fseek(f, padded, SEEK_CUR) != 0) {
        return false;
    }
    return true;
}

static bool parse_hdrl(avi_handle_t *avi)
{
    uint32_t list_id, list_size;
    if (!read_chunk_header(avi->file, &list_id, &list_size)) {
        return false;
    }
    if (list_id != FOURCC('L', 'I', 'S', 'T')) {
        return false;
    }
    uint8_t type[4];
    if (fread(type, 1, 4, avi->file) != 4) {
        return false;
    }
    if (memcmp(type, "hdrl", 4) != 0) {
        return false;
    }

    uint32_t end = ftell(avi->file) + list_size - 4;
    while (ftell(avi->file) + 8 <= end) {
        uint32_t id, size;
        long pos = ftell(avi->file);
        if (!read_chunk_header(avi->file, &id, &size)) {
            break;
        }
        if (id == FOURCC('a', 'v', 'i', 'h')) {
            uint8_t avih[56];
            if (fread(avih, 1, sizeof(avih), avi->file) != sizeof(avih)) {
                return false;
            }
            uint32_t us_per_frame = read_u32(avih + 0);
            if (us_per_frame > 0) {
                avi->video_fps = 1000000 / us_per_frame;
            }
            avi->video_frame_count = read_u32(avih + 24);
        } else if (id == FOURCC('L', 'I', 'S', 'T') &&
                   fread(&id, 1, 4, avi->file) == 4 &&
                   memcmp(&id, "strl", 4) == 0) {
            /* Parse strl: strh + strf. */
            uint32_t strl_end = pos + 8 + size;
            uint32_t strh_id, strh_size;
            if (!read_chunk_header(avi->file, &strh_id, &strh_size) ||
                    strh_id != FOURCC('s', 't', 'r', 'h')) {
                fseek(avi->file, strl_end, SEEK_SET);
                continue;
            }
            uint8_t strh[56];
            if (fread(strh, 1, sizeof(strh), avi->file) != sizeof(strh)) {
                return false;
            }
            uint32_t fcc_type = read_u32(strh + 0);
            /* uint32_t fcc_handler = read_u32(strh + 4); */

            uint32_t strf_id, strf_size;
            if (!read_chunk_header(avi->file, &strf_id, &strf_size) ||
                    strf_id != FOURCC('s', 't', 'r', 'f')) {
                fseek(avi->file, strl_end, SEEK_SET);
                continue;
            }
            if (fcc_type == FOURCC('v', 'i', 'd', 's')) {
                uint8_t strf[40];
                uint32_t rd = strf_size < sizeof(strf) ? strf_size : sizeof(strf);
                if (fread(strf, 1, rd, avi->file) != rd) {
                    return false;
                }
                avi->video_width = read_u32(strf + 4);
                avi->video_height = read_u32(strf + 8);
            } else if (fcc_type == FOURCC('a', 'u', 'd', 's')) {
                uint8_t strf[18];
                uint32_t rd = strf_size < sizeof(strf) ? strf_size : sizeof(strf);
                if (fread(strf, 1, rd, avi->file) != rd) {
                    return false;
                }
                avi->audio_sample_rate = read_u32(strf + 4);
                avi->audio_channels = read_u16(strf + 2);
                avi->audio_bits = read_u16(strf + 14);
                avi->has_audio = true;
            }
            fseek(avi->file, strl_end, SEEK_SET);
        } else {
            if (!skip_chunk(avi->file, size)) {
                break;
            }
        }
    }
    return true;
}

bool avi_open(avi_handle_t *avi, const char *path)
{
    if (avi == NULL || path == NULL) {
        return false;
    }
    memset(avi, 0, sizeof(*avi));
    avi->file = fopen(path, "rb");
    if (avi->file == NULL) {
        ESP_LOGE(TAG, "open %s failed", path);
        return false;
    }

    uint32_t riff_id, riff_size;
    if (!read_chunk_header(avi->file, &riff_id, &riff_size) ||
            riff_id != FOURCC('R', 'I', 'F', 'F')) {
        goto fail;
    }
    uint8_t avi_type[4];
    if (fread(avi_type, 1, 4, avi->file) != 4 || memcmp(avi_type, "AVI ", 4) != 0) {
        goto fail;
    }

    if (!parse_hdrl(avi)) {
        ESP_LOGE(TAG, "hdrl parse failed");
        goto fail;
    }

    /* Find movi LIST. */
    uint32_t file_end = 8 + riff_size;
    while (ftell(avi->file) + 12 <= file_end) {
        uint32_t id, size;
        long pos = ftell(avi->file);
        if (!read_chunk_header(avi->file, &id, &size)) {
            break;
        }
        if (id == FOURCC('L', 'I', 'S', 'T')) {
            uint8_t type[4];
            if (fread(type, 1, 4, avi->file) != 4) {
                break;
            }
            if (memcmp(type, "movi", 4) == 0) {
                avi->movi_start = ftell(avi->file);
                avi->movi_size = size - 4;
                avi->next_pos = avi->movi_start;
                ESP_LOGI(TAG, "movi at %lu, size %lu", avi->movi_start, avi->movi_size);
                return true;
            }
            /* Skip rest of this LIST. */
            fseek(avi->file, pos + 8 + size, SEEK_SET);
        } else {
            if (!skip_chunk(avi->file, size)) {
                break;
            }
        }
    }

fail:
    avi_close(avi);
    return false;
}

void avi_close(avi_handle_t *avi)
{
    if (avi == NULL) {
        return;
    }
    if (avi->file != NULL) {
        fclose(avi->file);
        avi->file = NULL;
    }
}

bool avi_seek_movi_start(avi_handle_t *avi)
{
    if (avi == NULL || avi->file == NULL) {
        return false;
    }
    if (fseek(avi->file, avi->movi_start, SEEK_SET) != 0) {
        return false;
    }
    avi->next_pos = avi->movi_start;
    return true;
}

bool avi_next_chunk(avi_handle_t *avi, avi_chunk_t *chunk)
{
    if (avi == NULL || avi->file == NULL || chunk == NULL) {
        return false;
    }
    if (avi->next_pos >= avi->movi_start + avi->movi_size) {
        return false;
    }
    if (fseek(avi->file, avi->next_pos, SEEK_SET) != 0) {
        return false;
    }
    uint32_t id, size;
    if (!read_chunk_header(avi->file, &id, &size)) {
        return false;
    }
    chunk->offset = ftell(avi->file);
    chunk->size = size;

    uint8_t stream_id = (uint8_t)(id & 0xFF);
    uint8_t chunk_type = (uint8_t)((id >> 8) & 0xFF);
    if (chunk_type == 'd' || chunk_type == 'D') {
        chunk->type = AVI_CHUNK_VIDEO;
    } else if (chunk_type == 'w' || chunk_type == 'W') {
        chunk->type = AVI_CHUNK_AUDIO;
    } else {
        chunk->type = AVI_CHUNK_UNKNOWN;
    }
    /* Keep stream id for possible multi-stream support; ignored here. */
    (void)stream_id;

    uint32_t padded = (size + 1) & ~1U;
    avi->next_pos = chunk->offset + padded;
    return true;
}

bool avi_read_chunk_data(avi_handle_t *avi, const avi_chunk_t *chunk,
                         uint8_t *buf, uint32_t buf_size)
{
    if (avi == NULL || avi->file == NULL || chunk == NULL || buf == NULL) {
        return false;
    }
    if (chunk->size > buf_size) {
        return false;
    }
    if (fseek(avi->file, chunk->offset, SEEK_SET) != 0) {
        return false;
    }
    if (fread(buf, 1, chunk->size, avi->file) != chunk->size) {
        return false;
    }
    return true;
}

bool avi_seek_video_near_percent(avi_handle_t *avi, int percent, avi_chunk_t *out_chunk)
{
    if (avi == NULL || avi->file == NULL || out_chunk == NULL) {
        return false;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t target = avi->movi_start + ((uint32_t)percent * avi->movi_size) / 100U;
    if (target > avi->movi_start + avi->movi_size - 8) {
        target = avi->movi_start + avi->movi_size - 8;
    }
    if (fseek(avi->file, target, SEEK_SET) != 0) {
        return false;
    }
    avi->next_pos = target;

    /* Find the next video chunk from this approximate position. */
    avi_chunk_t chunk;
    while (avi_next_chunk(avi, &chunk)) {
        if (chunk.type == AVI_CHUNK_VIDEO) {
            *out_chunk = chunk;
            return true;
        }
    }
    return false;
}
