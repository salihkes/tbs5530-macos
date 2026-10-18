// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "http.h"

#define RING_SIZE (8 << 20)	/* ~6 s of a 10 Mbit/s HD channel */

struct http_client {
	struct http_client *next;
	struct http *h;
	int fd;
	char addr[64];
	u8 *ring;
	size_t head, tail;	/* head = write, tail = read; empty when equal */
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool dead;		/* stop writing */
	bool exited;		/* writer thread is gone: safe to free */
};

static size_t ring_used(struct http_client *c)
{
	return (c->head - c->tail + RING_SIZE) % RING_SIZE;
}

static void *client_writer(void *arg)
{
	struct http_client *c = arg;

	pthread_mutex_lock(&c->lock);
	while (!c->dead) {
		size_t n;
		ssize_t w;

		while (c->head == c->tail && !c->dead)
			pthread_cond_wait(&c->cond, &c->lock);
		if (c->dead)
			break;
		/* contiguous part up to the wrap */
		n = c->head > c->tail ? c->head - c->tail : RING_SIZE - c->tail;
		pthread_mutex_unlock(&c->lock);
		w = send(c->fd, c->ring + c->tail, n, 0);
		pthread_mutex_lock(&c->lock);
		if (w <= 0) {
			c->dead = true;
			break;
		}
		c->tail = (c->tail + w) % RING_SIZE;
	}
	pthread_mutex_unlock(&c->lock);
	LOG("client %s disconnected\n", c->addr);
	close(c->fd);
	pthread_mutex_lock(&c->lock);
	c->exited = true;	/* last touch: http_send may free us after this */
	pthread_mutex_unlock(&c->lock);
	return NULL;
}

static void add_client(struct http *h, int fd, const char *peer, const char *path);

static void reply(int fd, const char *status, const char *type, const char *body)
{
	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr),
			 "HTTP/1.0 %s\r\nContent-Type: %s\r\nCache-Control: no-cache\r\n"
			 "Connection: close\r\n\r\n",
			 status, type);

	send(fd, hdr, n, 0);
	if (body)
		send(fd, body, strlen(body), 0);
}

static void handle(struct http *h, int fd, const char *peer)
{
	char req[2048], path[256] = "", host[256] = "", *p;
	int n = 0, r;

	/* read the request headers (until the blank line) */
	while (n < (int)sizeof(req) - 1 && (r = recv(fd, req + n, sizeof(req) - 1 - n, 0)) > 0) {
		n += r;
		req[n] = 0;
		if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
			break;
	}
	req[n] = 0;
	if (sscanf(req, "GET %255s", path) != 1) {
		close(fd);
		return;
	}
	p = strcasestr(req, "\nHost:");
	if (p)
		sscanf(p + 6, " %255[^\r\n]", host);

	if (strcmp(path, "/playlist.m3u") == 0 || strcmp(path, "/") == 0) {
		char body[768];

		/* use the Host header so the playlist works from other machines too */
		snprintf(body, sizeof(body), "#EXTM3U\n#EXTINF:-1,%s\nhttp://%s%s\n", h->name,
			 host[0] ? host : "127.0.0.1", h->path);
		reply(fd, "200 OK", "audio/x-mpegurl", body);
		close(fd);
		return;
	}
	if (strcmp(path, h->path) != 0 && h->route) {
		char *body = NULL;
		size_t len = 0;
		FILE *f = open_memstream(&body, &len);
		int r = f ? h->route(path, f, h->route_opaque) : -1;

		if (f)
			fclose(f);
		if (r == 0) {
			reply(fd, "200 OK", "text/html; charset=utf-8", NULL);
			send(fd, body, len, 0);
		} else {
			reply(fd, "404 Not Found", "text/plain", "not found\n");
		}
		free(body);
		close(fd);
		return;
	}
	if (strcmp(path, h->path) != 0) {
		reply(fd, "404 Not Found", "text/plain", "not found\n");
		close(fd);
		return;
	}

	add_client(h, fd, peer, path);
}

