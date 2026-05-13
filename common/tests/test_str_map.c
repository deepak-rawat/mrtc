/*
 * test_str_map.c - Unit tests for rtc_str_map_t.
 */
#include "rtc/rtc_str_map.h"
#include "test_harness.h"

TEST(init_and_free_borrowed) {
    rtc_str_map_t m;
    ASSERT_EQ(rtc_str_map_init(&m), RTC_OK);
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)0);
    ASSERT(!m.owns_keys);
    rtc_str_map_free(&m);
    rtc_str_map_free(&m); /* idempotent */
}

TEST(init_and_free_owned) {
    rtc_str_map_t m;
    ASSERT_EQ(rtc_str_map_init_owned(&m), RTC_OK);
    ASSERT(m.owns_keys);
    rtc_str_map_free(&m);
}

TEST(set_and_get_borrowed) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int a = 1, b = 2;
    /* String literals have static lifetime - safe for borrowed mode */
    ASSERT_EQ(rtc_str_map_set(&m, "alice", &a), RTC_OK);
    ASSERT_EQ(rtc_str_map_set(&m, "bob", &b), RTC_OK);
    ASSERT(rtc_str_map_get(&m, "alice") == &a);
    ASSERT(rtc_str_map_get(&m, "bob") == &b);
    ASSERT(rtc_str_map_get(&m, "carol") == NULL);
    ASSERT(rtc_str_map_has(&m, "alice"));
    ASSERT(!rtc_str_map_has(&m, "carol"));
    rtc_str_map_free(&m);
}

TEST(set_and_get_owned) {
    rtc_str_map_t m;
    rtc_str_map_init_owned(&m);
    int a = 42;
    /* In owned mode, key is copied - safe even if caller frees source string */
    char buf[16];
    snprintf(buf, sizeof(buf), "%s", "key123");
    ASSERT_EQ(rtc_str_map_set(&m, buf, &a), RTC_OK);
    /* Overwrite local buffer - map should still find the entry */
    memset(buf, 'X', sizeof(buf));
    buf[15] = '\0';
    ASSERT(rtc_str_map_get(&m, "key123") == &a);
    rtc_str_map_free(&m);
}

TEST(overwrite_existing_key) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int a = 1, b = 2;
    rtc_str_map_set(&m, "k", &a);
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)1);
    rtc_str_map_set(&m, "k", &b);
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)1);
    ASSERT(rtc_str_map_get(&m, "k") == &b);
    rtc_str_map_free(&m);
}

TEST(remove_returns_status) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int a = 1;
    rtc_str_map_set(&m, "foo", &a);
    ASSERT(rtc_str_map_remove(&m, "foo"));
    ASSERT(!rtc_str_map_has(&m, "foo"));
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)0);
    ASSERT(!rtc_str_map_remove(&m, "foo"));
    ASSERT(!rtc_str_map_remove(&m, "missing"));
    rtc_str_map_free(&m);
}

TEST(remove_owned_frees_key) {
    /* Just exercise the owned-mode remove path; valgrind/ASAN would catch leaks. */
    rtc_str_map_t m;
    rtc_str_map_init_owned(&m);
    int sentinel = 1;
    rtc_str_map_set(&m, "to_be_freed", &sentinel);
    ASSERT(rtc_str_map_remove(&m, "to_be_freed"));
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)0);
    rtc_str_map_free(&m);
}

TEST(null_value_storable) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    rtc_str_map_set(&m, "k", NULL);
    ASSERT(rtc_str_map_get(&m, "k") == NULL);
    ASSERT(rtc_str_map_has(&m, "k"));
    rtc_str_map_free(&m);
}

TEST(empty_string_key) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int a = 7;
    rtc_str_map_set(&m, "", &a);
    ASSERT(rtc_str_map_has(&m, ""));
    ASSERT(rtc_str_map_get(&m, "") == &a);
    rtc_str_map_free(&m);
}

