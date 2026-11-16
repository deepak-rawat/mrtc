#include "ui_sdl3.h"
#include "test_harness.h"

static void *peer_left_thread(void *user) {
    ui_sdl3_on_peer_left("alice", user);
    return NULL;
}

TEST(peer_left_removes_all_tiles) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 320, 240), 0);
    video_frame_t frame;
    ASSERT_EQ(video_frame_alloc(&frame, 64, 48), RTC_OK);
    memset(frame.planes[0], 80, 64 * 48);
    memset(frame.planes[1], 128, 32 * 24);
    memset(frame.planes[2], 128, 32 * 24);

    ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
    ui_sdl3_on_video_frame("alice", "screen", &frame, &ui);
    ui_sdl3_on_video_frame("bob", "camera", &frame, &ui);
    ui_sdl3_render(&ui);
    ASSERT(ui.tiles[0].texture != NULL);
    ASSERT(ui.tiles[1].texture != NULL);
    ASSERT(ui.tiles[2].texture != NULL);

    ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
    rtc_thread_t thread;
    ASSERT_EQ(rtc_thread_create(&thread, peer_left_thread, &ui), RTC_OK);
    ASSERT_EQ(rtc_thread_join(&thread), RTC_OK);
    ASSERT(ui.tiles[0].texture != NULL);
    ASSERT(ui.tiles[0].remove_pending);
    ui_sdl3_render(&ui);
    ASSERT(!ui.tiles[0].active);
    ASSERT(!ui.tiles[1].active);
    ASSERT(ui.tiles[0].texture == NULL);
    ASSERT(ui.tiles[0].pending_yuv == NULL);
    ASSERT(rtc_str_map_get(&ui.tile_index, "alice:camera") == NULL);
    ASSERT(rtc_str_map_get(&ui.tile_index, "alice:screen") == NULL);
    ASSERT(ui.tiles[2].active);
    ASSERT(ui.tiles[2].texture != NULL);

    for (int i = 0; i < UI_MAX_TILES + 2; i++) {
        ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
        ui_sdl3_render(&ui);
        ASSERT(ui.tiles[0].active);
        ASSERT(ui.tiles[0].texture != NULL);
        ui_sdl3_on_peer_left("alice", &ui);
        ui_sdl3_render(&ui);
        ASSERT(!ui.tiles[0].active);
    }
    video_frame_free(&frame);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(peer_left_before_first_render) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 320, 240), 0);
    video_frame_t frame;
    ASSERT_EQ(video_frame_alloc(&frame, 64, 48), RTC_OK);
    ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
    ui_sdl3_on_peer_left("alice", &ui);
    ui_sdl3_render(&ui);
    ASSERT(!ui.tiles[0].active);
    ASSERT(ui.tiles[0].texture == NULL);
    ASSERT(ui.tiles[0].pending_yuv == NULL);
    video_frame_free(&frame);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    RUN_TEST(peer_left_removes_all_tiles);
    RUN_TEST(peer_left_before_first_render);
    TEST_SUMMARY();
}