static void add_client(struct http *h, int fd, const char *peer, const char *path)
{
	struct http_client *c = calloc(1, sizeof(*c));
	pthread_t t;
	int one = 1;

	if (c)
		c->ring = malloc(RING_SIZE);
	if (!c || !c->ring) {
		free(c);
		close(fd);
		return;
	}
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	reply(fd, "200 OK", "video/mp2t", NULL);
	c->h = h;
	c->fd = fd;
	snprintf(c->addr, sizeof(c->addr), "%s", peer);
	pthread_mutex_init(&c->lock, NULL);
	pthread_cond_init(&c->cond, NULL);
	pthread_mutex_lock(&h->lock);
	c->next = h->clients;
	h->clients = c;
	pthread_mutex_unlock(&h->lock);
	LOG("client %s watching %s\n", peer, path);
	pthread_create(&t, NULL, client_writer, c);
	pthread_detach(t);
}

static void *accept_loop(void *arg)
{
	struct http *h = arg;

	while (h->running) {
		struct sockaddr_in sa;
		socklen_t sl = sizeof(sa);
		int fd = accept(h->listen_fd, (struct sockaddr *)&sa, &sl);
		char peer[64];

		if (fd < 0)
			continue;
		snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
		handle(h, fd, peer);
	}
	return NULL;
}

int http_start(struct http *h, int port, const char *name, const char *path)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port),
				  .sin_addr.s_addr = htonl(INADDR_ANY) };
	int one = 1;

	int (*route)(const char *, FILE *, void *) = h->route;	/* set by the caller */
	void *route_opaque = h->route_opaque;

	memset(h, 0, sizeof(*h));
	h->route = route;
	h->route_opaque = route_opaque;
	h->port = port;
	h->name = name;
	h->path = path;
	pthread_mutex_init(&h->lock, NULL);
	h->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (h->listen_fd < 0)
		return -errno;
	setsockopt(h->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(h->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(h->listen_fd, 8)) {
		LOG("cannot listen on port %d: %s\n", port, strerror(errno));
		close(h->listen_fd);
		return -errno;
	}
	h->running = true;
	return pthread_create(&h->accept_thread, NULL, accept_loop, h) ? -EIO : 0;
}

void http_send(struct http *h, const u8 *buf, int len)
{
	pthread_mutex_lock(&h->lock);
	for (struct http_client **pc = &h->clients; *pc;) {
		struct http_client *c = *pc;

		pthread_mutex_lock(&c->lock);
		if (c->exited) {
			/* writer thread has exited; free it here */
			pthread_mutex_unlock(&c->lock);
			*pc = c->next;
			free(c->ring);
			free(c);
			continue;
		}
		if (c->dead) {
			pthread_mutex_unlock(&c->lock);
			pc = &c->next;
			continue;
		}
		/* whole chunk or nothing, so TS packets stay aligned */
		if (RING_SIZE - 1 - ring_used(c) >= (size_t)len) {
			size_t first = min((size_t)len, RING_SIZE - c->head);

			memcpy(c->ring + c->head, buf, first);
			memcpy(c->ring, buf + first, len - first);
			c->head = (c->head + len) % RING_SIZE;
			pthread_cond_signal(&c->cond);
		}
		pthread_mutex_unlock(&c->lock);
		pc = &c->next;
	}
	pthread_mutex_unlock(&h->lock);
}

int http_clients(struct http *h)
{
	int n = 0;

	pthread_mutex_lock(&h->lock);
	for (struct http_client *c = h->clients; c; c = c->next)
		n += !c->dead;
	pthread_mutex_unlock(&h->lock);
	return n;
}

void http_stop(struct http *h)
{
	h->running = false;
	shutdown(h->listen_fd, SHUT_RDWR);
	close(h->listen_fd);
	pthread_join(h->accept_thread, NULL);
	pthread_mutex_lock(&h->lock);
	for (struct http_client *c = h->clients; c; c = c->next) {
		pthread_mutex_lock(&c->lock);
		c->dead = true;
		shutdown(c->fd, SHUT_RDWR);
		pthread_cond_signal(&c->cond);
		pthread_mutex_unlock(&c->lock);
	}
	pthread_mutex_unlock(&h->lock);
}
