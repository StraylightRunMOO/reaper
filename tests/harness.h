/* =====================================================================
 * tests/harness.h — minimal dependency-free test harness for reaper.h
 *
 * Defines the macros expected by test_reaper.c:
 *   TEST(name), RUN_TEST(name), TEST_SUITE(name),
 *   ASSERT_NOT_NULL(p), ASSERT_EQ(a,b), ASSERT_TRUE(x), ASSERT_FALSE(x)
 *
 * Uses C11 + setjmp/longjmp. Keeps the harness small enough to ship with
 * the library instead of pulling in an external framework.
 * ===================================================================== */

#ifndef REAPER_TEST_HARNESS_H
#define REAPER_TEST_HARNESS_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static jmp_buf rpr_test_jb;
static const char *rpr_test_name;
static const char *rpr_test_fail_msg;
static int rpr_test_failures;
static int rpr_test_total;

#define TEST(name) static void name(void)

#define RUN_TEST(name) do {                                      \
    rpr_test_name = #name;                                       \
    rpr_test_total++;                                            \
    if (setjmp(rpr_test_jb) == 0) {                              \
        name();                                                  \
        printf("  PASS %s\n", #name);                            \
    } else {                                                     \
        rpr_test_failures++;                                     \
        printf("  FAIL %s: %s\n", rpr_test_name,                \
               rpr_test_fail_msg ? rpr_test_fail_msg : "");      \
    }                                                            \
} while (0)

#define TEST_SUITE(name) printf("%s\n", name)

#define ASSERT_NOT_NULL(p) do {                                  \
    if ((p) == NULL) {                                           \
        rpr_test_fail_msg = #p " is NULL";                       \
        longjmp(rpr_test_jb, 1);                                 \
    }                                                            \
} while (0)

#define ASSERT_EQ(a, b) do {                                     \
    if ((a) != (b)) {                                            \
        rpr_test_fail_msg = #a " != " #b;                        \
        longjmp(rpr_test_jb, 1);                                 \
    }                                                            \
} while (0)

#define ASSERT_TRUE(x) do {                                      \
    if (!(x)) {                                                  \
        rpr_test_fail_msg = #x " is false";                      \
        longjmp(rpr_test_jb, 1);                                 \
    }                                                            \
} while (0)

#define ASSERT_FALSE(x) do {                                     \
    if (x) {                                                     \
        rpr_test_fail_msg = #x " is true";                       \
        longjmp(rpr_test_jb, 1);                                 \
    }                                                            \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* REAPER_TEST_HARNESS_H */
