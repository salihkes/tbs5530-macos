// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <stdlib.h>
#include <string.h>
#include "teletext.h"

/* EN 300 472 4.4: data unit bytes are sent bit-reversed (0xE4 <-> 0x27) */
static u8 rev8(u8 b)
{
	b = (b & 0xf0) >> 4 | (b & 0x0f) << 4;
	b = (b & 0xcc) >> 2 | (b & 0x33) << 2;
	return (b & 0xaa) >> 1 | (b & 0x55) << 1;
}

/* EN 300 706 8.2: the 16 Hamming 8/4 codewords, index = 4 data bits */
static const u8 ham84_code[16] = {
	0x15, 0x02, 0x49, 0x5e, 0x64, 0x73, 0x38, 0x2f,
	0xd0, 0xc7, 0x8c, 0x9b, 0xa1, 0xb6, 0xfd, 0xea,
};
static s8 ham84_tab[256];	/* byte -> nibble, -1 if two or more bit errors */

static void ham84_init(void)
{
	for (int b = 0; b < 256; b++) {
		ham84_tab[b] = -1;
		/* distance 0 = clean, 1 = one bit flipped and corrected */
		for (int n = 0; n < 16; n++)
			if (__builtin_popcount(b ^ ham84_code[n]) <= 1)
				ham84_tab[b] = n;
	}
}

static int ham84(u8 b, struct ttx *t)
{
	int n = ham84_tab[b];

	if (n < 0)
		t->bad_ham++;
	return n;
}

/* text bytes: 7 data bits + odd parity in bit 7 */
static u8 parity7(u8 b)
{
	return __builtin_popcount(b) & 1 ? b & 0x7f : ' ';
}

void ttx_init(struct ttx *t, int pid)
{
	static bool tab_done;

	if (!tab_done) {
		ham84_init();
		tab_done = true;
	}
	memset(t, 0, sizeof(*t));
	t->pid = pid;
	t->turkish = false;
	pthread_mutex_init(&t->lock, NULL);
	for (int m = 0; m < 8; m++)
		t->cur_page[m] = -1;
}

void ttx_free(struct ttx *t)
{
	for (int i = 0; i < 0x800; i++)
		free(t->pages[i]);
}

static bool page_is_decimal(int idx)
{
	return (idx & 0x0f) <= 9 && ((idx >> 4) & 0x0f) <= 9;
}

static int page_number(int idx)
{
	int mag = idx >> 8;

	return (mag ? mag : 8) * 100 + ((idx >> 4) & 0x0f) * 10 + (idx & 0x0f);
}

static int page_index(int number)
{
	int mag = number / 100, tens = number / 10 % 10, units = number % 10;

	if (mag < 1 || mag > 8)
		return -1;
	return (mag & 7) << 8 | tens << 4 | units;
}

/* move the page magazine m was receiving into the store */
static void commit(struct ttx *t, int m)
{
	int idx = t->cur_page[m];
	struct ttx_entry *e;
	int slot;

	if (idx < 0)
		return;
	t->cur_page[m] = -1;
	e = t->pages[idx];
	if (!e) {
		e = calloc(1, sizeof(*e));
		if (!e)
			return;
		t->pages[idx] = e;
	}
	for (slot = 0; slot < e->n; slot++)
		if (e->sub[slot].sub == t->cur[m].sub)
			break;
	if (slot == e->n) {
		if (e->n < TTX_MAXSUB) {
			e->n++;
		} else {
			slot = e->next;
			e->next = (e->next + 1) % TTX_MAXSUB;
		}
	}
	e->sub[slot] = t->cur[m];
}

