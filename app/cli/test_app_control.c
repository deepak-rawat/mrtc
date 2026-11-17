#include "control.h"
#include "test_harness.h"

TEST(valid_commands_and_correlated_results) {
    app_command_t command;
    const char *error;
    ASSERT_EQ(app_command_parse(&command,
                                "{\"version\":1,\"id\":\"join-a\",\"command\":\"join\","
                                "\"server\":\"ws://localhost:8080\",\"room\":\"room\","
                                "\"name\":\"Alice\",\"video\":\"none\",\"audio\":\"synthetic\","
                                "\"stun\":\"\"}",
                                &error),
              RTC_OK);
    ASSERT_EQ(command.options.video, APP_MEDIA_NONE);
    ASSERT_EQ(command.options.audio, APP_MEDIA_SYNTHETIC);
    ASSERT_STR_EQ(command.options.stun, "");
    app_session_snapshot_t snapshot = {.generation = 2, .epoch = 3};
    cJSON *result = app_control_result(&command, &snapshot, RTC_OK, NULL, NULL);
    ASSERT_STR_EQ(cJSON_GetObjectItemCaseSensitive(result, "id")->valuestring, "join-a");
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "ok")));
    cJSON_Delete(result);
    app_command_close(&command);
    ASSERT_EQ(app_command_parse(&command,
                                "{\"version\":1,\"id\":7,\"command\":\"send_message\","
                                "\"text\":\"hello\\nworld\",\"peer_id\":\"alice\"}",
                                &error),
              RTC_OK);
    ASSERT_STR_EQ(command.text, "hello\nworld");
    app_command_close(&command);
}

TEST(malformed_commands_are_rejected) {
    const char *cases[] = {
        "",
        "{}",
        "[]",
        "null",
        "{\"version\":1",
        "{\"version\":1,\"id\":1,\"command\":\"stats\"} trailing",
        "{\"version\":2,\"id\":1,\"command\":\"stats\"}",
        "{\"version\":1,\"id\":true,\"command\":\"stats\"}",
        "{\"version\":1,\"id\":1.5,\"command\":\"stats\"}",
        "{\"version\":1,\"id\":9007199254740992,\"command\":\"stats\"}",
        "{\"version\":1,\"id\":\"\",\"command\":\"stats\"}",
        "{\"version\":1,\"id\":1,\"command\":\"unknown\"}",
        "{\"version\":1,\"id\":1,\"command\":\"stats\",\"extra\":1}",
        "{\"version\":1,\"id\":1,\"command\":\"stats\",\"command\":\"quit\"}",
        "{\"version\":1,\"id\":1,\"command\":\"mute\",\"source\":\"screen\",\"muted\":true}",
        "{\"version\":1,\"id\":1,\"command\":\"mute\",\"source\":\"video\",\"muted\":1}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"\"}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"hi\",\"peer_id\":null}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"hi\\u0000bye\"}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"hi\1bye\"}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"\xc0\xaf\"}",
        "{\"version\":1,\"id\":1,\"command\":\"send_message\",\"text\":\"\xed\xa0\x80\"}",
        "{\"version\":1,\"id\":1,\"command\":\"join\",\"server\":\"ws://localhost\","
        "\"room\":\"r\",\"name\":\"n\",\"video\":\"camera\",\"audio\":\"none\"}",
        "{\"version\":1,\"id\":1,\"command\":\"join\",\"server\":\"ws://localhost\","
        "\"room\":\"r\",\"name\":\"n\",\"video\":\"none\"}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        app_command_t command;
        const char *error;
        ASSERT_EQ(app_command_parse(&command, cases[i], &error), RTC_ERR_INVALID);
        ASSERT(error);
        app_command_close(&command);
    }
}

TEST(bounds_and_error_correlation) {
    char line[APP_CONTROL_MAX_LINE + 2];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    app_command_t command;
    const char *error;
    ASSERT_EQ(app_command_parse(&command, line, &error), RTC_ERR_INVALID);
    app_command_close(&command);
    ASSERT_EQ(app_command_parse(
                  &command, "{\"version\":1,\"id\":\"negative\",\"command\":\"unknown\"}", &error),
              RTC_ERR_INVALID);
    app_session_snapshot_t snapshot = {.generation = 1, .epoch = 2};
    cJSON *result = app_control_result(&command, &snapshot, RTC_ERR_INVALID, error, NULL);
    ASSERT_STR_EQ(cJSON_GetObjectItemCaseSensitive(result, "id")->valuestring, "negative");
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result, "ok")));
    cJSON_Delete(result);
    app_command_close(&command);
}

TEST(message_and_identity_length_limits) {
    char text[APP_CONTROL_MAX_TEXT + 2];
    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "version", 1);
    cJSON_AddStringToObject(json, "id", "length-test");
    cJSON_AddStringToObject(json, "command", "send_message");
    cJSON_AddStringToObject(json, "text", text);
    char *line = cJSON_PrintUnformatted(json);
    app_command_t command;
    const char *error;
    ASSERT_EQ(app_command_parse(&command, line, &error), RTC_ERR_INVALID);
    app_command_close(&command);
    cJSON_free(line);
    text[APP_CONTROL_MAX_TEXT] = '\0';
    cJSON_ReplaceItemInObjectCaseSensitive(json, "text", cJSON_CreateString(text));
    line = cJSON_PrintUnformatted(json);
    ASSERT_EQ(app_command_parse(&command, line, &error), RTC_OK);
    app_command_close(&command);
    cJSON_free(line);
    cJSON_AddStringToObject(json, "peer_id", "1234567890123456");
    line = cJSON_PrintUnformatted(json);
    ASSERT_EQ(app_command_parse(&command, line, &error), RTC_ERR_INVALID);
    app_command_close(&command);
    cJSON_free(line);
    cJSON_Delete(json);
}

int main(void) {
    RUN_TEST(valid_commands_and_correlated_results);
    RUN_TEST(malformed_commands_are_rejected);
    RUN_TEST(bounds_and_error_correlation);
    RUN_TEST(message_and_identity_length_limits);
    TEST_SUMMARY();
}
