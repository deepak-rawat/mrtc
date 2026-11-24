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

static void *topology_thread(void *user) {
    ui_sdl3_on_topology("sfu", "switching", 2, "", user);
    int16_t samples[960] = {0};
    audio_frame_t audio = {
        .samples = samples, .sample_count = 960, .sample_rate = 48000, .channels = 1};
    ui_sdl3_on_audio_samples("alice", "mic", &audio, user);
    return NULL;
}

TEST(topology_and_audio_callbacks_are_deferred_to_main_render) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 320, 240), 0);
    char title[256];
    snprintf(title, sizeof(title), "%s", SDL_GetWindowTitle(ui.window));
    rtc_thread_t thread;
    ASSERT_EQ(rtc_thread_create(&thread, topology_thread, &ui), RTC_OK);
    ASSERT_EQ(rtc_thread_join(&thread), RTC_OK);
    ASSERT_STR_EQ(SDL_GetWindowTitle(ui.window), title);
    ASSERT(ui.status_dirty);
    ASSERT_EQ(ui.pending_audio_count, 960);
    ui_sdl3_render(&ui);
    ASSERT(strstr(SDL_GetWindowTitle(ui.window), "Switching to SFU") != NULL);
    ASSERT(!ui.status_dirty);
    ASSERT_EQ(ui.pending_audio_count, 0);
    ui_sdl3_on_error("test failure", &ui);
    ASSERT(strstr(SDL_GetWindowTitle(ui.window), "test failure") == NULL);
    ui_sdl3_render(&ui);
    ASSERT(strstr(SDL_GetWindowTitle(ui.window), "Error: test failure") != NULL);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(audio_activity_smooths_release_and_holds_through_quiet_frames) {
    ui_audio_activity_t activity = {0};
    ui_sdl3_update_audio_activity(&activity, .014f, 1000);
    ASSERT(!ui_sdl3_audio_active(&activity, 1000));
    ui_sdl3_update_audio_activity(&activity, .020f, 1020);
    ASSERT(ui_sdl3_audio_active(&activity, 1020));
    ui_sdl3_update_audio_activity(&activity, 0, 1040);
    ASSERT(activity.level > .015f);
    ASSERT(activity.level < .020f);
    for (uint64_t now = 1060; now <= 1300; now += 20) {
        ui_sdl3_update_audio_activity(&activity, .012f, now);
        ASSERT(ui_sdl3_audio_active(&activity, now));
    }
    ui_sdl3_update_audio_activity(&activity, 0, 1400);
    ASSERT(activity.level < .010f);
    ASSERT_EQ(activity.active_until, 1600);
    ASSERT(ui_sdl3_audio_active(&activity, 1599));
    ASSERT(!ui_sdl3_audio_active(&activity, 1600));
    ui_sdl3_update_audio_activity(&activity, .012f, 2000);
    ASSERT(!ui_sdl3_audio_active(&activity, 2000));
    ui_sdl3_update_audio_activity(&activity, .020f, 2020);
    ASSERT(ui_sdl3_audio_active(&activity, 2020));
    ASSERT(!ui_sdl3_audio_active(&activity, 2320));
}