/* one 42-byte teletext packet, already bit-reversed */
static void ttx_line(struct ttx *t, const u8 *b)
{
	int a = ham84(b[0], t), c = ham84(b[1], t);
	int m, row;

	if (a < 0 || c < 0)
		return;
	m = a & 7;			/* magazine 0 means 8 */
	row = a >> 3 | c << 1;
	t->lines++;

	if (row == 0) {
		int units = ham84(b[2], t), tens = ham84(b[3], t);
		int s1 = ham84(b[4], t), s2 = ham84(b[5], t), s3 = ham84(b[6], t), s4 = ham84(b[7], t);
		int c7 = ham84(b[8], t), c11 = ham84(b[9], t);
		struct ttx_page *p = &t->cur[m];
		int idx;

		if (units < 0 || tens < 0 || s1 < 0 || s2 < 0 || s3 < 0 || s4 < 0 || c7 < 0 || c11 < 0)
			return;
		/* a header ends the previous page of this magazine; in serial mode (C11) of all */
		if (c11 & 1) {
			for (int k = 0; k < 8; k++)
				commit(t, k);
		} else {
			commit(t, m);
		}
		idx = m << 8 | tens << 4 | units;
		if (!page_is_decimal(idx))
			return;		/* e.g. page FF: time filling, no real page */

		memset(p, 0, sizeof(*p));
		memset(p->text, ' ', sizeof(p->text));
		p->sub = (s4 & 3) << 12 | s3 << 8 | (s2 & 7) << 4 | s1;
		p->natopt = (c11 >> 1 & 1) << 2 | (c11 >> 2 & 1) << 1 | (c11 >> 3 & 1);
		p->updated = time(NULL);
		/* header text goes in columns 8..39; 0..7 show the page number */
		for (int i = 10; i < 42; i++)
			p->text[0][i - 2] = parity7(b[i]);
		t->cur_page[m] = idx;
		return;
	}
	if (row <= 24 && t->cur_page[m] >= 0) {
		for (int i = 0; i < 40; i++)
			t->cur[m].text[row][i] = parity7(b[i + 2]);
	}
	/* rows 25..31 (enhancements, links, 8/30 broadcast data) are ignored */
}

/* EN 300 472 4.3: PES with data_identifier 0x10..0x1f, then 46-byte data units */
static void ttx_pes(struct ttx *t, const u8 *p, int len)
{
	int hdr, end, i;

	if (len < 9 || p[0] || p[1] || p[2] != 1 || p[3] != 0xbd)
		return;
	end = 6 + (p[4] << 8 | p[5]);
	if (end > len || end == 6)
		end = len;
	hdr = 9 + p[8];
	if (hdr >= end || p[hdr] < 0x10 || p[hdr] > 0x1f)
		return;
	for (i = hdr + 1; i + 2 <= end;) {
		int id = p[i], ulen = p[i + 1];

		if (i + 2 + ulen > end)
			break;
		/* 0x02 teletext, 0x03 teletext subtitles; 44 bytes: line, framing, 42 data */
		if ((id == 0x02 || id == 0x03) && ulen == 0x2c && p[i + 3] == 0xe4) {
			u8 line[42];

			for (int k = 0; k < 42; k++)
				line[k] = rev8(p[i + 4 + k]);
			ttx_line(t, line);
		}
		i += 2 + ulen;
	}
}

void ttx_packet(struct ttx *t, const u8 *pkt)
{
	int off = 4;

	if (((pkt[1] & 0x1f) << 8 | pkt[2]) != t->pid || !(pkt[3] & 0x10))
		return;
	if (pkt[3] & 0x20)
		off += 1 + pkt[4];
	if (off >= 188)
		return;

	pthread_mutex_lock(&t->lock);
	if (pkt[1] & 0x40) {		/* payload_unit_start: a new PES begins */
		if (t->pes_active)
			ttx_pes(t, t->pes, t->pes_len);
		t->pes_len = 0;
		t->pes_active = true;
	}
	if (t->pes_active && t->pes_len + 188 - off <= (int)sizeof(t->pes)) {
		memcpy(t->pes + t->pes_len, pkt + off, 188 - off);
		t->pes_len += 188 - off;
	}
	pthread_mutex_unlock(&t->lock);
}

void ttx_flush(struct ttx *t)
{
	pthread_mutex_lock(&t->lock);
	if (t->pes_active)
		ttx_pes(t, t->pes, t->pes_len);
	t->pes_active = false;
	for (int m = 0; m < 8; m++)
		commit(t, m);
	pthread_mutex_unlock(&t->lock);
}

/* positions a national option sub-set replaces (EN 300 706 table 36) */
static const u8 nos_pos[13] = { 0x23, 0x24, 0x40, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x7b, 0x7c, 0x7d, 0x7e };

/* default Latin group, indexed by C12 C13 C14; Turkish replaces 110 when asked */
static const char *const nos[8][13] = {
	{ "£", "$", "@", "←", "½", "→", "↑", "#", "―", "¼", "‖", "¾", "÷" },	/* English */
	{ "#", "$", "§", "Ä", "Ö", "Ü", "^", "_", "°", "ä", "ö", "ü", "ß" },	/* German */
	{ "#", "¤", "É", "Ä", "Ö", "Å", "Ü", "_", "é", "ä", "ö", "å", "ü" },	/* Swedish/Finnish/Hungarian */
	{ "£", "$", "é", "°", "ç", "→", "↑", "#", "ù", "à", "ò", "è", "ì" },	/* Italian */
	{ "é", "ï", "à", "ë", "ê", "ù", "î", "#", "è", "â", "ô", "û", "ç" },	/* French */
	{ "ç", "$", "¡", "á", "é", "í", "ó", "ú", "¿", "ü", "ñ", "è", "à" },	/* Portuguese/Spanish */
	{ "#", "ů", "č", "ť", "ž", "ý", "í", "ř", "é", "á", "ě", "ú", "š" },	/* Czech/Slovak */
	{ "£", "$", "@", "←", "½", "→", "↑", "#", "―", "¼", "‖", "¾", "÷" },	/* (unused) */
};
static const char *const nos_turkish[13] = {
	"₺", "ğ", "İ", "Ş", "Ö", "Ç", "Ü", "Ğ", "ı", "ş", "ö", "ç", "ü",
};

