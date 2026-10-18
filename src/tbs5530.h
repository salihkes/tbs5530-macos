/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 * Based on linux_media drivers/media/usb/dvb-usb/tbs5530.c,
 * Copyright (c) 2022 Davin <Davin@tbsdtv.com>
 */
#ifndef TBS5530_H
#define TBS5530_H

#include "fx2.h"
#include "linux_shim.h"

struct tbs5530 {
	struct fx2 usb;
	struct i2c_adapter i2c;
	struct dvb_frontend *sat;
};

int tbs5530_open(struct tbs5530 *t);
void tbs5530_close(struct tbs5530 *t);
int tbs5530_sat_attach(struct tbs5530 *t);
int tbs5530_sat_ts_select(struct tbs5530 *t);

#endif
