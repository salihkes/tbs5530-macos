// SPDX-License-Identifier: GPL-2.0
/*
 * tbs5530: user-space driver for the TBS5530 USB tuner on macOS.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include "sat.h"
#include "stream.h"
#include "ts.h"
#include "service.h"
#include "http.h"
#include "teletext.h"

#define XFERS		32
#define XFER_SIZE	16384
#define LOCK_TIMEOUT_MS	5000

int g_verbose;

static struct sat_tune tp = { .port = -1, .dvbs2 = true };
static int opt_sid = -1, opt_http_port = 5530, opt_ttx_pid = -1;
static const char *opt_name;
static bool opt_vlc, opt_ttx_turkish;
static atomic_bool stop;

void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

void hexdump(const char *prefix, const u8 *buf, int len)
{
	fprintf(stderr, "%s", prefix);
	for (int i = 0; i < len; i++)
		fprintf(stderr, " %02x", buf[i]);
	fprintf(stderr, "\n");
}

static void on_signal(int sig)
{
	stop = true;
}

static int cmd_wake(struct fx2 *d)
{
	char path[1024];

	if (fx2_is_warm(d)) {
		LOG("already warm\n");
		return 0;
	}
	snprintf(path, sizeof(path), "%s/dvb-usb-id5530.fw", g_fw_dir);
	if (fx2_load_firmware(d, path))
		return 1;
	LOG("warm\n");
	return 0;
}

static int cmd_eeprom(struct fx2 *d)
{
	u8 e[256];

	if (fx2_read_eeprom(d, e)) {
		LOG("EEPROM read failed (run 'wake' first?)\n");
		return 1;
	}
	for (int i = 0; i < 256; i += 16) {
		printf("%02x:", i);
		for (int j = 0; j < 16; j++)
			printf(" %02x", e[i + j]);
		printf("\n");
	}
	return 0;
}

static int open_sat(struct tbs5530 *t, bool tune)
{
	if (tbs5530_open(t) || tbs5530_sat_attach(t))
		return -ENODEV;
	if (tune && sat_tune(t, &tp, LOCK_TIMEOUT_MS)) {
		LOG("no lock\n");
		return -ETIMEDOUT;
	}
	return 0;
}

static int cmd_probe(void)
{
	struct tbs5530 t;
	int ret = open_sat(&t, false);

	if (!ret)
		LOG("satellite frontend ready: %s\n", t.sat->ops.info.name);
	tbs5530_close(&t);
	return ret ? 1 : 0;
}

static int cmd_tune(void)
{
	struct tbs5530 t;
	int ret = open_sat(&t, true);

	if (t.sat)
		sat_print_status(&t);
	tbs5530_close(&t);
	return ret ? 1 : 0;
}

struct rec {
	FILE *f;
	struct ts_stats st;
};

static void rec_data(const u8 *buf, int len, void *opaque)
{
	struct rec *r = opaque;

	fwrite(buf, 1, len, r->f);
	ts_feed(&r->st, buf, len, NULL, NULL);
}

static int cmd_record(int seconds, const char *path)
{
	static struct rec r;
	struct tbs5530 t;
	struct ts_stream s;
	uint64_t t0;
	int ret = 1;

	ts_stats_init(&r.st);
	if (open_sat(&t, true))
		goto out;
	r.f = fopen(path, "wb");
	if (!r.f) {
		perror(path);
		goto out;
	}
	tbs5530_sat_ts_select(&t);
	if (ts_stream_start(&s, &t.usb, XFERS, XFER_SIZE, rec_data, &r))
		goto out;
	t0 = now_ms();
	for (int i = 0; i < seconds; i++) {
		msleep(1000);
		LOG("\r%d s, %.1f MB, %d USB errors", i + 1, s.bytes / 1e6, (int)s.errors);
	}
	ts_stream_stop(&s);
	LOG("\n");
	ts_stats_print(&r.st, (now_ms() - t0) / 1000.0);
	ret = 0;
out:
	if (r.f)
		fclose(r.f);
	tbs5530_close(&t);
	return ret;
}

struct srv {
	struct ts_stats st;
	struct service_filter sf;
	struct http http;
	struct ttx ttx;
	u8 batch[TS_PACKET * 64];
	int n;
	u64 sent;
};

static void srv_flush(struct srv *s)
{
	if (s->n) {
		http_send(&s->http, s->batch, s->n);
		s->sent += s->n;
		s->n = 0;
	}
}

static void srv_packet(const u8 *p, void *opaque)
{
	struct srv *s = opaque;

	if (ts_pid(p) == s->sf.ttx_pid) {
		s->ttx.pid = s->sf.ttx_pid;
		ttx_packet(&s->ttx, p);
	}
	memcpy(s->batch + s->n, p, TS_PACKET);
	s->n += TS_PACKET;
	if (s->n == sizeof(s->batch))
		srv_flush(s);
}

static void srv_data(const u8 *buf, int len, void *opaque)
{
	struct srv *s = opaque;

	ts_feed(&s->st, buf, len, service_packet, &s->sf);
	srv_flush(s);
}

/* /teletext, /teletext?p=NNN, /teletext/NNN */
static int srv_route(const char *path, FILE *out, void *opaque)
{
	struct srv *s = opaque;
	const char *q;

	if (strncmp(path, "/teletext", 9))
		return -1;
	q = strstr(path, "?p=");
	if (q && atoi(q + 3))
		return ttx_html_page(&s->ttx, atoi(q + 3), out);
	if (path[9] == '/' && atoi(path + 10))
		return ttx_html_page(&s->ttx, atoi(path + 10), out);
	ttx_html_index(&s->ttx, out);
	return 0;
}

