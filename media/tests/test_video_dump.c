/*
 * IVF dump tests.
 */
#include "test_harness.h"
#include "video_dump.h"
#include <string.h>
#include <stdio.h>

TEST(dump_write_and_verify) {
    const char *path = "test_dump.ivf";
    video_dump_t *d = video_dump_open(path, "VP80", 320, 240, 30);
    ASSERT(d != NULL);

    uint8_t frame[100];
    memset(frame, 0xAB, sizeof(frame));
    ASSERT(video_dump_write_frame(d, frame, sizeof(frame), 3000) == RTC_OK);
    ASSERT(video_dump_write_frame(d, frame, sizeof(frame), 6000) == RTC_OK);
    video_dump_close(d);

    /* Verify IVF header */
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t hdr[32];
    ASSERT(fread(hdr, 1, 32, fp) == 32);
    ASSERT(memcmp(hdr, "DKIF", 4) == 0);
    ASSERT(memcmp(hdr + 8, "VP80", 4) == 0);
    uint32_t fc = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    ASSERT(fc == 2);

    /* Verify first frame header */
    uint8_t fh[12];
    ASSERT(fread(fh, 1, 12, fp) == 12);
    uint32_t sz = fh[0] | (fh[1] << 8) | (fh[2] << 16) | (fh[3] << 24);
    ASSERT(sz == 100);
    fclose(fp);
    remove(path);
}

TEST(dump_null_safety) {
    ASSERT(video_dump_open(NULL, "VP80", 320, 240, 30) == NULL);
    ASSERT(video_dump_write_frame(NULL, NULL, 0, 0) == RTC_ERR_INVALID);
    video_dump_close(NULL);
}

int main(void) {
    RUN_TEST(dump_write_and_verify);
    RUN_TEST(dump_null_safety);
    return _test_fail_count > 0 ? 1 : 0;
}
