/* check.h - tiny shared test harness (no framework dependency).
 * SPDX-License-Identifier: MIT */
#ifndef DX_TEST_CHECK_H
#define DX_TEST_CHECK_H

#include <stdio.h>

/*
 * CHECK counts every assertion and keeps going, so one run lists all failures.
 * dx_test_done() prints the summary line and returns the process exit code.
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

static inline int dx_test_done(void)
{
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}

#endif
