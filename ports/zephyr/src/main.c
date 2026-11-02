/*
 * Zephyr application: firmware scheduler thread, CMIS host interface
 * (I2C target on hardware, a self-test thread on native_sim) and a shell
 * bridge to the firmware's UART console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include "omt/fw.h"

extern const struct omt_hal omt_zephyr_hal;
int omt_zephyr_hal_init(void);

static struct omt_fw fw;

#if DT_HAS_CHOSEN(omt_host_i2c)
#include <zephyr/drivers/i2c.h>
/* Byte-wise I2C target callbacks mapped onto omt_i2c_write/read. */
static uint8_t wbuf[130];
static size_t wlen;
static int t_write_requested(struct i2c_target_config *c) { ARG_UNUSED(c); wlen = 0; return 0; }
static int t_write_received(struct i2c_target_config *c, uint8_t v)
{
	ARG_UNUSED(c);
	if (wlen < sizeof(wbuf)) {
		wbuf[wlen++] = v;
	}
	return 0;
}
static int t_read_requested(struct i2c_target_config *c, uint8_t *v)
{
	ARG_UNUSED(c);
	if (wlen) {             /* address-only write before a repeated start */
		omt_i2c_write(&fw, wbuf, wlen);
		wlen = 0;
	}
	return omt_i2c_read(&fw, v, 1);
}
static int t_read_processed(struct i2c_target_config *c, uint8_t *v)
{
	ARG_UNUSED(c);
	return omt_i2c_read(&fw, v, 1);
}
static int t_stop(struct i2c_target_config *c)
{
	ARG_UNUSED(c);
	if (wlen) {
		omt_i2c_write(&fw, wbuf, wlen);
		wlen = 0;
	}
	return 0;
}
static const struct i2c_target_callbacks tcb = {
	.write_requested = t_write_requested, .write_received = t_write_received,
	.read_requested = t_read_requested, .read_processed = t_read_processed, .stop = t_stop,
};
static struct i2c_target_config tcfg = { .address = 0x50, .callbacks = &tcb };
#endif

static void fw_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	while (1) {
		omt_fw_tick(&fw);
		k_msleep(CONFIG_OMT_TICK_MS);
	}
}
K_THREAD_DEFINE(omt_fw_tid, 4096, fw_thread, NULL, NULL, NULL, 5, 0, 100);

#ifdef CONFIG_OMT_HOST_SELFTEST
static uint8_t host_rd(uint8_t off)
{
	uint8_t v;

	omt_i2c_write(&fw, &off, 1);
	omt_i2c_read(&fw, &v, 1);
	return v;
}

static void host_thread(void *a, void *b, void *c)
{
	static const char *const st[] = { "MgmtInit", "LowPwr", "PwrUp", "Ready", "PwrDn", "Fault" };
	const uint8_t up[2] = { CMIS_MOD_CTRL, 0x00 };

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	k_msleep(200);
	printk("host: id 0x%02X cmis %u.%u state %s fw %u.%u\n", host_rd(0), host_rd(1) >> 4,
	       host_rd(1) & 0xF, st[(host_rd(3) >> 1) & 7], host_rd(39), host_rd(40));
	omt_i2c_write(&fw, up, sizeof(up));   /* clear LowPwr requests */
	k_msleep(600);
	uint8_t s = (host_rd(3) >> 1) & 7;

	printk("host: after power-up request state %s, fault cause %u\n", st[s < 6 ? s : 0], host_rd(41));
	printk("host: done\n");
}
K_THREAD_DEFINE(omt_host_tid, 2048, host_thread, NULL, NULL, NULL, 4, 0, 300);
#endif

static int cmd_omt(const struct shell *sh, size_t argc, char **argv)
{
	char line[96] = { 0 };

	for (size_t i = 1; i < argc; i++) {
		strncat(line, argv[i], sizeof(line) - strlen(line) - 2);
		strcat(line, i + 1 < argc ? " " : "\n");
	}
	omt_console_rx(&fw, line, strlen(line));
	return 0;
}
SHELL_CMD_ARG_REGISTER(omt, NULL, "firmware console: omt <help|ver|state|mon|faults ...>", cmd_omt, 2, 6);

int main(void)
{
	int rc = omt_zephyr_hal_init();

	printk("omt: module controller firmware on Zephyr %s, NVM %s\n", KERNEL_VERSION_STRING,
	       rc ? "unavailable" : "ok");
	omt_fw_init(&fw, &omt_zephyr_hal);
#if DT_HAS_CHOSEN(omt_host_i2c)
	(void)i2c_target_register(DEVICE_DT_GET(DT_CHOSEN(omt_host_i2c)), &tcfg);
#endif
	return 0;
}