TEST(growth_with_owned_keys) {
    rtc_str_map_t m;
    rtc_str_map_init_owned(&m);
    char key[32];
    int sentinel = 1;
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof(key), "k_%d", i);
        ASSERT_EQ(rtc_str_map_set(&m, key, &sentinel), RTC_OK);
    }
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)500);
    for (int i = 0; i < 500; i++) {
        snprintf(key, sizeof(key), "k_%d", i);
        ASSERT(rtc_str_map_has(&m, key));
    }
    rtc_str_map_free(&m);
}

TEST(stress_set_remove) {
    rtc_str_map_t m;
    rtc_str_map_init_owned(&m);
    int sentinel = 1;
    char key[32];
    /* Insert 300 */
    for (int i = 0; i < 300; i++) {
        snprintf(key, sizeof(key), "node_%d", i);
        rtc_str_map_set(&m, key, &sentinel);
    }
    /* Remove every third */
    for (int i = 0; i < 300; i += 3) {
        snprintf(key, sizeof(key), "node_%d", i);
        ASSERT(rtc_str_map_remove(&m, key));
    }
    /* Verify */
    for (int i = 0; i < 300; i++) {
        snprintf(key, sizeof(key), "node_%d", i);
        bool expected = (i % 3 != 0);
        ASSERT(rtc_str_map_has(&m, key) == expected);
    }
    rtc_str_map_free(&m);
}

TEST(iterate_all_entries) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int sentinel = 0;
    rtc_str_map_set(&m, "a", &sentinel);
    rtc_str_map_set(&m, "b", &sentinel);
    rtc_str_map_set(&m, "c", &sentinel);
    rtc_str_map_set(&m, "d", &sentinel);

    rtc_str_map_iter_t it = {0};
    const char *key;
    void *value;
    int count = 0;
    bool seen_a = false, seen_b = false, seen_c = false, seen_d = false;
    while (rtc_str_map_next(&m, &it, &key, &value)) {
        ASSERT(value == &sentinel);
        if (strcmp(key, "a") == 0)
            seen_a = true;
        else if (strcmp(key, "b") == 0)
            seen_b = true;
        else if (strcmp(key, "c") == 0)
            seen_c = true;
        else if (strcmp(key, "d") == 0)
            seen_d = true;
        count++;
    }
    ASSERT_EQ(count, 4);
    ASSERT(seen_a && seen_b && seen_c && seen_d);
    rtc_str_map_free(&m);
}

TEST(clear_empties_map) {
    rtc_str_map_t m;
    rtc_str_map_init_owned(&m);
    int x = 1;
    char key[16];
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "item_%d", i);
        rtc_str_map_set(&m, key, &x);
    }
    rtc_str_map_clear(&m);
    ASSERT_EQ(rtc_str_map_len(&m), (size_t)0);
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "item_%d", i);
        ASSERT(!rtc_str_map_has(&m, key));
    }
    /* Reuse after clear */
    rtc_str_map_set(&m, "fresh", &x);
    ASSERT(rtc_str_map_has(&m, "fresh"));
    rtc_str_map_free(&m);
}

TEST(invalid_args) {
    rtc_str_map_t m;
    rtc_str_map_init(&m);
    int a = 1;
    ASSERT_EQ(rtc_str_map_set(&m, NULL, &a), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_str_map_set(NULL, "k", &a), RTC_ERR_INVALID);
    ASSERT(rtc_str_map_get(&m, NULL) == NULL);
    ASSERT(!rtc_str_map_has(&m, NULL));
    ASSERT(!rtc_str_map_remove(&m, NULL));
    rtc_str_map_free(&m);
}

int main(void) {
    printf("\n========== rtc_str_map tests ==========\n");
    RUN_TEST(init_and_free_borrowed);
    RUN_TEST(init_and_free_owned);
    RUN_TEST(set_and_get_borrowed);
    RUN_TEST(set_and_get_owned);
    RUN_TEST(overwrite_existing_key);
    RUN_TEST(remove_returns_status);
    RUN_TEST(remove_owned_frees_key);
    RUN_TEST(null_value_storable);
    RUN_TEST(empty_string_key);
    RUN_TEST(growth_with_owned_keys);
    RUN_TEST(stress_set_remove);
    RUN_TEST(iterate_all_entries);
    RUN_TEST(clear_empties_map);
    RUN_TEST(invalid_args);
    TEST_SUMMARY();
}
