/*
 * test_u32_map.c - Unit tests for rtc_u32_map_t.
 */
#include "rtc/rtc_u32_map.h"
#include "test_harness.h"

TEST(init_and_free) {
    rtc_u32_map_t m;
    ASSERT_EQ(rtc_u32_map_init(&m), RTC_OK);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)0);
    ASSERT(m.entries == NULL);
    rtc_u32_map_free(&m);
    rtc_u32_map_free(&m); /* idempotent */
}

TEST(get_on_empty) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    ASSERT(rtc_u32_map_get(&m, 42) == NULL);
    ASSERT(!rtc_u32_map_has(&m, 42));
    ASSERT(!rtc_u32_map_remove(&m, 42));
    rtc_u32_map_free(&m);
}

TEST(set_and_get) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int a = 1, b = 2, c = 3;
    ASSERT_EQ(rtc_u32_map_set(&m, 100, &a), RTC_OK);
    ASSERT_EQ(rtc_u32_map_set(&m, 200, &b), RTC_OK);
    ASSERT_EQ(rtc_u32_map_set(&m, 300, &c), RTC_OK);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)3);
    ASSERT(rtc_u32_map_get(&m, 100) == &a);
    ASSERT(rtc_u32_map_get(&m, 200) == &b);
    ASSERT(rtc_u32_map_get(&m, 300) == &c);
    ASSERT(rtc_u32_map_get(&m, 999) == NULL);
    ASSERT(rtc_u32_map_has(&m, 100));
    ASSERT(!rtc_u32_map_has(&m, 999));
    rtc_u32_map_free(&m);
}

TEST(overwrite_existing_key) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int a = 1, b = 2;
    rtc_u32_map_set(&m, 42, &a);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)1);
    rtc_u32_map_set(&m, 42, &b);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)1);
    ASSERT(rtc_u32_map_get(&m, 42) == &b);
    rtc_u32_map_free(&m);
}

TEST(remove_then_get) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int a = 1;
    rtc_u32_map_set(&m, 42, &a);
    ASSERT(rtc_u32_map_remove(&m, 42));
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)0);
    ASSERT(rtc_u32_map_get(&m, 42) == NULL);
    ASSERT(!rtc_u32_map_has(&m, 42));
    /* Removing again returns false */
    ASSERT(!rtc_u32_map_remove(&m, 42));
    rtc_u32_map_free(&m);
}

TEST(reinsert_after_remove) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int a = 1, b = 2;
    rtc_u32_map_set(&m, 42, &a);
    rtc_u32_map_remove(&m, 42);
    /* Re-insert: tombstone should be reused */
    ASSERT_EQ(rtc_u32_map_set(&m, 42, &b), RTC_OK);
    ASSERT(rtc_u32_map_get(&m, 42) == &b);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)1);
    rtc_u32_map_free(&m);
}

TEST(null_value_storable) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    ASSERT_EQ(rtc_u32_map_set(&m, 1, NULL), RTC_OK);
    /* get returns NULL but has() reports true */
    ASSERT(rtc_u32_map_get(&m, 1) == NULL);
    ASSERT(rtc_u32_map_has(&m, 1));
    ASSERT(!rtc_u32_map_has(&m, 2));
    rtc_u32_map_free(&m);
}

TEST(growth_triggers_rehash) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    /* Insert many entries to force several grows */
    int values[1000];
    for (int i = 0; i < 1000; i++) {
        values[i] = i;
        ASSERT_EQ(rtc_u32_map_set(&m, (uint32_t)(i + 1), &values[i]), RTC_OK);
    }
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)1000);
    /* All entries still findable after rehashing */
    for (int i = 0; i < 1000; i++) {
        int *p = (int *)rtc_u32_map_get(&m, (uint32_t)(i + 1));
        ASSERT(p != NULL);
        ASSERT_EQ(*p, i);
    }
    rtc_u32_map_free(&m);
}

TEST(stress_set_remove) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int sentinel = 1;
    /* Insert 500 */
    for (uint32_t i = 0; i < 500; i++)
        rtc_u32_map_set(&m, i * 7 + 1, &sentinel);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)500);
    /* Remove every other */
    for (uint32_t i = 0; i < 500; i += 2)
        ASSERT(rtc_u32_map_remove(&m, i * 7 + 1));
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)250);
    /* Verify remaining */
    for (uint32_t i = 0; i < 500; i++) {
        bool expected = (i % 2 == 1);
        ASSERT(rtc_u32_map_has(&m, i * 7 + 1) == expected);
    }
    /* Re-insert removed ones */
    for (uint32_t i = 0; i < 500; i += 2)
        rtc_u32_map_set(&m, i * 7 + 1, &sentinel);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)500);
    rtc_u32_map_free(&m);
}

TEST(iterate_all_entries) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int sentinel = 0;
    const uint32_t N = 50;
    for (uint32_t i = 0; i < N; i++)
        rtc_u32_map_set(&m, i + 1000, &sentinel);

    rtc_u32_map_iter_t it = {0};
    uint32_t key;
    void *value;
    bool seen[50] = {false};
    int count = 0;
    while (rtc_u32_map_next(&m, &it, &key, &value)) {
        ASSERT(key >= 1000 && key < 1000 + N);
        ASSERT(!seen[key - 1000]);
        seen[key - 1000] = true;
        ASSERT(value == &sentinel);
        count++;
    }
    ASSERT_EQ(count, (int)N);
    for (uint32_t i = 0; i < N; i++)
        ASSERT(seen[i]);
    rtc_u32_map_free(&m);
}

TEST(clear_empties_map) {
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int x = 1;
    for (uint32_t i = 0; i < 100; i++)
        rtc_u32_map_set(&m, i, &x);
    rtc_u32_map_clear(&m);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)0);
    for (uint32_t i = 0; i < 100; i++)
        ASSERT(!rtc_u32_map_has(&m, i));
    /* Can still use map after clear */
    rtc_u32_map_set(&m, 42, &x);
    ASSERT_EQ(rtc_u32_map_len(&m), (size_t)1);
    rtc_u32_map_free(&m);
}

TEST(key_zero_works) {
    /* Key 0 is a valid uint32 key - make sure state byte separation handles it. */
    rtc_u32_map_t m;
    rtc_u32_map_init(&m);
    int a = 42;
    ASSERT_EQ(rtc_u32_map_set(&m, 0, &a), RTC_OK);
    ASSERT(rtc_u32_map_has(&m, 0));
    ASSERT(rtc_u32_map_get(&m, 0) == &a);
    ASSERT(rtc_u32_map_remove(&m, 0));
    ASSERT(!rtc_u32_map_has(&m, 0));
    rtc_u32_map_free(&m);
}

int main(void) {
    printf("\n========== rtc_u32_map tests ==========\n");
    RUN_TEST(init_and_free);
    RUN_TEST(get_on_empty);
    RUN_TEST(set_and_get);
    RUN_TEST(overwrite_existing_key);
    RUN_TEST(remove_then_get);
    RUN_TEST(reinsert_after_remove);
    RUN_TEST(null_value_storable);
    RUN_TEST(growth_triggers_rehash);
    RUN_TEST(stress_set_remove);
    RUN_TEST(iterate_all_entries);
    RUN_TEST(clear_empties_map);
    RUN_TEST(key_zero_works);
    TEST_SUMMARY();
}
