/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal HTTP server for one live MPEG-TS stream, a playlist and HTML
 * pages. Each client has its own ring buffer and writer thread, so a slow
 * viewer never stalls the tuner.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef HTTP_H
#define HTTP_H

#include <pthread.h>
#include <stdatomic.h>
#include "common.h"

struct http_client;

struct http {
	int listen_fd;
	int port;
	const char *name;	/* channel name for the playlist */
	const char *path;	/* stream path */
	pthread_t accept_thread;
	pthread_mutex_t lock;
	struct http_client *clients;
	atomic_bool running;
	/* other pages (teletext): write HTML to out, return 0, or -1 for 404 */
	int (*route)(const char *path, FILE *out, void *opaque);
	void *route_opaque;
};

int http_start(struct http *h, int port, const char *name, const char *path);
/* queue whole TS packets for every connected client */
void http_send(struct http *h, const u8 *buf, int len);
int http_clients(struct http *h);
void http_stop(struct http *h);

#endif
