/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The part of the Linux kernel API the TBS frontend drivers use, in user
 * space, so the driver sources can stay as they are.
 *
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#ifndef LINUX_SHIM_H
#define LINUX_SHIM_H

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "common.h"

#define GFP_KERNEL 0

static inline void *kzalloc(size_t n, int flags)
{
	return calloc(1, n);
}

static inline void kfree(const void *p)
{
	free((void *)p);
}

#define clamp_val(v, lo, hi) min(max((v), (lo)), (hi))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define strscpy(dst, src, n) strlcpy((dst), (src), (n))
#define MHz 1000000UL

#define IS_ERR(p) ((p) == NULL)
#define PTR_ERR(p) (-ENOMEM)
#define EREMOTEIO 121

/* jiffies are milliseconds here */
#define HZ 1000
#define jiffies now_ms()
#define msecs_to_jiffies(ms) (ms)
#define jiffies_to_msecs(j) ((unsigned int)(j))
#define time_after(a, b) ((int64_t)((b) - (a)) < 0)
void usleep_range(unsigned long min_us, unsigned long max_us);

struct device {
	const char *name;
};

/* dev_info at -v, dev_dbg at -vv */
#define dev_dbg(d, fmt, ...)  DBG(2, "[%s] " fmt "\n", (d)->name, ##__VA_ARGS__)
#define dev_info(d, fmt, ...) DBG(1, "[%s] " fmt, (d)->name, ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) LOG("[%s] " fmt, (d)->name, ##__VA_ARGS__)
#define dev_err(d, fmt, ...)  LOG("[%s] " fmt, (d)->name, ##__VA_ARGS__)

struct mutex {
	pthread_mutex_t m;
};

void mutex_init(struct mutex *m);
#define mutex_lock(x) pthread_mutex_lock(&(x)->m)
#define mutex_unlock(x) pthread_mutex_unlock(&(x)->m)

#define I2C_M_RD 0x0001

struct i2c_msg {
	u16 addr;
	u16 flags;
	u16 len;
	u8 *buf;
};

struct i2c_adapter {
	struct device dev;
	int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	void *priv;
};

/* number of messages transferred, or a negative errno */
static inline int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	return adap->xfer(adap, msgs, num);
}

struct i2c_client {
	struct i2c_adapter *adapter;
	u16 addr;
	void *clientdata;
};

static inline void *i2c_get_clientdata(struct i2c_client *c)
{
	return c->clientdata;
}

static inline void i2c_set_clientdata(struct i2c_client *c, void *d)
{
	c->clientdata = d;
}

/* regmap with 8-bit registers and values on I2C */
struct regmap_config {
	int reg_bits;
	int val_bits;
};

struct reg_sequence {
	unsigned int reg;
	unsigned int def;
	unsigned int delay_us;
};

struct regmap {
	struct i2c_client *client;
};

struct regmap *regmap_init_i2c(struct i2c_client *client, const struct regmap_config *cfg);
void regmap_exit(struct regmap *map);
int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val);
int regmap_write(struct regmap *map, unsigned int reg, unsigned int val);
int regmap_bulk_read(struct regmap *map, unsigned int reg, void *val, size_t count);
int regmap_bulk_write(struct regmap *map, unsigned int reg, const void *val, size_t count);
int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int val);
int regmap_multi_reg_write(struct regmap *map, const struct reg_sequence *regs, int num);

/* request_firmware() reads <g_fw_dir>/<name> */
struct firmware {
	size_t size;
	const u8 *data;
};

extern const char *g_fw_dir;
extern bool g_fw_reload;
int request_firmware(const struct firmware **fw, const char *name, struct device *dev);
void release_firmware(const struct firmware *fw);

/* linux/dvb/frontend.h and media/dvb_frontend.h, satellite subset */
enum fe_status {
	FE_NONE		= 0x00,
	FE_HAS_SIGNAL	= 0x01,
	FE_HAS_CARRIER	= 0x02,
	FE_HAS_VITERBI	= 0x04,
	FE_HAS_SYNC	= 0x08,
	FE_HAS_LOCK	= 0x10,
	FE_TIMEDOUT	= 0x20,
	FE_REINIT	= 0x40,
};

enum fe_sec_voltage { SEC_VOLTAGE_13, SEC_VOLTAGE_18, SEC_VOLTAGE_OFF };
enum fe_sec_tone_mode { SEC_TONE_ON, SEC_TONE_OFF };
enum fe_sec_mini_cmd { SEC_MINI_A, SEC_MINI_B };
enum fe_spectral_inversion { INVERSION_OFF, INVERSION_ON, INVERSION_AUTO };

