/*
 * Minimal test harness for mrtc component tests.
 *
 * Usage:
 *   TEST(name) { ... ASSERT(...); ... }
 *   int main() { RUN_TEST(name); TEST_SUMMARY(); }
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _test_pass_count = 0;
static int _test_fail_count = 0;
static int _test_current_failed = 0;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name)                      \
    do {                                    \
        _test_current_failed = 0;           \
        printf("  [RUN ] %s\n", #name);     \
        test_##name();                      \
        if (_test_current_failed) {         \
            printf("  [FAIL] %s\n", #name); \
            _test_fail_count++;             \
        } else {                            \
            printf("  [ OK ] %s\n", #name); \
            _test_pass_count++;             \
        }                                   \
    } while (0)

#define ASSERT(cond)                                                               \
    do {                                                                           \
        if (!(cond)) {                                                             \
            printf("    ASSERT FAILED: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            _test_current_failed = 1;                                              \
            return;                                                                \
        }                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                      \
    do {                                                                                     \
        if ((a) != (b)) {                                                                    \
            printf("    ASSERT_EQ FAILED: %s == %s  (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
            printf("      got: %d  expected: %d\n", (int)(a), (int)(b));                     \
            _test_current_failed = 1;                                                        \
            return;                                                                          \
        }                                                                                    \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                      \
    do {                                                                                         \
        if (strcmp((a), (b)) != 0) {                                                             \
            printf("    ASSERT_STR_EQ FAILED: %s == %s  (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
            printf("      got: \"%s\"  expected: \"%s\"\n", (a), (b));                           \
            _test_current_failed = 1;                                                            \
            return;                                                                              \
        }                                                                                        \
    } while (0)

#define ASSERT_MEM_EQ(a, b, len)                                                        \
    do {                                                                                \
        if (memcmp((a), (b), (len)) != 0) {                                             \
            printf("    ASSERT_MEM_EQ FAILED: %s == %s (%zu bytes)  (%s:%d)\n", #a, #b, \
                   (size_t)(len), __FILE__, __LINE__);                                  \
            _test_current_failed = 1;                                                   \
            return;                                                                     \
        }                                                                               \
    } while (0)

#define TEST_SUMMARY()                                                                   \
    do {                                                                                 \
        printf("\n========================================\n");                          \
        printf("  Results: %d passed, %d failed\n", _test_pass_count, _test_fail_count); \
        printf("========================================\n");                            \
        return _test_fail_count ? 1 : 0;                                                 \
    } while (0)

#endif /* TEST_HARNESS_H */
