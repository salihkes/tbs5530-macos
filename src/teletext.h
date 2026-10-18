/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DVB teletext: ETSI EN 300 472 (PES) and EN 300 706 (packets, Hamming 8/4,
 * odd parity, national option sub-sets).
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef TELETEXT_H
#define TELETEXT_H

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "common.h"

#define TTX_ROWS	25
#define TTX_COLS	40
#define TTX_MAXSUB	32

struct ttx_page {
	u16 sub;			/* subcode S4..S1 */
	u8 natopt;			/* C12 C13 C14 as a 3-bit number */
	u8 text[TTX_ROWS][TTX_COLS];	/* 7-bit codes, parity removed */
	time_t updated;
};

struct ttx_entry {
	int n;				/* subpages stored */
	int next;			/* slot to overwrite when full */
	struct ttx_page sub[TTX_MAXSUB];
};

struct ttx {
	int pid;
	bool turkish;			/* national option 110 as Turkish, not Czech/Slovak */
	pthread_mutex_t lock;		/* fed on the USB thread, read by HTTP */
	struct ttx_entry *pages[0x800];	/* index (magazine & 7) << 8 | page byte */
	struct ttx_page cur[8];		/* page being received, per magazine */
	int cur_page[8];		/* index into pages[] or -1 */
	u8 pes[65536 + 184];
	int pes_len;
	bool pes_active;
	u64 lines, bad_ham;
};

void ttx_init(struct ttx *t, int pid);
void ttx_free(struct ttx *t);
/* feed one 188-byte TS packet (others PIDs are ignored) */
void ttx_packet(struct ttx *t, const u8 *pkt);
/* store pages still being received (end of a file) */
void ttx_flush(struct ttx *t);

/* page numbers as people write them: 100..899 */
int ttx_count(struct ttx *t);
void ttx_print_index(struct ttx *t, FILE *out);
int ttx_print_text(struct ttx *t, int page, FILE *out);	/* -1 if not received */
void ttx_html_index(struct ttx *t, FILE *out);
int ttx_html_page(struct ttx *t, int page, FILE *out);		/* -1 if not received */

#endif
