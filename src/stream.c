// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <stdlib.h>
#include <string.h>
#include "stream.h"

static void LIBUSB_CALL xfer_done(struct libusb_transfer *t)
{
	struct ts_stream *s = t->user_data;

	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		if (t->actual_length > 0) {
			s->bytes += t->actual_length;
			s->cb(t->buffer, t->actual_length, s->opaque);
		}
	} else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
		s->errors++;
		DBG(1, "TS transfer: %s\n", libusb_error_name(t->status));
	}
	if (s->running && t->status != LIBUSB_TRANSFER_NO_DEVICE &&
	    !libusb_submit_transfer(t))
		return;
	s->active--;
}

static void *event_thread(void *arg)
{
	struct ts_stream *s = arg;
	struct timeval tv = { 0, 100000 };

	while (s->running || s->active > 0)
		libusb_handle_events_timeout_completed(s->usb->ctx, &tv, NULL);
	return NULL;
}

int ts_stream_start(struct ts_stream *s, struct fx2 *usb, int n_xfers, int xfer_size,
		    ts_data_cb cb, void *opaque)
{
	int ret;

	memset(s, 0, sizeof(*s));
	s->usb = usb;
	s->cb = cb;
	s->opaque = opaque;
	s->n_xfers = n_xfers;
	s->xfers = calloc(n_xfers, sizeof(*s->xfers));
	if (!s->xfers)
		return -ENOMEM;

	libusb_clear_halt(usb->h, TBS5530_TS_EP);
	s->running = true;
	for (int i = 0; i < n_xfers; i++) {
		u8 *buf = malloc(xfer_size);

		s->xfers[i] = libusb_alloc_transfer(0);
		if (!buf || !s->xfers[i]) {
			free(buf);
			goto err;
		}
		libusb_fill_bulk_transfer(s->xfers[i], usb->h, TBS5530_TS_EP, buf, xfer_size,
					  xfer_done, s, 0);
		s->xfers[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;
		ret = libusb_submit_transfer(s->xfers[i]);
		if (ret) {
			LOG("cannot submit TS transfer: %s\n", libusb_error_name(ret));
			goto err;
		}
		s->active++;
	}
	if (pthread_create(&s->thread, NULL, event_thread, s))
		goto err;
	return 0;
err:
	s->running = false;
	ts_stream_stop(s);
	return -EIO;
}

void ts_stream_stop(struct ts_stream *s)
{
	struct timeval tv = { 0, 50000 };

	if (!s->xfers)
		return;
	if (s->running) {
		s->running = false;
		for (int i = 0; i < s->n_xfers; i++)
			if (s->xfers[i])
				libusb_cancel_transfer(s->xfers[i]);
		pthread_join(s->thread, NULL);
	} else {
		/* start failed before the event thread existed */
		for (int i = 0; i < s->n_xfers; i++)
			if (s->xfers[i])
				libusb_cancel_transfer(s->xfers[i]);
		for (int k = 0; k < 40 && s->active > 0; k++)
			libusb_handle_events_timeout_completed(s->usb->ctx, &tv, NULL);
	}
	for (int i = 0; i < s->n_xfers; i++)
		if (s->xfers[i])
			libusb_free_transfer(s->xfers[i]);
	free(s->xfers);
	s->xfers = NULL;
}
