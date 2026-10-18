/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MPEG-TS packet checks: sync, PIDs, continuity counters.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef TS_H
#define TS_H

#include "common.h"

#define TS_PACKET 188
#define TS_SYNC 0x47
#define TS_PIDS 8192

struct ts_stats {
	u64 packets;
	u64 sync_lost;		/* bytes skipped to find 0x47 again */
	u64 cc_errors;		/* continuity counter jumps = lost packets */
	u64 tei;		/* transport error indicator set by the demod */
	u64 pid_count[TS_PIDS];
	int8_t last_cc[TS_PIDS];

	u8 carry[TS_PACKET];	/* partial packet across buffer boundaries */
	int carry_len;
};

void ts_stats_init(struct ts_stats *st);
/* feed raw bytes; calls pkt() for each aligned 188-byte packet if non-NULL */
void ts_feed(struct ts_stats *st, const u8 *buf, int len,
	     void (*pkt)(const u8 *p, void *opaque), void *opaque);
void ts_stats_print(const struct ts_stats *st, double seconds);

static inline int ts_pid(const u8 *p) { return (p[1] & 0x1f) << 8 | p[2]; }

#endif
