#include "app_session.h"
#include "test_harness.h"

static void preview(const video_frame_t *frame, void *user) {
    int *frames = user;
    if (frame && frame->width == APP_SESSION_VIDEO_WIDTH &&
        frame->height == APP_SESSION_VIDEO_HEIGHT)
        ++*frames;
}

TEST(preview_is_paced_without_starting_rtc) {
    int frames = 0;
    app_session_config_t config = {.on_local_video = preview, .user_data = &frames};
    app_session_t *session = app_session_create(&config);
    ASSERT(session);
    app_session_preview_tick(session, 1000);
    app_session_preview_tick(session, 1001);
    app_session_preview_tick(session, 1066);
    ASSERT_EQ(frames, 2);
    app_session_snapshot_t snapshot;
    app_session_get_snapshot(session, &snapshot);
    ASSERT(!snapshot.active);
    ASSERT_EQ(snapshot.generation, 0);
    app_session_stop(session);
    app_session_destroy(session);
}

TEST(options_and_invalid_start) {
    app_session_t *session = app_session_create(NULL);
    ASSERT(session);
    app_session_options_t options = {.server = APP_SESSION_DEFAULT_SERVER,
                                     .room = "lifecycle",
                                     .video = APP_MEDIA_NONE,
                                     .audio = APP_MEDIA_NONE,
                                     .stun = ""};
    ASSERT_EQ(app_session_validate_options(&options), RTC_OK);
    options.video = (app_media_mode_t)999;
    ASSERT_EQ(app_session_start(session, &options), RTC_ERR_INVALID);
    options.video = APP_MEDIA_NONE;
    options.server = "http://127.0.0.1";
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    options.server = "ws://";
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    options.server = "ws://localhost:invalid";
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    options.server = "ws://localhost:65536";
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    options.server = APP_SESSION_DEFAULT_SERVER;
    options.room = "";
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    options.room = "lifecycle";
    options.data_only = true;
    options.audio = APP_MEDIA_SYNTHETIC;
    ASSERT_EQ(app_session_validate_options(&options), RTC_ERR_INVALID);
    app_session_snapshot_t snapshot;
    app_session_get_snapshot(session, &snapshot);
    ASSERT_EQ(snapshot.generation, 0);
    ASSERT_EQ(app_session_send_message(session, NULL, (const uint8_t *)"hi", 2), RTC_ERR_INVALID);
    ASSERT_EQ(app_session_mute(session, "video", true), RTC_ERR_INVALID);
    app_session_destroy(session);
}

TEST(stop_and_restart_reset_call_state) {
    app_session_t *session = app_session_create(NULL);
    ASSERT(session);
    app_session_options_t options = {.server = "ws://127.0.0.1:1",
                                     .room = "lifecycle",
                                     .name = "native",
                                     .video = APP_MEDIA_SYNTHETIC,
                                     .audio = APP_MEDIA_SYNTHETIC,
                                     .stun = ""};
    for (unsigned generation = 1; generation <= 2; ++generation) {
        int rc = app_session_start(session, &options);
        ASSERT(rc == RTC_OK || rc == RTC_ERR_GENERIC);
        app_session_snapshot_t snapshot;
        app_session_get_snapshot(session, &snapshot);
        ASSERT_EQ(snapshot.generation, generation);
        if (rc == RTC_OK) {
            ASSERT_EQ(app_session_start(session, &options), RTC_ERR_INVALID);
            ASSERT_EQ(app_session_mute(session, "video", true), RTC_OK);
            app_session_tick(session, 1000);
            app_session_get_snapshot(session, &snapshot);
            ASSERT(snapshot.video_muted);
        }
        app_session_stop(session);
        app_session_stop(session);
        app_session_get_snapshot(session, &snapshot);
        ASSERT(!snapshot.active);
        ASSERT(!snapshot.joined);
        ASSERT_EQ(snapshot.peer_count, 0);
        ASSERT_EQ(snapshot.source_count, 0);
        ASSERT_EQ(snapshot.epoch, 0);
        ASSERT_STR_EQ(snapshot.status, "idle");
    }
    app_session_destroy(session);
}

int main(void) {
    RUN_TEST(preview_is_paced_without_starting_rtc);
    RUN_TEST(options_and_invalid_start);
    RUN_TEST(stop_and_restart_reset_call_state);
    TEST_SUMMARY();
}