TEST(local_and_remote_activity_measure_the_entire_audio_frame) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 640, 480), 0);
    ui_sdl3_on_peer_joined("alice", &ui);
    int16_t samples[1920] = {0};
    for (size_t i = 256; i < sizeof(samples) / sizeof(samples[0]); i++)
        samples[i] = 3277;
    audio_frame_t audio = {
        .samples = samples, .sample_count = 960, .sample_rate = 48000, .channels = 2};
    ui_sdl3_on_local_audio(&audio, &ui);
    ui_sdl3_on_audio_samples("alice", "mic", &audio, &ui);
    ASSERT(ui.local_audio_activity.level > .09f);
    ASSERT(ui.local_audio_activity.level < .10f);
    ASSERT_EQ(ui.local_audio_activity.level, ui.tiles[0].audio_activity.level);
    ASSERT(ui_sdl3_audio_active(&ui.local_audio_activity, ui.local_audio_activity.updated_at));
    ASSERT(
        ui_sdl3_audio_active(&ui.tiles[0].audio_activity, ui.tiles[0].audio_activity.updated_at));
    ASSERT_EQ(ui.pending_audio_count, 1920);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(speaking_changes_only_tile_border_and_indicator) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 640, 480), 0);
    ui.in_call = true;
    ui_sdl3_on_peer_joined("alice", &ui);
    ui_layout_t layout;
    ui_sdl3_layout(640, 480, false, &layout);
    SDL_FRect tiles[] = {ui_sdl3_tile_rect(layout.gallery, 2, 0),
                         ui_sdl3_tile_rect(layout.gallery, 2, 1)};
    char path[128];
    snprintf(path, sizeof(path), "ui-activity-%llu.bmp", (unsigned long long)SDL_GetTicksNS());
    const char *previous = SDL_getenv("MRTC_UI_SNAPSHOT");
    char *saved = previous ? SDL_strdup(previous) : NULL;
    ASSERT(!previous || saved);
    ASSERT(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "MRTC_UI_SNAPSHOT", path, true));
    ui_sdl3_render(&ui);
    SDL_Surface *idle = SDL_LoadBMP(path);
    ASSERT(idle != NULL);

    int16_t samples[960];
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
        samples[i] = 3277;
    audio_frame_t audio = {
        .samples = samples, .sample_count = 960, .sample_rate = 48000, .channels = 1};
    ui_sdl3_on_audio_samples("alice", "mic", &audio, &ui);
    ui_sdl3_on_local_audio(&audio, &ui);
    ui_sdl3_render(&ui);
    SDL_Surface *active = SDL_LoadBMP(path);
    ASSERT(active != NULL);
    ui.mic_muted = true;
    ui_sdl3_render(&ui);
    SDL_Surface *muted = SDL_LoadBMP(path);
    ASSERT(muted != NULL);
    if (saved)
        ASSERT(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "MRTC_UI_SNAPSHOT", saved, true));
    else
        ASSERT(SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "MRTC_UI_SNAPSHOT"));
    SDL_free(saved);
    ASSERT_EQ(remove(path), 0);
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        SDL_Color before, after, muted_border;
        int x = (int)(tiles[i].x + tiles[i].w / 2);
        int y = (int)(tiles[i].y + tiles[i].h / 2);
        ASSERT(SDL_ReadSurfacePixel(idle, x, y, &before.r, &before.g, &before.b, &before.a));
        ASSERT(SDL_ReadSurfacePixel(active, x, y, &after.r, &after.g, &after.b, &after.a));
        ASSERT_MEM_EQ(&before, &after, sizeof(before));
        x = (int)tiles[i].x;
        y = (int)tiles[i].y;
        ASSERT(SDL_ReadSurfacePixel(idle, x, y, &before.r, &before.g, &before.b, &before.a));
        ASSERT(SDL_ReadSurfacePixel(active, x, y, &after.r, &after.g, &after.b, &after.a));
        ASSERT_EQ(after.r, 64);
        ASSERT_EQ(after.g, 210);
        ASSERT_EQ(after.b, 140);
        ASSERT(SDL_ReadSurfacePixel(muted, x, y, &muted_border.r, &muted_border.g, &muted_border.b,
                                    &muted_border.a));
        ASSERT_MEM_EQ(&muted_border, i == 0 ? &after : &before, sizeof(muted_border));
        x = (int)(tiles[i].x + tiles[i].w - 12);
        y = (int)(tiles[i].y + tiles[i].h - 18);
        ASSERT(SDL_ReadSurfacePixel(active, x, y, &after.r, &after.g, &after.b, &after.a));
        ASSERT_EQ(after.r, 64);
        ASSERT_EQ(after.g, 210);
        ASSERT_EQ(after.b, 140);
    }
    SDL_DestroySurface(idle);
    SDL_DestroySurface(active);
    SDL_DestroySurface(muted);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(responsive_layout_keeps_chat_and_controls_separate) {
    const int widths[] = {640, 1120, 1920};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        ui_layout_t layout;
        ui_sdl3_layout((float)widths[i], 720, true, &layout);
        ASSERT(layout.gallery.w > 0);
        ASSERT(layout.gallery.x + layout.gallery.w <= layout.chat.x);
        ASSERT(layout.chat.y + layout.chat.h < layout.controls[0].y);
        ASSERT(layout.controls[4].x + layout.controls[4].w <= widths[i]);
        SDL_FRect bounds = {20, 40, 320, 180};
        SDL_FRect fit = ui_sdl3_fit(bounds, 640, 480);
        ASSERT_EQ((int)fit.w, 240);
        ASSERT_EQ((int)fit.h, 180);
        ASSERT_EQ((int)fit.x, 60);
        ASSERT_EQ((int)fit.y, 40);
        SDL_FRect first = ui_sdl3_tile_rect(layout.gallery, 3, 0);
        SDL_FRect last = ui_sdl3_tile_rect(layout.gallery, 3, 2);
        ASSERT(first.w > 0 && first.h > 0);
        ASSERT(last.x + last.w <= layout.gallery.x + layout.gallery.w);
        ASSERT(last.y + last.h <= layout.gallery.y + layout.gallery.h);
    }
}

