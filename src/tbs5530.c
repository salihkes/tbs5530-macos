// SPDX-License-Identifier: GPL-2.0
/*
 * TBS5530 board glue: the FX2 as an I2C adapter and the satellite frontend.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 * Based on linux_media drivers/media/usb/dvb-usb/tbs5530.c,
 * Copyright (c) 2022 Davin <Davin@tbsdtv.com>
 */
#include "tbs5530.h"
#include "m88rs6060.h"

/* the firmware only does "write" (0x80) and "write register, read" (0x90 + 0x91) */
static int tbs5530_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msg, int num)
{
	struct fx2 *d = adap->priv;
	int ret;

	if (num == 2 && !(msg[0].flags & I2C_M_RD) && (msg[1].flags & I2C_M_RD) &&
	    msg[0].len == 1) {
		ret = fx2_i2c_read_reg(d, msg[0].addr, msg[0].buf[0], msg[1].buf, msg[1].len);
		return ret == msg[1].len ? 2 : -EIO;
	}
	if (num == 1 && !(msg[0].flags & I2C_M_RD)) {
		ret = fx2_i2c_write(d, msg[0].addr, msg[0].buf, msg[0].len);
		return ret ? -EIO : 1;
	}
	LOG("tbs5530: unsupported i2c transfer (num=%d)\n", num);
	return -EOPNOTSUPP;
}

int tbs5530_open(struct tbs5530 *t)
{
	char path[1024];

	memset(t, 0, sizeof(*t));
	if (fx2_open(&t->usb))
		return -ENODEV;
	if (g_fw_reload || !fx2_is_warm(&t->usb)) {
		snprintf(path, sizeof(path), "%s/dvb-usb-id5530.fw", g_fw_dir);
		if (fx2_load_firmware(&t->usb, path))
			return -EIO;
	}
	t->i2c.dev.name = "tbs5530";
	t->i2c.xfer = tbs5530_i2c_xfer;
	t->i2c.priv = &t->usb;
	return 0;
}

void tbs5530_close(struct tbs5530 *t)
{
	fx2_close(&t->usb);
}

int tbs5530_sat_attach(struct tbs5530 *t)
{
	struct m88rs6060_cfg cfg = {
		.clk = 27000000,
		.i2c_wr_max = 33,
		.ts_mode = MtFeTsOutMode_Common,
		.demod_adr = 0x69,
		.tuner_adr = 0x2c,
		.repeater_value = 0x11,
	};
	u8 buf[2];

	t->i2c.dev.name = "m88rs6060";
	t->sat = m88rs6060_attach(&cfg, &t->i2c);
	if (!t->sat)
		return -ENODEV;

	/* same order as the Linux driver: probe the demod first, then power up */
	buf[0] = 0;
	buf[1] = 0;
	fx2_ctrl(&t->usb, false, REQ_B7, 0, buf, 2);
	buf[0] = 8;
	buf[1] = 1;
	fx2_ctrl(&t->usb, false, REQ_BOARD, 0, buf, 2);
	msleep(10);

	return t->sat->ops.init(t->sat);
}

/* {10, 1} routes the satellite demod's TS to the USB endpoint */
int tbs5530_sat_ts_select(struct tbs5530 *t)
{
	u8 buf[2] = { 10, 1 };

	return fx2_ctrl(&t->usb, false, REQ_BOARD, 0, buf, 2) == 2 ? 0 : -EIO;
}
