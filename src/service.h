/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Pick one service out of a transponder: follow PAT -> PMT, pass only its
 * PIDs and replace the PAT with a single-program one, so players do not
 * wait for the PMTs that were dropped.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef SERVICE_H
#define SERVICE_H

#include "ts.h"

typedef void (*ts_out_cb)(const u8 *pkt, void *opaque);

struct service_filter {
	u16 sid;		/* program_number */
	int pmt_pid;		/* -1 until found in the PAT */
	int pmt_version;	/* -1 until the PMT was parsed */
	int ttx_pid;		/* teletext PID from the PMT (descriptor 0x56), or -1 */
	bool pass[TS_PIDS];
	u8 pat[TS_PACKET];	/* our single-program PAT */
	bool have_pat;
	u8 pat_cc;
	ts_out_cb out;
	void *opaque;
};

void service_init(struct service_filter *sf, u16 sid, ts_out_cb out, void *opaque);
/* feed one aligned 188-byte packet (use as the ts_feed callback) */
void service_packet(const u8 *p, void *sf);

u32 crc32_mpeg2(const u8 *buf, int len);

#endif