TEST(small_window_keeps_controls_and_join_form_inside_bounds) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 1120, 760), 0);
    ASSERT(SDL_SetWindowSize(ui.window, 640, 480));
    ui_layout_t layout;
    ui_sdl3_layout(640, 480, true, &layout);
    for (int i = 0; i < 5; i++) {
        SDL_FRect control = layout.controls[i];
        ASSERT(control.x >= 0 && control.y >= 0);
        ASSERT(control.x + control.w <= 640);
        ASSERT(control.y + control.h <= 480);
        ASSERT(layout.gallery.y + layout.gallery.h <= control.y);
        ASSERT(layout.chat.y + layout.chat.h <= control.y);
        if (i)
            ASSERT(layout.controls[i - 1].x + layout.controls[i - 1].w <= control.x);
    }
    ASSERT(layout.join.y + layout.join.h < 442);
    for (int i = 0; i < 3; i++)
        ASSERT(layout.fields[i].x + layout.fields[i].w <= 640);
    ui_sdl3_render(&ui);
    ui.in_call = ui.chat_open = true;
    ui_sdl3_on_peer_joined("alice", &ui);
    ui_sdl3_set_peer_name(&ui, "alice", "Alice");
    ui_sdl3_render(&ui);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(named_placeholders_stay_in_place_until_frames_arrive) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 640, 480), 0);
    ui.in_call = true;
    ui_sdl3_on_peer_joined("alice", &ui);
    ui_sdl3_set_peer_name(&ui, "alice",
                          "Al\xC3\xAF"
                          "ce");
    ui_sdl3_on_peer_joined("bob", &ui);
    ASSERT(ui.tiles[0].active);
    ASSERT(ui.tiles[0].texture == NULL);
    ASSERT_STR_EQ(ui.tiles[0].name, "Al\xC3\xAF"
                                    "ce");
    ui_sdl3_render(&ui);
    ASSERT_STR_EQ(ui.tiles[0].peer_id, "alice");
    ASSERT_STR_EQ(ui.tiles[1].peer_id, "bob");
    video_frame_t frame;
    ASSERT_EQ(video_frame_alloc(&frame, 64, 48), RTC_OK);
    ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
    ui_sdl3_render(&ui);
    ASSERT_EQ(ui.tile_count, 2);
    ASSERT(ui.tiles[0].texture != NULL);
    ASSERT_STR_EQ(ui.tiles[0].name, "Al\xC3\xAF"
                                    "ce");
    video_frame_free(&frame);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(editing_does_not_trigger_shortcuts_and_backspace_is_utf8_safe) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 640, 480), 0);
    ui.focused_field = 2;
    ui.name[0] = 0;
    SDL_Event event = {0};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "M\xC3\xA9";
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_NONE);
    ASSERT_STR_EQ(ui.name, "M\xC3\xA9");
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_M;
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_NONE);
    event.key.key = SDLK_BACKSPACE;
    ui_sdl3_event(&ui, &event);
    ASSERT_STR_EQ(ui.name, "M");
    ui.in_call = true;
    ui.chat_open = true;
    ui.focused_field = 3;
    event.key.key = SDLK_V;
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_NONE);
    event.key.key = SDLK_RETURN;
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_SEND);
    ui.focused_field = -1;
    event.key.key = SDLK_M;
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_MUTE);
    ASSERT(SDL_SetWindowSize(ui.window, 1120, 760));
    ui_sdl3_render(&ui);
    ui_layout_t layout;
    ui_sdl3_layout(1120, 760, true, &layout);
    const ui_action_t actions[] = {UI_ACTION_MUTE, UI_ACTION_CAMERA, UI_ACTION_CHAT, UI_ACTION_SEND,
                                   UI_ACTION_LEAVE};
    for (int i = 0; i < 5; i++) {
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = layout.controls[i].x + 2;
        event.button.y = layout.controls[i].y + 2;
        ASSERT_EQ(ui_sdl3_event(&ui, &event), actions[i]);
    }
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(chat_is_bounded_and_preserves_complete_unicode) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 640, 480), 0);
    for (int i = 0; i < UI_CHAT_LINES + 5; i++) {
        char message[32];
        snprintf(message, sizeof(message), "line %d", i);
        ui_sdl3_add_chat(&ui, "Alice", (const uint8_t *)message, strlen(message));
    }
    ASSERT_EQ(ui.chat_count, UI_CHAT_LINES);
    ASSERT_STR_EQ(ui.chat_lines[0], "Alice: line 5");
    ui_sdl3_on_peer_joined("alice", &ui);
    ui_sdl3_set_peer_name(&ui, "alice", "Alice");
    ui_sdl3_on_message("alice", (const uint8_t *)"h\xC3\xA9", 3, &ui);
    ASSERT_STR_EQ(ui.chat_lines[UI_CHAT_LINES - 1], "Alice: h\xC3\xA9");
    const uint8_t incomplete[] = {0xf0, 0x9f};
    ui_sdl3_on_message("alice", incomplete, sizeof(incomplete), &ui);
    ASSERT_STR_EQ(ui.chat_lines[UI_CHAT_LINES - 1], "Alice: h\xC3\xA9");
    ui.in_call = ui.chat_open = true;
    ui_sdl3_render(&ui);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(chat_wrap_prefers_word_boundaries) {
    ASSERT_EQ(ui_sdl3_wrap_line("Alice: Ready to review?", 19), 15);
    ASSERT_EQ(ui_sdl3_wrap_line("You: Yes, let's start.", 19), 15);
    ASSERT_EQ(ui_sdl3_wrap_line("short", 19), 5);
    ASSERT_EQ(ui_sdl3_wrap_line("unbreakable", 5), 5);
    ASSERT_EQ(ui_sdl3_wrap_line("five words", 4), 4);
}

