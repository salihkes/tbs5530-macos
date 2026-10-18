/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MPEG-TS from the FX2 bulk endpoint. The FX2 FIFO holds only a few KB, so
 * many transfers stay queued and one thread resubmits them as they finish.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef STREAM_H
#define STREAM_H

#include <pthread.h>
#include <stdatomic.h>
#include "fx2.h"

typedef void (*ts_data_cb)(const u8 *buf, int len, void *opaque);

struct ts_stream {
	struct fx2 *usb;
	struct libusb_transfer **xfers;
	int n_xfers;
	atomic_int active;
	atomic_bool running;
	pthread_t thread;
	ts_data_cb cb;		/* runs on the event thread */
	void *opaque;
	atomic_ullong bytes;
	atomic_int errors;
};

int ts_stream_start(struct ts_stream *s, struct fx2 *usb, int n_xfers, int xfer_size,
		    ts_data_cb cb, void *opaque);
void ts_stream_stop(struct ts_stream *s);

#endif
