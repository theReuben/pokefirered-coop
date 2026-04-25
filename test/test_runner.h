#ifndef GUARD_TEST_RUNNER_H
#define GUARD_TEST_RUNNER_H

#include <stdio.h>
#include <stdlib.h>

static int sTestsPassed = 0;
static int sTestsFailed = 0;

#define ASSERT(cond) do { \
    if (cond) { \
        sTestsPassed++; \
    } else { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        sTestsFailed++; \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))

#define TEST_SUMMARY() do { \
    printf("%s: %d passed, %d failed\n", \
           sTestsFailed == 0 ? "PASS" : "FAIL", \
           sTestsPassed, sTestsFailed); \
    return sTestsFailed == 0 ? 0 : 1; \
} while (0)

#endif // GUARD_TEST_RUNNER_H
