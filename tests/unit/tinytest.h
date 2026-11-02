/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal unit test harness: no dependencies, works on host and target. */
#ifndef TINYTEST_H
#define TINYTEST_H

#include <stdio.h>
#include <string.h>

static int tt_fail, tt_checks;

#define CHECK(cond)                                                              \
	do {                                                                     \
		tt_checks++;                                                     \
		if (!(cond)) {                                                   \
			tt_fail++;                                               \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

#define CHECK_EQ(a, b)                                                                \
	do {                                                                          \
		long long _a = (long long)(a), _b = (long long)(b);                   \
		tt_checks++;                                                          \
		if (_a != _b) {                                                       \
			tt_fail++;                                                    \
			printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,   \
			       __LINE__, #a, #b, _a, _b);                             \
		}                                                                     \
	} while (0)

#define RUN(fn)                                  \
	do {                                     \
		printf("[ RUN  ] %s\n", #fn);    \
		fn();                            \
	} while (0)

#define TT_END()                                                                   \
	do {                                                                       \
		printf("%s: %d checks, %d failures\n", tt_fail ? "FAILED" : "PASSED", \
		       tt_checks, tt_fail);                                        \
		return tt_fail ? 1 : 0;                                            \
	} while (0)

#endif
