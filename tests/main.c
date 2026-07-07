/* =====================================================================
 * tests/main.c — test driver for reaper.h
 *
 * Defines REAPER_IMPLEMENTATION, includes the single header, the harness,
 * and the test suite. Running this program returns 0 on full success or
 * a non-zero count of failed tests.
 * ===================================================================== */

#include <stdio.h>

#define REAPER_IMPLEMENTATION
#include "reaper.h"

#include "harness.h"
#include "test_reaper.c"

int main(void) {
    rpr_test_failures = 0;
    rpr_test_total = 0;

    run_reaper_tests();

    printf("\n%d/%d tests passed\n",
           rpr_test_total - rpr_test_failures, rpr_test_total);

    return rpr_test_failures;
}
