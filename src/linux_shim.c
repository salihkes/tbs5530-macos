// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 The tbs5530-macos authors
 */
#include <time.h>
#include <unistd.h>
#include "linux_shim.h"

const char *g_fw_dir = "firmware";
bool g_fw_reload;

uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void usleep_range(unsigned long min_us, unsigned long max_us)
{
	usleep(min_us);
}

void mutex_init(struct mutex *m)
{
	pthread_mutexattr_t a;

	/* error-checking: some m88rs6060 error paths unlock without locking */
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&m->m, &a);
	pthread_mutexattr_destroy(&a);
}

struct regmap *regmap_init_i2c(struct i2c_client *client, const struct regmap_config *cfg)
{
	struct regmap *map = kzalloc(sizeof(*map), GFP_KERNEL);

	if (map)
		map->client = client;
	return map;
}

void regmap_exit(struct regmap *map)
{
	kfree(map);
}

int regmap_bulk_read(struct regmap *map, unsigned int reg, void *val, size_t count)
{
	u8 r = reg;
	struct i2c_msg msg[2] = {
		{ .addr = map->client->addr, .flags = 0, .len = 1, .buf = &r },
		{ .addr = map->client->addr, .flags = I2C_M_RD, .len = count, .buf = val },
	};
	int ret = i2c_transfer(map->client->adapter, msg, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

int regmap_read(struct regmap *map, unsigned int reg, unsigned int *val)
{
	u8 b;
	int ret = regmap_bulk_read(map, reg, &b, 1);

	if (!ret)
		*val = b;
	return ret;
}

int regmap_bulk_write(struct regmap *map, unsigned int reg, const void *val, size_t count)
{
	u8 buf[64];
	struct i2c_msg msg = {
		.addr = map->client->addr, .flags = 0, .len = count + 1, .buf = buf,
	};
	int ret;

	if (count + 1 > sizeof(buf))
		return -EINVAL;
	buf[0] = reg;
	memcpy(buf + 1, val, count);
	ret = i2c_transfer(map->client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

int regmap_write(struct regmap *map, unsigned int reg, unsigned int val)
{
	u8 b = val;

	return regmap_bulk_write(map, reg, &b, 1);
}

int regmap_update_bits(struct regmap *map, unsigned int reg, unsigned int mask,
		       unsigned int val)
{
	unsigned int tmp;
	int ret = regmap_read(map, reg, &tmp);

	if (ret)
		return ret;
	return regmap_write(map, reg, (tmp & ~mask) | (val & mask));
}

int regmap_multi_reg_write(struct regmap *map, const struct reg_sequence *regs, int num)
{
	for (int i = 0; i < num; i++) {
		int ret = regmap_write(map, regs[i].reg, regs[i].def);

		if (ret)
			return ret;
		if (regs[i].delay_us)
			usleep(regs[i].delay_us);
	}
	return 0;
}

int request_firmware(const struct firmware **out, const char *name, struct device *dev)
{
	char path[1024];
	struct firmware *fw;
	FILE *f;
	long n;
	u8 *data;

	snprintf(path, sizeof(path), "%s/%s", g_fw_dir, name);
	f = fopen(path, "rb");
	if (!f)
		return -ENOENT;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	data = malloc(n > 0 ? n : 1);
	if (!fw || !data || fread(data, 1, n, f) != (size_t)n) {
		fclose(f);
		free(data);
		kfree(fw);
		return -EIO;
	}
	fclose(f);
	fw->size = n;
	fw->data = data;
	*out = fw;
	return 0;
}

void release_firmware(const struct firmware *fw)
{
	if (fw) {
		free((void *)fw->data);
		kfree(fw);
	}
}