/* UTF-8 for one G0 character */
static const char *glyph(const struct ttx *t, const struct ttx_page *p, u8 c, char buf[8])
{
	for (int i = 0; i < 13; i++)
		if (c == nos_pos[i])
			return p->natopt == 6 && t->turkish ? nos_turkish[i] : nos[p->natopt][i];
	if (c == 0x7f)
		return "■";
	buf[0] = c;
	buf[1] = 0;
	return buf;
}

/* 2x3 block mosaic -> Unicode "sextant" (U+1FB00 block, plus ▌ ▐ █) */
static const char *mosaic(u8 c, char buf[8])
{
	int bits = (c & 0x1f) | (c & 0x40) >> 1;
	unsigned int cp;

	if (bits == 0)
		return " ";
	if (bits == 0x3f)
		return "█";
	if (bits == 0x15)
		return "▌";
	if (bits == 0x2a)
		return "▐";
	cp = 0x1fb00 + bits - 1 - (bits > 0x15) - (bits > 0x2a);
	buf[0] = 0xf0 | cp >> 18;
	buf[1] = 0x80 | (cp >> 12 & 0x3f);
	buf[2] = 0x80 | (cp >> 6 & 0x3f);
	buf[3] = 0x80 | (cp & 0x3f);
	buf[4] = 0;
	return buf;
}

/*
 * Walk one row the way a TV does: control codes (< 0x20) show as a space
 * and switch colour / graphics mode for what follows ("set-after"), except
 * background changes which apply at once ("set-at").
 */
struct cell {
	const char *s;
	int fg, bg;
};

static void render_row(const struct ttx *t, const struct ttx_page *p, int row,
		       struct cell out[TTX_COLS], char bufs[TTX_COLS][8])
{
	int fg = 7, bg = 0;
	bool gfx = false;

	for (int col = 0; col < TTX_COLS; col++) {
		u8 c = p->text[row][col];

		if (c < 0x20) {
			if (c == 0x1c)
				bg = 0;			/* black background */
			else if (c == 0x1d)
				bg = fg;		/* new background */
			out[col] = (struct cell){ " ", fg, bg };
			if (c <= 0x07) {
				fg = c;			/* alphanumeric colour */
				gfx = false;
			} else if (c >= 0x10 && c <= 0x17) {
				fg = c - 0x10;		/* mosaic colour */
				gfx = true;
			}
			continue;
		}
		if (gfx && (c < 0x40 || c >= 0x60))
			out[col] = (struct cell){ mosaic(c, bufs[col]), fg, bg };
		else
			out[col] = (struct cell){ glyph(t, p, c, bufs[col]), fg, bg };
	}
}

static void collect(struct ttx *t, int list[0x800], int *n)
{
	*n = 0;
	/* magazine 1..7 then 8 (index 0), each 00..99 */
	for (int mag = 1; mag <= 8; mag++)
		for (int pg = 0; pg < 0x100; pg++) {
			int idx = (mag & 7) << 8 | pg;

			if (t->pages[idx] && page_is_decimal(idx))
				list[(*n)++] = idx;
		}
}

int ttx_count(struct ttx *t)
{
	int list[0x800], n;

	pthread_mutex_lock(&t->lock);
	collect(t, list, &n);
	pthread_mutex_unlock(&t->lock);
	return n;
}

void ttx_print_index(struct ttx *t, FILE *out)
{
	int list[0x800], n;

	pthread_mutex_lock(&t->lock);
	collect(t, list, &n);
	fprintf(out, "%d pages, %llu packets, %llu Hamming errors\n", n,
		(unsigned long long)t->lines, (unsigned long long)t->bad_ham);
	for (int i = 0; i < n; i++)
		fprintf(out, "%d%s%s", page_number(list[i]),
			t->pages[list[i]]->n > 1 ? "*" : "", i % 16 == 15 ? "\n" : " ");
	fprintf(out, "\n");
	pthread_mutex_unlock(&t->lock);
}

static int sub_cmp(const void *a, const void *b)
{
	return (int)((const struct ttx_page *)a)->sub - (int)((const struct ttx_page *)b)->sub;
}

