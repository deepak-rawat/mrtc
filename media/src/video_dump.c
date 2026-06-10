/*
 * IVF bitstream dump for VP8/VP9.
 */
#include "video_dump.h"
#include <rtc/rtc_common.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct video_dump {
    FILE *fp;
    uint32_t frame_count;
};

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

video_dump_t *video_dump_open(const char *path, const char *codec_fourcc, int width, int height,
                              int fps) {
    if (!path || !codec_fourcc)
        return NULL;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        RTC_LOG_ERR("video_dump: cannot open %s", path);
        return NULL;
    }

    video_dump_t *d = (video_dump_t *)calloc(1, sizeof(*d));
    if (!d) {
        fclose(fp);
        return NULL;
    }
    d->fp = fp;

    /* 32-byte IVF file header */
    uint8_t hdr[32] = {0};
    memcpy(hdr, "DKIF", 4);
    put_le16(hdr + 4, 0);  /* version */
    put_le16(hdr + 6, 32); /* header size */
    memcpy(hdr + 8, codec_fourcc, 4);
    put_le16(hdr + 12, (uint16_t)width);
    put_le16(hdr + 14, (uint16_t)height);
    put_le32(hdr + 16, (uint32_t)fps); /* fps numerator */
    put_le32(hdr + 20, 1);             /* fps denominator */
    /* hdr[24..27] = 0: frame count, patched on close */

    if (fwrite(hdr, 1, 32, fp) != 32) {
        RTC_LOG_ERR("video_dump: header write failed");
        fclose(fp);
        free(d);
        return NULL;
    }

    RTC_LOG_INFO("video_dump: opened %s (%dx%d @ %d fps)", path, width, height, fps);
    return d;
}

int video_dump_write_frame(video_dump_t *d, const uint8_t *data, size_t len, uint64_t timestamp) {
    if (!d || !d->fp || !data)
        return RTC_ERR_INVALID;

    /* 12-byte per-frame header: 4 size + 8 timestamp */
    uint8_t fh[12];
    put_le32(fh, (uint32_t)len);
    put_le64(fh + 4, timestamp);

    if (fwrite(fh, 1, 12, d->fp) != 12)
        return RTC_ERR_GENERIC;
    if (fwrite(data, 1, len, d->fp) != len)
        return RTC_ERR_GENERIC;

    d->frame_count++;
    return RTC_OK;
}

void video_dump_close(video_dump_t *d) {
    if (!d)
        return;
    if (d->fp) {
        /* Seek back and patch frame count at offset 24 */
        fseek(d->fp, 24, SEEK_SET);
        uint8_t buf[4];
        put_le32(buf, d->frame_count);
        fwrite(buf, 1, 4, d->fp);
        fclose(d->fp);
        RTC_LOG_INFO("video_dump: closed (%u frames)", d->frame_count);
    }
    free(d);
}