static int cmd_serve(void)
{
	static struct srv s;
	char url[128], name[32];
	struct tbs5530 t;
	struct ts_stream ts;
	int ret = 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	if (!opt_name) {
		snprintf(name, sizeof(name), "SID %d", opt_sid);
		opt_name = name;
	}
	ts_stats_init(&s.st);
	service_init(&s.sf, opt_sid, srv_packet, &s);
	ttx_init(&s.ttx, -1);
	s.ttx.turkish = opt_ttx_turkish;
	s.http.route = srv_route;
	s.http.route_opaque = &s;
	if (http_start(&s.http, opt_http_port, opt_name, "/stream.ts"))
		return 1;

	if (open_sat(&t, true))
		goto out;
	sat_print_status(&t);
	tbs5530_sat_ts_select(&t);
	if (ts_stream_start(&ts, &t.usb, XFERS, XFER_SIZE, srv_data, &s))
		goto out;

	snprintf(url, sizeof(url), "http://127.0.0.1:%d/stream.ts", opt_http_port);
	LOG("\n%s on %s\n"
	    "  playlist http://<this-host>:%d/playlist.m3u\n"
	    "  teletext http://<this-host>:%d/teletext\n"
	    "Ctrl-C to stop\n\n", opt_name, url, opt_http_port, opt_http_port);
	if (opt_vlc) {
		char cmd[256];

		snprintf(cmd, sizeof(cmd), "open -a VLC '%s'", url);
		if (system(cmd))
			LOG("could not start VLC\n");
	}

	for (int sec = 1; !stop; sec++) {
		msleep(1000);
		if (sec % 5 == 0)
			LOG("\r%d s  in %.1f MB  out %.1f MB  clients %d  CC errors %llu  teletext %d pages ",
			    sec, ts.bytes / 1e6, s.sent / 1e6, http_clients(&s.http),
			    (unsigned long long)s.st.cc_errors, ttx_count(&s.ttx));
	}
	LOG("\nstopping\n");
	ts_stream_stop(&ts);
	ret = 0;
out:
	http_stop(&s.http);
	tbs5530_close(&t);
	return ret;
}

/* first PID carrying a teletext PES (stream_id 0xbd, data_identifier 0x10..0x1f) */
static int find_ttx_pid(const u8 *p)
{
	int off = 4;

	if (!(p[1] & 0x40) || !(p[3] & 0x10))
		return -1;
	if (p[3] & 0x20)
		off += 1 + p[4];
	if (off + 10 > TS_PACKET || p[off] || p[off + 1] || p[off + 2] != 1 || p[off + 3] != 0xbd)
		return -1;
	off += 9 + p[off + 8];
	if (off >= TS_PACKET || p[off] < 0x10 || p[off] > 0x1f)
		return -1;
	return ts_pid(p);
}

static void ttx_file_packet(const u8 *p, void *opaque)
{
	struct ttx *t = opaque;

	if (t->pid < 0) {
		t->pid = find_ttx_pid(p);
		if (t->pid >= 0)
			LOG("teletext on PID %d\n", t->pid);
	}
	ttx_packet(t, p);
}

static int cmd_ttx(const char *path, int page)
{
	static struct ttx t;
	static struct ts_stats st;
	static u8 buf[1 << 16];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) {
		perror(path);
		return 1;
	}
	ttx_init(&t, opt_ttx_pid);
	t.turkish = opt_ttx_turkish;
	ts_stats_init(&st);
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		ts_feed(&st, buf, n, ttx_file_packet, &t);
	fclose(f);
	if (t.pid < 0) {
		LOG("no teletext found\n");
		return 1;
	}
	ttx_flush(&t);
	ttx_print_index(&t, stdout);
	if (ttx_print_text(&t, page, stdout))
		printf("page %d not in this recording\n", page);
	ttx_free(&t);
	return 0;
}

