// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <stdio.h>
#include <string.h>
#include "ts.h"

void ts_stats_init(struct ts_stats *st)
{
	memset(st, 0, sizeof(*st));
	memset(st->last_cc, -1, sizeof(st->last_cc));
}

static void one_packet(struct ts_stats *st, const u8 *p,
		       void (*pkt)(const u8 *, void *), void *opaque)
{
	int pid = ts_pid(p);
	int cc = p[3] & 0x0f;
	bool has_payload = p[3] & 0x10;

	st->packets++;
	st->pid_count[pid]++;
	if (p[1] & 0x80)
		st->tei++;
	/* CC only advances on packets with payload; PID 0x1fff is padding */
	if (pid != 0x1fff && has_payload) {
		if (st->last_cc[pid] >= 0 && cc != ((st->last_cc[pid] + 1) & 0x0f) &&
		    cc != st->last_cc[pid])
			st->cc_errors++;
		st->last_cc[pid] = cc;
	}
	if (pkt)
		pkt(p, opaque);
}

void ts_feed(struct ts_stats *st, const u8 *buf, int len,
	     void (*pkt)(const u8 *, void *), void *opaque)
{
	/* finish a packet split across the previous buffer */
	if (st->carry_len) {
		int need = TS_PACKET - st->carry_len;

		if (len < need) {
			memcpy(st->carry + st->carry_len, buf, len);
			st->carry_len += len;
			return;
		}
		memcpy(st->carry + st->carry_len, buf, need);
		st->carry_len = 0;
		if (st->carry[0] == TS_SYNC)
			one_packet(st, st->carry, pkt, opaque);
		buf += need;
		len -= need;
	}
	while (len >= TS_PACKET) {
		/* aligned if this and the next packet both start with 0x47 */
		if (buf[0] == TS_SYNC && (len == TS_PACKET || len < 2 * TS_PACKET ||
					  buf[TS_PACKET] == TS_SYNC)) {
			one_packet(st, buf, pkt, opaque);
			buf += TS_PACKET;
			len -= TS_PACKET;
		} else {
			st->sync_lost++;
			buf++;
			len--;
		}
	}
	if (len > 0) {
		memcpy(st->carry, buf, len);
		st->carry_len = len;
	}
}

void ts_stats_print(const struct ts_stats *st, double seconds)
{
	int shown = 0;

	printf("packets %llu (%.1f Mbit/s), sync lost %llu bytes, CC errors %llu, TEI %llu\n",
	       (unsigned long long)st->packets,
	       seconds > 0 ? st->packets * TS_PACKET * 8 / seconds / 1e6 : 0.0,
	       (unsigned long long)st->sync_lost, (unsigned long long)st->cc_errors,
	       (unsigned long long)st->tei);
	printf("PIDs:");
	for (int pid = 0; pid < TS_PIDS; pid++) {
		if (!st->pid_count[pid])
			continue;
		printf("%s %d:%llu", shown % 8 ? "" : "\n ", pid,
		       (unsigned long long)st->pid_count[pid]);
		shown++;
	}
	printf("\n");
}
