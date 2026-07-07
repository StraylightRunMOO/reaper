/* =====================================================================
 * tests/main.cpp — C++ test driver for reaper.hpp
 * ===================================================================== */

#include <cstdio>

#define REAPER_IMPLEMENTATION
#include "reaper.hpp"

#include "harness.h"
#include "test_reaper_cpp.cpp"

int main(void) {
    rpr_test_failures = 0;
    rpr_test_total = 0;

    run_reaper_cpp_tests();

    printf("\n%d/%d tests passed\n",
           rpr_test_total - rpr_test_failures, rpr_test_total);

    return rpr_test_failures;
}
