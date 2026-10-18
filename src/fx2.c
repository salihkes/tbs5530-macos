// SPDX-License-Identifier: GPL-2.0
/*
 * TBS5530 USB bridge (Cypress FX2) over libusb.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 * Protocol from linux_media drivers/media/usb/dvb-usb/tbs5530.c,
 * Copyright (c) 2022 Davin <Davin@tbsdtv.com>
 */
#include <stdlib.h>
#include <string.h>
#include "fx2.h"

#define USB_TIMEOUT_MS	2000

/* 8051 reset control; 0x7f92 is the same register on AN21xx parts */
#define FX2_CPUCS	0xe600
#define FX2_CPUCS_OLD	0x7f92

/*
 * The TBS firmware's SET_CONFIGURATION handler sets EP2AUTOINLEN to 0x0400:
 * 1024-byte packets on a 512-byte high-speed bulk endpoint. macOS rejects
 * them (LIBUSB_ERROR_OVERFLOW), so turn the MOV A,#04h into MOV A,#02h in
 * the RAM copy. Only patched when the surrounding bytes match.
 */
#define SETCFG_PATCH_AT	0x0dcd
static const u8 setcfg_1024[] = { 0x90, 0xe6, 0x20, 0x74, 0x04, 0xf0 };

int fx2_ctrl(struct fx2 *d, bool in, u8 req, u16 value, u8 *data, u16 len)
{
	u8 type = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
		  (in ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT);
	int ret = libusb_control_transfer(d->h, type, req, value, 0, data, len,
					  USB_TIMEOUT_MS);

	if (g_verbose >= 3) {
		char prefix[64];

		snprintf(prefix, sizeof(prefix), "usb %s req=%02x val=%04x ret=%d:",
			 in ? "in " : "out", req, value, ret);
		hexdump(prefix, data, in ? (ret > 0 ? ret : 0) : len);
	}
	return ret;
}

int fx2_open(struct fx2 *d)
{
	int ret;

	memset(d, 0, sizeof(*d));
	ret = libusb_init(&d->ctx);
	if (ret) {
		LOG("libusb_init: %s\n", libusb_error_name(ret));
		return -EIO;
	}
	d->h = libusb_open_device_with_vid_pid(d->ctx, TBS5530_VID, TBS5530_PID);
	if (!d->h) {
		LOG("TBS5530 (%04x:%04x) not found\n", TBS5530_VID, TBS5530_PID);
		return -ENODEV;
	}
	ret = libusb_claim_interface(d->h, 0);
	if (ret) {
		LOG("cannot claim interface 0: %s (is another program using it?)\n",
		    libusb_error_name(ret));
		return -EBUSY;
	}
	d->claimed = true;
	return 0;
}

void fx2_close(struct fx2 *d)
{
	if (d->h) {
		if (d->claimed)
			libusb_release_interface(d->h, 0);
		libusb_close(d->h);
	}
	if (d->ctx)
		libusb_exit(d->ctx);
	memset(d, 0, sizeof(*d));
}

/* the boot ROM stalls everything but 0xa0, so an I2C read only works warm */
bool fx2_is_warm(struct fx2 *d)
{
	u8 b;

	return fx2_i2c_read_reg(d, TBS5530_EEPROM >> 1, 0, &b, 1) == 1;
}

static int read_file(const char *path, u8 **data, size_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -ENOENT;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	*data = malloc(n > 0 ? n : 1);
	*size = *data ? fread(*data, 1, n, f) : 0;
	fclose(f);
	return *data && (long)*size == n ? 0 : -EIO;
}

int fx2_load_firmware(struct fx2 *d, const char *path)
{
	u8 *fw, reset;
	size_t size, i;
	int ret;

	ret = read_file(path, &fw, &size);
	if (ret) {
		LOG("cannot read firmware '%s'\n", path);
		return ret;
	}
	LOG("uploading %s (%zu bytes)\n", path, size);

	if (size > SETCFG_PATCH_AT + sizeof(setcfg_1024) &&
	    !memcmp(fw + SETCFG_PATCH_AT, setcfg_1024, sizeof(setcfg_1024))) {
		fw[SETCFG_PATCH_AT + 4] = 0x02;
		DBG(1, "patched SET_CONFIGURATION: EP2AUTOINLEN 1024 -> 512\n");
	}

	reset = 1;
	fx2_ctrl(d, false, REQ_FW_LOAD, FX2_CPUCS_OLD, &reset, 1);
	fx2_ctrl(d, false, REQ_FW_LOAD, FX2_CPUCS, &reset, 1);

	for (i = 0; i < size; i += 0x40) {
		u16 chunk = min(size - i, (size_t)0x40);

		if (fx2_ctrl(d, false, REQ_FW_LOAD, i, fw + i, chunk) != chunk) {
			LOG("firmware write failed at 0x%04zx\n", i);
			ret = -EIO;
			break;
		}
	}

	reset = 0;
	if (!ret && (fx2_ctrl(d, false, REQ_FW_LOAD, FX2_CPUCS_OLD, &reset, 1) != 1 ||
		     fx2_ctrl(d, false, REQ_FW_LOAD, FX2_CPUCS, &reset, 1) != 1)) {
		LOG("could not restart the FX2 CPU\n");
		ret = -EIO;
	}
	free(fw);
	return ret ? ret : fx2_reopen(d);
}