TEST(join_form_sources_and_clicks) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 1120, 760), 0);
    ui_layout_t layout;
    ui_sdl3_layout(1120, 760, false, &layout);
    SDL_Event event = {0};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    for (int i = 0; i < 3; i++) {
        event.button.x = layout.fields[i].x + 2;
        event.button.y = layout.fields[i].y + 2;
        ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_NONE);
        ASSERT_EQ(ui.focused_field, i);
    }
    for (int i = 0; i < 2; i++) {
        event.button.x = layout.sources[i].x + 2;
        event.button.y = layout.sources[i].y + 2;
        ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_NONE);
    }
    ASSERT(ui.use_camera);
    ASSERT(ui.use_mic);
    event.button.x = layout.join.x + 2;
    event.button.y = layout.join.y + 2;
    ASSERT_EQ(ui_sdl3_event(&ui, &event), UI_ACTION_JOIN);
    ui_sdl3_render(&ui);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

TEST(gallery_snapshot_with_named_placeholders_and_chat) {
    ui_sdl3_t ui;
    ASSERT_EQ(ui_sdl3_init(&ui, 1120, 760), 0);
    ui.in_call = ui.chat_open = true;
    snprintf(ui.room, sizeof(ui.room), "Weekly design");
    ui_sdl3_on_peer_joined("alice", &ui);
    ui_sdl3_set_peer_name(&ui, "alice", "Alice");
    ui_sdl3_on_peer_joined("bob", &ui);
    ui_sdl3_set_peer_name(&ui, "bob", "Bob");
    video_frame_t frame;
    ASSERT_EQ(video_frame_alloc(&frame, 320, 240), RTC_OK);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            frame.planes[0][y * frame.stride[0] + x] = (uint8_t)(32 + x / 2);
    memset(frame.planes[1], 100, 160 * 120);
    memset(frame.planes[2], 160, 160 * 120);
    ui_sdl3_on_video_frame("alice", "camera", &frame, &ui);
    ui_sdl3_update_preview(&ui, &frame);
    ui_sdl3_add_chat(&ui, "Alice", (const uint8_t *)"Ready to review?", 16);
    ui_sdl3_add_chat(&ui, "You", (const uint8_t *)"Yes, let's start.", 17);
    ui_sdl3_set_status(&ui, "Connected / remote audio activity lights tile borders");
    ui_sdl3_render(&ui);
    ASSERT(ui.tiles[0].texture != NULL);
    ASSERT(ui.tiles[1].texture == NULL);
    ASSERT_STR_EQ(ui.tiles[1].name, "Bob");
    video_frame_free(&frame);
    ui_sdl3_close(&ui);
    SDL_Quit();
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    RUN_TEST(peer_left_removes_all_tiles);
    RUN_TEST(peer_left_before_first_render);
    RUN_TEST(topology_and_audio_callbacks_are_deferred_to_main_render);
    RUN_TEST(audio_activity_smooths_release_and_holds_through_quiet_frames);
    RUN_TEST(local_and_remote_activity_measure_the_entire_audio_frame);
    RUN_TEST(speaking_changes_only_tile_border_and_indicator);
    RUN_TEST(responsive_layout_keeps_chat_and_controls_separate);
    RUN_TEST(small_window_keeps_controls_and_join_form_inside_bounds);
    RUN_TEST(named_placeholders_stay_in_place_until_frames_arrive);
    RUN_TEST(editing_does_not_trigger_shortcuts_and_backspace_is_utf8_safe);
    RUN_TEST(chat_is_bounded_and_preserves_complete_unicode);
    RUN_TEST(chat_wrap_prefers_word_boundaries);
    RUN_TEST(join_form_sources_and_clicks);
    RUN_TEST(gallery_snapshot_with_named_placeholders_and_chat);
    TEST_SUMMARY();
}
