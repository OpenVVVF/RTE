#include "test_framework.h"

#include <cstdio>

int main() {
    std::printf("=== Gen7 I2C driver host verification ===\n");
    hosttest::test_temp_suite();
    hosttest::test_rail_suite();

    std::printf("=== %d/%d checks passed", hosttest::g_checks.passed,
                hosttest::g_checks.total);
    if (hosttest::g_checks.failed != 0) {
        std::printf("  (%d FAILED)", hosttest::g_checks.failed);
    }
    std::printf(" ===\n");
    return hosttest::g_checks.failed == 0 ? 0 : 1;
}
