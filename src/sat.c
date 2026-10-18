// SPDX-License-Identifier: GPL-2.0
/*
 * Satellite tuning: LNB, DiSEqC 1.0 switch and demod, in dvb-core order.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include "sat.h"

/* E0 10 38 Fx: committed switch, x = port << 2 | horizontal << 1 | high band */
static int diseqc_committed(struct dvb_frontend *fe, int port, bool pol_h, bool hiband)
{
	struct dvb_diseqc_master_cmd cmd = {
		.msg = { 0xe0, 0x10, 0x38,
			 0xf0 | port << 2 | (pol_h ? 2 : 0) | (hiband ? 1 : 0) },
		.msg_len = 4,
	};
	int ret;

	DBG(1, "diseqc: %02x %02x %02x %02x\n", cmd.msg[0], cmd.msg[1], cmd.msg[2], cmd.msg[3]);
	ret = fe->ops.diseqc_send_master_cmd(fe, &cmd);
	msleep(25);
	/* repeat once, framed as a repetition (E1), for slow switches */
	cmd.msg[0] = 0xe1;
	fe->ops.diseqc_send_master_cmd(fe, &cmd);
	msleep(25);
	return ret;
}

int sat_tune(struct tbs5530 *t, const struct sat_tune *p, int timeout_ms)
{
	struct dvb_frontend *fe = t->sat;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	bool hiband = p->freq_khz >= LNB_SLOF;
	u32 lof = hiband ? LNB_LOF_HI : LNB_LOF_LO;
	enum fe_status st = 0;
	uint64_t t0;

	LOG("tuning %u.%03u MHz %c %u kS/s, %s band, IF %u MHz, port %c\n",
	    p->freq_khz / 1000, p->freq_khz % 1000, p->pol_h ? 'H' : 'V',
	    p->symbol_rate / 1000, hiband ? "high" : "low", (p->freq_khz - lof) / 1000,
	    p->port < 0 ? '-' : 'A' + p->port);

	fe->ops.set_voltage(fe, p->pol_h ? SEC_VOLTAGE_18 : SEC_VOLTAGE_13);
	fe->ops.set_tone(fe, SEC_TONE_OFF);
	msleep(100);

	if (p->port >= 0) {
		if (diseqc_committed(fe, p->port, p->pol_h, hiband))
			LOG("warning: DiSEqC command not sent\n");
		fe->ops.diseqc_send_burst(fe, p->port & 1 ? SEC_MINI_B : SEC_MINI_A);
		msleep(25);
	}
	fe->ops.set_tone(fe, hiband ? SEC_TONE_ON : SEC_TONE_OFF);
	msleep(25);

	memset(c, 0, sizeof(*c));
	c->delivery_system = p->dvbs2 ? SYS_DVBS2 : SYS_DVBS;
	c->frequency = p->freq_khz - lof;
	c->symbol_rate = p->symbol_rate;
	c->inversion = INVERSION_AUTO;
	c->stream_id = NO_STREAM_ID_FILTER;
	if (fe->ops.set_frontend(fe)) {
		LOG("set_frontend failed\n");
		return -EIO;
	}

	for (t0 = now_ms(); now_ms() - t0 < (uint64_t)timeout_ms; msleep(100)) {
		fe->ops.read_status(fe, &st);
		if (st & FE_HAS_LOCK) {
			DBG(1, "lock after %llu ms\n", (unsigned long long)(now_ms() - t0));
			return 0;
		}
	}
	return -ETIMEDOUT;
}

static const char *fec_name(enum fe_code_rate f)
{
	static const struct {
		enum fe_code_rate f;
		const char *n;
	} t[] = {
		{ FEC_1_2, "1/2" }, { FEC_2_3, "2/3" }, { FEC_3_4, "3/4" }, { FEC_4_5, "4/5" },
		{ FEC_5_6, "5/6" }, { FEC_7_8, "7/8" }, { FEC_8_9, "8/9" }, { FEC_3_5, "3/5" },
		{ FEC_9_10, "9/10" },
	};

	for (size_t i = 0; i < ARRAY_SIZE(t); i++)
		if (t[i].f == f)
			return t[i].n;
	return "?";
}

void sat_print_status(struct tbs5530 *t)
{
	static const char * const mods[] = {
		[QPSK] = "QPSK", [PSK_8] = "8PSK", [APSK_16] = "16APSK",
		[APSK_32] = "32APSK", [APSK_64] = "64APSK",
	};
	struct dvb_frontend *fe = t->sat;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	struct dtv_frontend_properties info = { 0 };
	enum fe_status st = 0;
	const char *mod;

	fe->ops.read_status(fe, &st);
	fe->ops.get_frontend(fe, &info);

	printf("status  %s%s%s%s%s\n", st & FE_HAS_SIGNAL ? "SIGNAL " : "",
	       st & FE_HAS_CARRIER ? "CARRIER " : "", st & FE_HAS_VITERBI ? "FEC " : "",
	       st & FE_HAS_SYNC ? "SYNC " : "", st & FE_HAS_LOCK ? "LOCK" : "(no lock)");
	if (c->strength.len > 1)
		printf("level   %.1f dBm (%lld%%)\n", c->strength.stat[0].svalue / 1000.0,
		       (long long)c->strength.stat[1].svalue * 100 / 65535);
	if (c->cnr.len > 1 && c->cnr.stat[0].scale == FE_SCALE_DECIBEL)
		printf("snr     %.1f dB\n", c->cnr.stat[0].svalue / 1000.0);
	if (st & FE_HAS_LOCK) {
		mod = info.modulation < ARRAY_SIZE(mods) && mods[info.modulation] ?
		      mods[info.modulation] : "?";
		printf("mode    %s %s, pilot %s\n", mod, fec_name(info.fec_inner),
		       info.pilot == PILOT_ON ? "on" : "off");
	}
}
