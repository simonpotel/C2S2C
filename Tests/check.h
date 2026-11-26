/* check.h - tiny shared test harness (no framework dependency).
 * SPDX-License-Identifier: MIT */
#ifndef C2S2C_TEST_CHECK_H
#define C2S2C_TEST_CHECK_H

#include <stdio.h>

/*
 * CHECK counts every assertion and keeps going, so one run lists all failures.
 * c2s2c_test_done() prints the summary line and returns the process exit code.
 */
static int g_pass, g_fail;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (c)                                                                                     \
            g_pass++;                                                                              \
        else {                                                                                     \
            g_fail++;                                                                              \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                     \
        }                                                                                          \
    } while (0)

static inline int c2s2c_test_done(void)
{
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}

#endif