enum fe_code_rate {
	FEC_NONE = 0, FEC_1_2, FEC_2_3, FEC_3_4, FEC_4_5, FEC_5_6, FEC_6_7, FEC_7_8,
	FEC_8_9, FEC_AUTO, FEC_3_5, FEC_9_10, FEC_2_5, FEC_1_3, FEC_1_4, FEC_5_9,
	FEC_7_9, FEC_8_15, FEC_11_15, FEC_13_18, FEC_9_20, FEC_11_20, FEC_23_36,
	FEC_25_36, FEC_13_45, FEC_26_45, FEC_28_45, FEC_32_45, FEC_77_90, FEC_11_45,
	FEC_4_15, FEC_14_45, FEC_7_15, FEC_29_45, FEC_31_45,
};

enum fe_modulation {
	QPSK, QAM_16, QAM_32, QAM_64, QAM_128, QAM_256, QAM_AUTO, VSB_8, VSB_16,
	PSK_8, APSK_16, APSK_32, DQPSK, QAM_4_NR, APSK_64,
};

enum fe_pilot { PILOT_ON, PILOT_OFF, PILOT_AUTO };

enum fe_rolloff {
	ROLLOFF_35, ROLLOFF_20, ROLLOFF_25, ROLLOFF_AUTO, ROLLOFF_15, ROLLOFF_10, ROLLOFF_5,
};

enum fe_delivery_system {
	SYS_UNDEFINED, SYS_DVBC_ANNEX_A, SYS_DVBC_ANNEX_B, SYS_DVBT, SYS_DSS,
	SYS_DVBS, SYS_DVBS2,
};

enum dvbfe_algo { DVBFE_ALGO_HW = 1, DVBFE_ALGO_SW = 2, DVBFE_ALGO_CUSTOM = 4 };

enum fecap_scale_params {
	FE_SCALE_NOT_AVAILABLE = 0, FE_SCALE_DECIBEL, FE_SCALE_RELATIVE, FE_SCALE_COUNTER,
};

struct dtv_stats {
	u8 scale;
	union {
		u64 uvalue;
		s64 svalue;
	};
};

struct dtv_fe_stats {
	u8 len;
	struct dtv_stats stat[4];
};

#define NO_STREAM_ID_FILTER (~0U)

struct dtv_frontend_properties {
	u32 frequency;		/* satellite: IF in kHz */
	enum fe_modulation modulation;
	enum fe_spectral_inversion inversion;
	enum fe_code_rate fec_inner;
	u32 symbol_rate;
	u32 bandwidth_hz;
	enum fe_pilot pilot;
	enum fe_rolloff rolloff;
	enum fe_delivery_system delivery_system;
	u32 stream_id;
	u32 scrambling_sequence_index;

	struct dtv_fe_stats strength;
	struct dtv_fe_stats cnr;
	struct dtv_fe_stats post_bit_error;
	struct dtv_fe_stats post_bit_count;
};

struct dvb_diseqc_master_cmd {
	u8 msg[6];
	u8 msg_len;
};

struct dvb_frontend;

struct dvb_frontend_ops {
	struct {
		char name[128];
	} info;
	int (*init)(struct dvb_frontend *fe);
	enum dvbfe_algo (*get_frontend_algo)(struct dvb_frontend *fe);
	int (*tune)(struct dvb_frontend *fe, bool re_tune, unsigned int mode_flags,
		    unsigned int *delay, enum fe_status *status);
	int (*set_frontend)(struct dvb_frontend *fe);
	int (*get_frontend)(struct dvb_frontend *fe, struct dtv_frontend_properties *p);
	int (*read_status)(struct dvb_frontend *fe, enum fe_status *status);
	int (*read_ber)(struct dvb_frontend *fe, u32 *ber);
	int (*read_signal_strength)(struct dvb_frontend *fe, u16 *strength);
	int (*read_snr)(struct dvb_frontend *fe, u16 *snr);
	int (*set_voltage)(struct dvb_frontend *fe, enum fe_sec_voltage voltage);
	int (*set_tone)(struct dvb_frontend *fe, enum fe_sec_tone_mode tone);
	int (*diseqc_send_master_cmd)(struct dvb_frontend *fe,
				      struct dvb_diseqc_master_cmd *cmd);
	int (*diseqc_send_burst)(struct dvb_frontend *fe, enum fe_sec_mini_cmd minicmd);
};

struct dvb_frontend {
	struct dvb_frontend_ops ops;
	void *demodulator_priv;
	struct dtv_frontend_properties dtv_property_cache;
};

#endif
