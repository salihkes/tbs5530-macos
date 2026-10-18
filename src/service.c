// SPDX-License-Identifier: GPL-2.0
/*
 * Tables: ISO/IEC 13818-1 2.4.4 and ETSI EN 300 468.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <string.h>
#include "service.h"

/* CRC-32/MPEG-2: poly 0x04C11DB7, init all ones, MSB first, no final xor */
u32 crc32_mpeg2(const u8 *buf, int len)
{
	u32 crc = 0xffffffff;

	for (int i = 0; i < len; i++) {
		crc ^= (u32)buf[i] << 24;
		for (int b = 0; b < 8; b++)
			crc = crc & 0x80000000 ? crc << 1 ^ 0x04c11db7 : crc << 1;
	}
	return crc;
}

void service_init(struct service_filter *sf, u16 sid, ts_out_cb out, void *opaque)
{
	memset(sf, 0, sizeof(*sf));
	sf->sid = sid;
	sf->pmt_pid = -1;
	sf->pmt_version = -1;
	sf->ttx_pid = -1;
	sf->out = out;
	sf->opaque = opaque;
	/* SDT (names), EIT (programme guide), TDT/TOT (clock) are shared tables */
	sf->pass[0x11] = sf->pass[0x12] = sf->pass[0x14] = true;
}

/*
 * Start of the section in a packet with payload_unit_start set, or NULL.
 * *avail = bytes of the section inside this packet.
 */
static const u8 *section_start(const u8 *p, int *avail)
{
	int off = 4;

	if (!(p[1] & 0x40) || !(p[3] & 0x10))
		return NULL;
	if (p[3] & 0x20)		/* adaptation field */
		off += 1 + p[4];
	if (off >= TS_PACKET)
		return NULL;
	off += 1 + p[off];		/* pointer_field */
	if (off >= TS_PACKET)
		return NULL;
	*avail = TS_PACKET - off;
	return p + off;
}

static void build_pat(struct service_filter *sf, const u8 *orig)
{
	u8 *p = sf->pat, *s = p + 5;
	u32 crc;

	memset(p, 0xff, TS_PACKET);
	p[0] = TS_SYNC;
	p[1] = 0x40;		/* payload_unit_start, PID 0 */
	p[2] = 0x00;
	p[3] = 0x10;		/* payload only; CC filled in on send */
	p[4] = 0x00;		/* pointer_field */

	s[0] = 0x00;		/* table_id: PAT */
	s[1] = 0xb0;		/* section_syntax 1, reserved, length hi = 0 */
	s[2] = 13;		/* 5 header + 4 per program + 4 CRC */
	s[3] = orig[3];		/* transport_stream_id */
	s[4] = orig[4];
	s[5] = orig[5];		/* version, current_next */
	s[6] = 0;		/* section_number */
	s[7] = 0;		/* last_section_number */
	s[8] = sf->sid >> 8;
	s[9] = sf->sid & 0xff;
	s[10] = 0xe0 | sf->pmt_pid >> 8;
	s[11] = sf->pmt_pid & 0xff;
	crc = crc32_mpeg2(s, 12);
	s[12] = crc >> 24;
	s[13] = crc >> 16;
	s[14] = crc >> 8;
	s[15] = crc;
	sf->have_pat = true;
}

static void parse_pat(struct service_filter *sf, const u8 *p)
{
	int avail, len;
	const u8 *s = section_start(p, &avail);

	if (!s || s[0] != 0x00 || avail < 12)
		return;
	len = (s[1] & 0x0f) << 8 | s[2];
	if (3 + len > avail)
		return;			/* PAT longer than one packet: not handled */
	for (int i = 8; i + 4 <= 3 + len - 4; i += 4) {
		int prog = s[i] << 8 | s[i + 1];
		int pid = (s[i + 2] & 0x1f) << 8 | s[i + 3];

		if (prog == sf->sid) {
			if (pid != sf->pmt_pid) {
				LOG("PAT: program %d -> PMT PID %d\n", prog, pid);
				if (sf->pmt_pid >= 0)
					sf->pass[sf->pmt_pid] = false;
				sf->pmt_pid = pid;
				sf->pass[pid] = true;
				sf->pmt_version = -1;
			}
			build_pat(sf, s);
			return;
		}
	}
}

static const char *stream_type_name(int t)
{
	switch (t) {
	case 0x01: return "MPEG-1 video";
	case 0x02: return "MPEG-2 video";
	case 0x03: return "MPEG-1 audio";
	case 0x04: return "MPEG-2 audio";
	case 0x06: return "private (teletext/subtitles/AC-3)";
	case 0x0f: return "AAC audio";
	case 0x1b: return "H.264 video";
	case 0x24: return "HEVC video";
	default: return "other";
	}
}

static void parse_pmt(struct service_filter *sf, const u8 *p)
{
	int avail, len, version, pcr, pil, end;
	const u8 *s = section_start(p, &avail);

	if (!s || s[0] != 0x02 || avail < 16)
		return;
	if ((s[3] << 8 | s[4]) != sf->sid)
		return;
	version = (s[5] >> 1) & 0x1f;
	if (version == sf->pmt_version)
		return;
	len = (s[1] & 0x0f) << 8 | s[2];
	end = min(3 + len - 4, avail);	/* stop before the CRC */
	pcr = (s[8] & 0x1f) << 8 | s[9];
	pil = (s[10] & 0x0f) << 8 | s[11];

	sf->pmt_version = version;
	sf->pass[pcr] = true;
	LOG("PMT v%d: PCR PID %d\n", version, pcr);
	for (int i = 12 + pil; i + 5 <= end;) {
		int type = s[i];
		int pid = (s[i + 1] & 0x1f) << 8 | s[i + 2];
		int esl = (s[i + 3] & 0x0f) << 8 | s[i + 4];

		sf->pass[pid] = true;
		LOG("  PID %4d  type 0x%02x  %s", pid, type, stream_type_name(type));
		/* ES descriptors: 0x56 teletext, 0x59 DVB subtitles (may repeat) */
		const char *what = "";

		for (int d = i + 5; d + 2 <= i + 5 + esl && d + 2 <= end; d += 2 + s[d + 1]) {
			if (s[d] == 0x56) {
				sf->ttx_pid = pid;
				what = " = teletext";
			} else if (s[d] == 0x59) {
				what = " = DVB subtitles";
			}
		}
		LOG("%s\n", what);
		i += 5 + esl;
	}
}

void service_packet(const u8 *p, void *opaque)
{
	struct service_filter *sf = opaque;
	int pid = ts_pid(p);

	if (pid == 0) {
		parse_pat(sf, p);
		if (sf->have_pat && (p[1] & 0x40)) {
			/* send our PAT in place of each original one */
			sf->pat[3] = 0x10 | (sf->pat_cc++ & 0x0f);
			sf->out(sf->pat, sf->opaque);
		}
		return;
	}
	if (pid == sf->pmt_pid)
		parse_pmt(sf, p);
	if (sf->pass[pid])
		sf->out(p, sf->opaque);
}