static __noreturn void usage(void)
{
	fprintf(stderr,
		"usage: tbs5530 [options] COMMAND\n"
		"\n"
		"  wake | eeprom | usbinfo | probe\n"
		"  tune   FREQ_MHZ H|V SYMBOL_RATE_KS\n"
		"  record FREQ_MHZ H|V SYMBOL_RATE_KS SECONDS FILE.ts\n"
		"  serve  FREQ_MHZ H|V SYMBOL_RATE_KS --sid N\n"
		"  ttx    FILE.ts [PAGE]\n"
		"\n"
		"  --port A|B|C|D      DiSEqC 1.0 switch port (default: no switch)\n"
		"  --dvbs              DVB-S instead of DVB-S2\n"
		"  --sid N             service (program number) for serve\n"
		"  --name STR          name in the playlist\n"
		"  --http-port N       default 5530\n"
		"  --vlc               open VLC when serving\n"
		"  --pid N             teletext PID for ttx (default: autodetect)\n"
		"  --ttx-turkish       national option 110 is Turkish, not Czech/Slovak\n"
		"  --fw-dir DIR        firmware directory (default: ./firmware)\n"
		"  --reload            upload the FX2 firmware even if it is running\n"
		"  -v, -vv, -vvv       more output\n");
	exit(2);
}

static void parse_tuning(const char *const *a)
{
	tp.freq_khz = (u32)(atof(a[0]) * 1000);
	if ((a[1][0] | 0x20) != 'h' && (a[1][0] | 0x20) != 'v')
		usage();
	tp.pol_h = (a[1][0] | 0x20) == 'h';
	tp.symbol_rate = (u32)atoi(a[2]) * 1000;
	if (tp.freq_khz < 10000000 || tp.freq_khz > 13000000 || !tp.symbol_rate)
		usage();
}

int main(int argc, char **argv)
{
	const char *cmd = NULL, *pos[5];
	int npos = 0, ret;
	struct fx2 d;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		bool more = i + 1 < argc;

		if (a[0] == '-' && a[1] == 'v' && strspn(a + 1, "v") == strlen(a + 1)) {
			g_verbose += strlen(a) - 1;
		} else if (!strcmp(a, "--port") && more) {
			tp.port = (argv[++i][0] | 0x20) - 'a';
			if (tp.port < 0 || tp.port > 3)
				usage();
		} else if (!strcmp(a, "--dvbs")) {
			tp.dvbs2 = false;
		} else if (!strcmp(a, "--sid") && more) {
			opt_sid = atoi(argv[++i]);
		} else if (!strcmp(a, "--name") && more) {
			opt_name = argv[++i];
		} else if (!strcmp(a, "--http-port") && more) {
			opt_http_port = atoi(argv[++i]);
		} else if (!strcmp(a, "--vlc")) {
			opt_vlc = true;
		} else if (!strcmp(a, "--pid") && more) {
			opt_ttx_pid = atoi(argv[++i]);
		} else if (!strcmp(a, "--ttx-turkish")) {
			opt_ttx_turkish = true;
		} else if (!strcmp(a, "--fw-dir") && more) {
			g_fw_dir = argv[++i];
		} else if (!strcmp(a, "--reload")) {
			g_fw_reload = true;
		} else if (a[0] == '-') {
			usage();
		} else if (!cmd) {
			cmd = a;
		} else if (npos < (int)ARRAY_SIZE(pos)) {
			pos[npos++] = a;
		} else {
			usage();
		}
	}
	if (!cmd)
		usage();

	if (!strcmp(cmd, "ttx")) {
		if (npos < 1 || npos > 2)
			usage();
		return cmd_ttx(pos[0], npos == 2 ? atoi(pos[1]) : 100);
	}
	if (!strcmp(cmd, "tune") || !strcmp(cmd, "serve") || !strcmp(cmd, "record")) {
		if (npos != (strcmp(cmd, "record") ? 3 : 5))
			usage();
		parse_tuning(pos);
	} else if (npos) {
		usage();
	}
	if (!strcmp(cmd, "probe"))
		return cmd_probe();
	if (!strcmp(cmd, "tune"))
		return cmd_tune();
	if (!strcmp(cmd, "record"))
		return cmd_record(atoi(pos[3]), pos[4]);
	if (!strcmp(cmd, "serve")) {
		if (opt_sid < 0)
			usage();
		return cmd_serve();
	}

	if (fx2_open(&d)) {
		fx2_close(&d);
		return 1;
	}
	if (!strcmp(cmd, "wake")) {
		ret = cmd_wake(&d);
	} else if (!strcmp(cmd, "eeprom")) {
		ret = cmd_eeprom(&d);
	} else if (!strcmp(cmd, "usbinfo")) {
		fx2_print_descriptors(&d);
		ret = 0;
	} else {
		fx2_close(&d);
		usage();
	}
	fx2_close(&d);
	return ret;
}
