#pragma once

/**
 * @brief Minimal assertion helpers for the host harness (no dependencies).
 */

#include <cmath>
#include <cstdio>

namespace hosttest {

struct Counters {
    int passed = 0;
    int failed = 0;
    int total  = 0;
};

inline Counters g_checks;

inline void report(bool ok, const char* expr, const char* file, int line) {
    ++g_checks.total;
    if (ok) {
        ++g_checks.passed;
        std::printf("  PASS  %s\n", expr);
    } else {
        ++g_checks.failed;
        std::printf("  FAIL  %s  (%s:%d)\n", expr, file, line);
    }
}

inline void reportNear(double actual, double expect, double tol,
                       const char* expr, const char* file, int line) {
    const bool ok = std::fabs(actual - expect) <= tol;
    ++g_checks.total;
    if (ok) {
        ++g_checks.passed;
        std::printf("  PASS  %s = %.6f (expect %.6f +/- %.4g)\n",
                    expr, actual, expect, tol);
    } else {
        ++g_checks.failed;
        std::printf("  FAIL  %s = %.6f, expected %.6f +/- %.4g  (%s:%d)\n",
                    expr, actual, expect, tol, file, line);
    }
}

#define CHECK(cond) \
    ::hosttest::report((cond), #cond, __FILE__, __LINE__)
#define CHECK_NEAR(actual, expect, tol) \
    ::hosttest::reportNear((actual), (expect), (tol), #actual, __FILE__, __LINE__)

void test_temp_suite();
void test_rail_suite();
void test_temp_adv_suite();
void test_rail_adv_suite();

} // namespace hosttest
