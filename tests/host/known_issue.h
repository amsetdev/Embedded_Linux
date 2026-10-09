/*
 * KNOWN_ISSUE(correct_condition, "why") — Unity counterpart of pytest's
 * xfail(strict=True).
 *
 * Write the condition the code SHOULD satisfy. While the bug exists the test
 * is reported IGNORED with the reason; once the bug is fixed the condition
 * holds and the test FAILS, reminding you to turn it into a normal assertion.
 */
#ifndef KNOWN_ISSUE_H
#define KNOWN_ISSUE_H

#include "unity.h"

#define KNOWN_ISSUE(correct_condition, reason)                                         \
    do {                                                                               \
        if (correct_condition) {                                                       \
            TEST_FAIL_MESSAGE("Known issue now FIXED - replace KNOWN_ISSUE with a "    \
                              "normal assertion: " reason);                            \
        }                                                                              \
        TEST_IGNORE_MESSAGE("KNOWN ISSUE: " reason);                                   \
    } while (0)

#endif /* KNOWN_ISSUE_H */
