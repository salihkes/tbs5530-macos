/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define __noreturn __attribute__((noreturn))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

extern int g_verbose;

#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define DBG(lvl, ...)						\
	do {							\
		if (g_verbose >= (lvl))				\
			fprintf(stderr, __VA_ARGS__);		\
	} while (0)

void msleep(unsigned int ms);
uint64_t now_ms(void);
void hexdump(const char *prefix, const u8 *buf, int len);

#endif
