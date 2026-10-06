// SPDX-License-Identifier: Apache-2.0
// Minimal test harness for host unit tests. One executable per test file:
//
//   #include "test.h"
//   TEST(adds) { CHECK_EQ_INT(1 + 1, 2); }
//   TEST_MAIN(RUN(adds))
//
// Exit code is the number of failed checks, so CTest reports failures.
#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_test_failures;
static int g_test_checks;
static const char *g_test_current;

#define TEST(name) static void test_##name(void)
#define RUN(name)                                                                                                    \
    do {                                                                                                             \
        g_test_current = #name;                                                                                      \
        int before_ = g_test_failures;                                                                               \
        test_##name();                                                                                               \
        printf("%s %s\n", g_test_failures == before_ ? "[ OK ]" : "[FAIL]", #name);                                  \
    } while (0);

#define TEST_MAIN(...)                                                                                               \
    int main(void) {                                                                                                 \
        __VA_ARGS__                                                                                                  \
        printf("%d checks, %d failures\n", g_test_checks, g_test_failures);                                          \
        return g_test_failures;                                                                                      \
    }

#define TEST_FAIL_(fmt, ...)                                                                                         \
    do {                                                                                                             \
        g_test_failures++;                                                                                           \
        printf("  %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);                                               \
    } while (0)

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        g_test_checks++;                                                                                             \
        if (!(cond)) TEST_FAIL_("CHECK(%s) failed", #cond);                                                          \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                                           \
    do {                                                                                                             \
        long long a_ = (long long)(a), b_ = (long long)(b);                                                          \
        g_test_checks++;                                                                                             \
        if (a_ != b_) TEST_FAIL_("%s == %s failed: %lld vs %lld", #a, #b, a_, b_);                                   \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                        \
    do {                                                                                                             \
        double a_ = (double)(a), b_ = (double)(b);                                                                   \
        g_test_checks++;                                                                                             \
        if (!(fabs(a_ - b_) <= (eps))) TEST_FAIL_("%s ~= %s failed: %g vs %g (eps %g)", #a, #b, a_, b_, (double)(eps)); \
    } while (0)

#define CHECK_STR(a, b)                                                                                              \
    do {                                                                                                             \
        const char *a_ = (a), *b_ = (b);                                                                             \
        g_test_checks++;                                                                                             \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) TEST_FAIL_("%s == %s failed: \"%s\" vs \"%s\"", #a, #b,              \
                                                          a_ ? a_ : "(null)", b_ ? b_ : "(null)");                   \
    } while (0)