/* the firmware re-enumerates with new descriptors, so the old handle is stale */
int fx2_reopen(struct fx2 *d)
{
	for (int t = 0; t < 50; t++) {
		msleep(100);
		if (d->h) {
			if (d->claimed)
				libusb_release_interface(d->h, 0);
			libusb_close(d->h);
			d->h = NULL;
			d->claimed = false;
		}
		d->h = libusb_open_device_with_vid_pid(d->ctx, TBS5530_VID, TBS5530_PID);
		if (d->h && !libusb_claim_interface(d->h, 0)) {
			d->claimed = true;
			if (fx2_is_warm(d)) {
				DBG(1, "device back after %d ms\n", (t + 1) * 100);
				return 0;
			}
		}
	}
	LOG("device did not come back after the firmware upload\n");
	return -ENODEV;
}

int fx2_i2c_write(struct fx2 *d, u8 addr, const u8 *buf, int len)
{
	u8 msg[64];

	if (len + 2 > (int)sizeof(msg))
		return -EINVAL;
	msg[0] = len + 1;
	msg[1] = addr << 1;
	memcpy(msg + 2, buf, len);
	return fx2_ctrl(d, false, REQ_I2C_WRITE, 0, msg, len + 2) == len + 2 ? 0 : -EIO;
}

/* returns len on success */
int fx2_i2c_read_reg(struct fx2 *d, u8 addr, u8 reg, u8 *buf, int len)
{
	u8 msg[3] = { len, addr << 1, reg };
	u8 in[64];

	if (len > (int)sizeof(in))
		return -EINVAL;
	if (fx2_ctrl(d, false, REQ_I2C_ADDR, 0, msg, 3) != 3)
		return -EIO;
	if (fx2_ctrl(d, true, REQ_I2C_READ, 0, in, len) != len)
		return -EIO;
	memcpy(buf, in, len);
	return len;
}

int fx2_read_eeprom(struct fx2 *d, u8 eeprom[256])
{
	for (int i = 0; i < 256; i++)
		if (fx2_i2c_read_reg(d, TBS5530_EEPROM >> 1, i, &eeprom[i], 1) != 1)
			return -EIO;
	return 0;
}

void fx2_print_descriptors(struct fx2 *d)
{
	static const char * const types[] = { "control", "iso", "bulk", "interrupt" };
	static const char * const speeds[] = { "?", "low", "full", "high", "super", "super+" };
	libusb_device *dev = libusb_get_device(d->h);
	struct libusb_device_descriptor dd;
	struct libusb_config_descriptor *cfg;
	int sp = libusb_get_device_speed(dev);

	libusb_get_device_descriptor(dev, &dd);
	printf("%04x:%04x bcdDevice %04x, %s speed\n", dd.idVendor, dd.idProduct, dd.bcdDevice,
	       sp >= 0 && sp < (int)ARRAY_SIZE(speeds) ? speeds[sp] : "?");
	if (libusb_get_active_config_descriptor(dev, &cfg))
		return;
	for (int i = 0; i < cfg->bNumInterfaces; i++) {
		for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
			const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[a];

			printf("  interface %d alt %d class %02x, %d endpoints\n",
			       id->bInterfaceNumber, id->bAlternateSetting,
			       id->bInterfaceClass, id->bNumEndpoints);
			for (int e = 0; e < id->bNumEndpoints; e++)
				printf("    ep 0x%02x %s maxpacket %d\n",
				       id->endpoint[e].bEndpointAddress,
				       types[id->endpoint[e].bmAttributes & 3],
				       id->endpoint[e].wMaxPacketSize);
		}
	}
	libusb_free_config_descriptor(cfg);
}