/* copy of an entry with subpages in order, so we can render without the lock */
static int snapshot(struct ttx *t, int number, struct ttx_entry *copy)
{
	int idx = page_index(number);

	pthread_mutex_lock(&t->lock);
	if (idx < 0 || !t->pages[idx]) {
		pthread_mutex_unlock(&t->lock);
		return -1;
	}
	*copy = *t->pages[idx];
	pthread_mutex_unlock(&t->lock);
	qsort(copy->sub, copy->n, sizeof(copy->sub[0]), sub_cmp);
	return 0;
}

int ttx_print_text(struct ttx *t, int number, FILE *out)
{
	static struct ttx_entry e;	/* ~32 KB */

	if (snapshot(t, number, &e))
		return -1;
	for (int s = 0; s < e.n; s++) {
		struct ttx_page *p = &e.sub[s];

		fprintf(out, "---- page %d, subpage %04x, national option %d ----\n", number, p->sub,
			p->natopt);
		for (int row = 0; row < TTX_ROWS; row++) {
			struct cell cells[TTX_COLS];
			char bufs[TTX_COLS][8];

			render_row(t, p, row, cells, bufs);
			if (row == 0)
				fprintf(out, "P%d    ", number);
			for (int col = row ? 0 : 8; col < TTX_COLS; col++)
				fputs(cells[col].s, out);
			fputc('\n', out);
		}
	}
	return 0;
}

static const char *const colours[8] = {
	"#000", "#f00", "#0f0", "#ff0", "#00f", "#f0f", "#0ff", "#fff",
};

static void html_head(FILE *out, const char *title, int refresh)
{
	fprintf(out,
		"<!doctype html><html><head><meta charset=utf-8>"
		"<meta name=viewport content='width=device-width'>"
		"<meta http-equiv=refresh content=%d><title>%s</title><style>"
		"body{background:#000;color:#fff;font-family:Menlo,monospace;margin:16px}"
		"a{color:#0ff}pre{font-size:clamp(10px,2.6vw,20px);line-height:1.15;margin:0 0 1.5em}"
		"nav{margin-bottom:1em}input{width:4em;font:inherit}"
		"</style></head><body>",
		refresh, title);
}

void ttx_html_index(struct ttx *t, FILE *out)
{
	int list[0x800], n, last_hundred = -1;

	pthread_mutex_lock(&t->lock);
	collect(t, list, &n);
	html_head(out, "Teletext", 30);
	fprintf(out, "<h1>Teletext</h1><p>%d pages received (refreshes every 30 s)</p>"
		     "<form action=/teletext>Page <input name=p value=100></form>", n);
	for (int i = 0; i < n; i++) {
		int num = page_number(list[i]);

		if (num / 100 != last_hundred) {
			fprintf(out, "%s<p>", last_hundred < 0 ? "" : "</p>");
			last_hundred = num / 100;
		}
		fprintf(out, "<a href=/teletext/%d>%d</a>%s ", num, num,
			t->pages[list[i]]->n > 1 ? "*" : "");
	}
	pthread_mutex_unlock(&t->lock);
	fprintf(out, "</p></body></html>\n");
}

int ttx_html_page(struct ttx *t, int number, FILE *out)
{
	static struct ttx_entry e;
	char title[32];

	if (snapshot(t, number, &e))
		return -1;
	snprintf(title, sizeof(title), "Teletext %d", number);
	html_head(out, title, 15);
	fprintf(out, "<nav><a href=/teletext/%d>&larr; %d</a> &middot; <a href=/teletext>index</a>"
		     " &middot; <a href=/teletext/%d>%d &rarr;</a></nav>",
		number - 1, number - 1, number + 1, number + 1);
	for (int s = 0; s < e.n; s++) {
		struct ttx_page *p = &e.sub[s];

		fprintf(out, "<pre>");
		for (int row = 0; row < TTX_ROWS; row++) {
			struct cell cells[TTX_COLS];
			char bufs[TTX_COLS][8];

			render_row(t, p, row, cells, bufs);
			if (row == 0)
				fprintf(out, "<span style=color:#fff>P%-7d</span>", number);
			for (int col = row ? 0 : 8; col < TTX_COLS; col++) {
				const char *c = cells[col].s;

				fprintf(out, "<span style='color:%s;background:%s'>", colours[cells[col].fg],
					colours[cells[col].bg]);
				if (c[0] == '<')
					fputs("&lt;", out);
				else if (c[0] == '&')
					fputs("&amp;", out);
				else
					fputs(c, out);
				fputs("</span>", out);
			}
			fputc('\n', out);
		}
		fprintf(out, "</pre>");
	}
	fprintf(out, "</body></html>\n");
	return 0;
}
