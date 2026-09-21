#ifndef TFM_TEST_H
#define TFM_TEST_H

/* Minimal header-only test harness for tfm. No external dependency (matches
 * this project's zero-dependency build philosophy) - include this in one .c
 * file per test binary; that file writes its own main() using TFM_RUN() and
 * TFM_SUMMARY(), the same way an existing project convention would look if
 * one existed here already.
 *
 * Usage:
 *   #include "test.h"
 *   TEST(copy_preserves_content) {
 *       ASSERT_TRUE(some_condition);
 *   }
 *   int main(void) {
 *       TFM_RUN(copy_preserves_content);
 *       return TFM_SUMMARY();
 *   }
 *
 * ASSERT_* failures print file:line and the failing expression, then
 * `return;` out of the current test function immediately (rather than just
 * flagging failure and continuing) - a wrong precondition in a filesystem
 * test (e.g. a file that unexpectedly wasn't created) would otherwise lead
 * the rest of the test into a NULL-deref or garbage-path crash instead of a
 * clean, readable failure report. */

#include <stdio.h>
#include <string.h>

static int tfm_test_failed;
static int tfm_tests_run;
static int tfm_tests_passed;

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "    FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #cond); \
            tfm_test_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) \
    do { \
        long long tfm_a_ = (long long)(a); \
        long long tfm_b_ = (long long)(b); \
        if (tfm_a_ != tfm_b_) { \
            fprintf(stderr, "    FAIL %s:%d: ASSERT_EQ(%s, %s): %lld != %lld\n", \
                    __FILE__, __LINE__, #a, #b, tfm_a_, tfm_b_); \
            tfm_test_failed = 1; \
            return; \
        } \
    } while (0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        const char *tfm_a_ = (a); \
        const char *tfm_b_ = (b); \
        if (tfm_a_ == NULL || tfm_b_ == NULL || strcmp(tfm_a_, tfm_b_) != 0) { \
            fprintf(stderr, "    FAIL %s:%d: ASSERT_STR_EQ(%s, %s): \"%s\" != \"%s\"\n", \
                    __FILE__, __LINE__, #a, #b, \
                    tfm_a_ ? tfm_a_ : "(null)", tfm_b_ ? tfm_b_ : "(null)"); \
            tfm_test_failed = 1; \
            return; \
        } \
    } while (0)

#define TEST(name) static void tfm_test_##name(void)

#define TFM_RUN(name) \
    do { \
        tfm_test_failed = 0; \
        tfm_tests_run++; \
        tfm_test_##name(); \
        if (tfm_test_failed) { \
            fprintf(stderr, "FAIL %s\n", #name); \
        } else { \
            fprintf(stderr, "PASS %s\n", #name); \
            tfm_tests_passed++; \
        } \
    } while (0)

#define TFM_SUMMARY() \
    (fprintf(stderr, "\n%d/%d tests passed\n", tfm_tests_passed, tfm_tests_run), \
     tfm_tests_passed == tfm_tests_run ? 0 : 1)

#endif
