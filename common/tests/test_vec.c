/*
 * Unit tests for rtc_vec_t dynamic array.
 */
#include "rtc/rtc_vec.h"
#include "test_harness.h"

TEST(init_and_free) {
    rtc_vec_t v;
    ASSERT_EQ(rtc_vec_init(&v, sizeof(int)), RTC_OK);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)0);
    ASSERT(v.data == NULL);
    ASSERT_EQ(v.cap, (size_t)0);
    ASSERT_EQ(v.elem_size, sizeof(int));
    rtc_vec_free(&v);
    /* Safe to free again (zero state) */
    rtc_vec_free(&v);
}

TEST(init_invalid) {
    rtc_vec_t v;
    ASSERT_EQ(rtc_vec_init(NULL, sizeof(int)), RTC_ERR_INVALID);
    ASSERT_EQ(rtc_vec_init(&v, 0), RTC_ERR_INVALID);
}

TEST(push_and_at) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));

    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(rtc_vec_push(&v, &i), RTC_OK);
    }
    ASSERT_EQ(rtc_vec_len(&v), (size_t)100);

    for (int i = 0; i < 100; i++) {
        int *p = (int *)rtc_vec_at(&v, (size_t)i);
        ASSERT(p != NULL);
        ASSERT_EQ(*p, i);
    }

    /* Out-of-range access returns NULL */
    ASSERT(rtc_vec_at(&v, 100) == NULL);
    ASSERT(rtc_vec_at(&v, SIZE_MAX) == NULL);

    rtc_vec_free(&v);
}

TEST(push_struct) {
    typedef struct {
        int a;
        double b;
    } pair_t;

    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(pair_t));

    pair_t p1 = {1, 2.5}, p2 = {3, 4.5};
    rtc_vec_push(&v, &p1);
    rtc_vec_push(&v, &p2);

    pair_t *q1 = (pair_t *)rtc_vec_at(&v, 0);
    pair_t *q2 = (pair_t *)rtc_vec_at(&v, 1);
    ASSERT_EQ(q1->a, 1);
    ASSERT(q1->b == 2.5);
    ASSERT_EQ(q2->a, 3);
    ASSERT(q2->b == 4.5);

    rtc_vec_free(&v);
}

TEST(reserve_grows_capacity) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    ASSERT_EQ(rtc_vec_reserve(&v, 100), RTC_OK);
    ASSERT(v.cap >= 100);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)0);
    /* Reserve smaller is no-op */
    size_t cap_before = v.cap;
    ASSERT_EQ(rtc_vec_reserve(&v, 10), RTC_OK);
    ASSERT_EQ(v.cap, cap_before);
    rtc_vec_free(&v);
}

TEST(remove_shifts_elements) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    for (int i = 0; i < 5; i++)
        rtc_vec_push(&v, &i);
    /* Remove middle element (index 2 = value 2) */
    rtc_vec_remove(&v, 2);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)4);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 0), 0);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 1), 1);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 2), 3);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 3), 4);
    /* Remove first */
    rtc_vec_remove(&v, 0);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 0), 1);
    /* Remove last */
    rtc_vec_remove(&v, rtc_vec_len(&v) - 1);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)2);
    /* Remove out-of-range is no-op */
    rtc_vec_remove(&v, 100);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)2);
    rtc_vec_free(&v);
}

TEST(swap_remove_o1) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    for (int i = 0; i < 5; i++)
        rtc_vec_push(&v, &i);
    /* swap_remove(1): replaces index 1 with last (4), shrinks */
    rtc_vec_swap_remove(&v, 1);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)4);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 0), 0);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 1), 4);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 2), 2);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 3), 3);
    /* swap_remove(last) just shrinks */
    rtc_vec_swap_remove(&v, 3);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)3);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 2), 2);
    rtc_vec_free(&v);
}

TEST(clear_preserves_capacity) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    for (int i = 0; i < 32; i++)
        rtc_vec_push(&v, &i);
    size_t cap = v.cap;
    rtc_vec_clear(&v);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)0);
    ASSERT_EQ(v.cap, cap);
    /* Can keep pushing into the cleared buffer */
    int x = 99;
    rtc_vec_push(&v, &x);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 0), 99);
    rtc_vec_free(&v);
}

TEST(stress_push) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    const int N = 10000;
    for (int i = 0; i < N; i++)
        ASSERT_EQ(rtc_vec_push(&v, &i), RTC_OK);
    ASSERT_EQ(rtc_vec_len(&v), (size_t)N);
    for (int i = 0; i < N; i++)
        ASSERT_EQ(*(int *)rtc_vec_at(&v, (size_t)i), i);
    rtc_vec_free(&v);
}

TEST(reuse_after_free) {
    rtc_vec_t v;
    rtc_vec_init(&v, sizeof(int));
    int x = 7;
    rtc_vec_push(&v, &x);
    rtc_vec_free(&v);
    /* After free, vector is empty but elem_size preserved; safe to push again. */
    ASSERT_EQ(rtc_vec_len(&v), (size_t)0);
    int y = 8;
    ASSERT_EQ(rtc_vec_push(&v, &y), RTC_OK);
    ASSERT_EQ(*(int *)rtc_vec_at(&v, 0), 8);
    rtc_vec_free(&v);
}

int main(void) {
    printf("\n========== rtc_vec tests ==========\n");
    RUN_TEST(init_and_free);
    RUN_TEST(init_invalid);
    RUN_TEST(push_and_at);
    RUN_TEST(push_struct);
    RUN_TEST(reserve_grows_capacity);
    RUN_TEST(remove_shifts_elements);
    RUN_TEST(swap_remove_o1);
    RUN_TEST(clear_preserves_capacity);
    RUN_TEST(stress_push);
    RUN_TEST(reuse_after_free);
    TEST_SUMMARY();
}
