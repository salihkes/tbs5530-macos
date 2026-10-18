/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef SAT_H
#define SAT_H

#include "tbs5530.h"

struct sat_tune {
	u32 freq_khz;		/* transponder frequency */
	bool pol_h;		/* horizontal = 18 V, vertical = 13 V */
	u32 symbol_rate;	/* symbols/s */
	int port;		/* DiSEqC 1.0 committed port 0..3, -1 for none */
	bool dvbs2;
};

/* universal LNB */
#define LNB_LOF_LO	9750000
#define LNB_LOF_HI	10600000
#define LNB_SLOF	11700000

int sat_tune(struct tbs5530 *t, const struct sat_tune *p, int timeout_ms);
void sat_print_status(struct tbs5530 *t);

#endif
