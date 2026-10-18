/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TBS5530 USB bridge: Cypress FX2 running the TBS firmware.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 * Protocol from linux_media drivers/media/usb/dvb-usb/tbs5530.c,
 * Copyright (c) 2022 Davin <Davin@tbsdtv.com>
 */
#ifndef FX2_H
#define FX2_H

#include <libusb.h>
#include "common.h"

#define TBS5530_VID	0x734c
#define TBS5530_PID	0x5530
#define TBS5530_TS_EP	0x82
#define TBS5530_EEPROM	0xa0

/* vendor requests; 0xa0 is answered by the FX2 core itself */
#define REQ_FW_LOAD	0xa0
#define REQ_I2C_WRITE	0x80
#define REQ_I2C_ADDR	0x90
#define REQ_I2C_READ	0x91
#define REQ_BOARD	0x8a
#define REQ_B7		0xb7

struct fx2 {
	libusb_context *ctx;
	libusb_device_handle *h;
	bool claimed;
};

int fx2_open(struct fx2 *d);
void fx2_close(struct fx2 *d);
int fx2_reopen(struct fx2 *d);

bool fx2_is_warm(struct fx2 *d);
int fx2_load_firmware(struct fx2 *d, const char *path);

int fx2_ctrl(struct fx2 *d, bool in, u8 req, u16 value, u8 *data, u16 len);
int fx2_i2c_write(struct fx2 *d, u8 addr, const u8 *buf, int len);
int fx2_i2c_read_reg(struct fx2 *d, u8 addr, u8 reg, u8 *buf, int len);

int fx2_read_eeprom(struct fx2 *d, u8 eeprom[256]);
void fx2_print_descriptors(struct fx2 *d);

#endif
