/*
 * Zephyr implementation of struct omt_hal.
 *
 * Every peripheral is optional in devicetree; missing ones return -ENODEV so
 * the same image runs on native_sim (flash only) and on a full board.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#if DT_NODE_EXISTS(DT_NODELABEL(omt_laser))
#include <zephyr/drivers/spi.h>
static const struct spi_dt_spec laser = SPI_DT_SPEC_GET(DT_NODELABEL(omt_laser),
							 SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
#endif
#if DT_NODE_EXISTS(DT_NODELABEL(omt_mdio))
#include <zephyr/drivers/mdio.h>
static const struct device *const mdio = DEVICE_DT_GET(DT_NODELABEL(omt_mdio));
#endif
#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
#include <zephyr/drivers/watchdog.h>
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_ch = -1;
#endif

#include "omt/fw.h"

/* Pins without a devicetree binding are kept virtual (native_sim). */
static int vpin[OMT_PIN_COUNT] = { [OMT_PIN_LPMODE] = 1, [OMT_PIN_RESETL] = 1, [OMT_PIN_INTL] = 1 };
static const struct flash_area *nvm;
static unsigned int irq_key;
static int irq_depth;

static uint32_t z_millis(void *ctx)
{
	ARG_UNUSED(ctx);
	return k_uptime_get_32();
}

static uint8_t z_reset_cause(void *ctx)
{
	uint32_t c = 0;

	ARG_UNUSED(ctx);
	if (hwinfo_get_reset_cause(&c) != 0) {
		return OMT_RST_POWER_ON;
	}
	(void)hwinfo_clear_reset_cause();
	if (c & RESET_WATCHDOG) {
		return OMT_RST_WATCHDOG;
	}
	if (c & RESET_SOFTWARE) {
		return OMT_RST_SOFTWARE;
	}
	if (c & RESET_PIN) {
		return OMT_RST_HOST_PIN;
	}
	return OMT_RST_POWER_ON;
}

static int z_spi(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len)
{
	ARG_UNUSED(ctx);
#if DT_NODE_EXISTS(DT_NODELABEL(omt_laser))
	const struct spi_buf tb = { .buf = (void *)tx, .len = len }, rb = { .buf = rx, .len = len };
	const struct spi_buf_set ts = { &tb, 1 }, rs = { &rb, 1 };

	return spi_transceive_dt(&laser, &ts, &rs);
#else
	ARG_UNUSED(tx);
	memset(rx, 0xFF, len); /* no laser driver: bus floats high */
	return 0;
#endif
}

static int z_mdio_read(void *ctx, uint8_t prtad, uint8_t devad, uint16_t reg, uint16_t *val)
{
	ARG_UNUSED(ctx);
#if DT_NODE_EXISTS(DT_NODELABEL(omt_mdio))
	return mdio_read_c45(mdio, prtad, devad, reg, val);
#else
	ARG_UNUSED(prtad); ARG_UNUSED(devad); ARG_UNUSED(reg); ARG_UNUSED(val);
	return -ENODEV;
#endif
}

static int z_mdio_write(void *ctx, uint8_t prtad, uint8_t devad, uint16_t reg, uint16_t val)
{
	ARG_UNUSED(ctx);
#if DT_NODE_EXISTS(DT_NODELABEL(omt_mdio))
	return mdio_write_c45(mdio, prtad, devad, reg, val);
#else
	ARG_UNUSED(prtad); ARG_UNUSED(devad); ARG_UNUSED(reg); ARG_UNUSED(val);
	return -ENODEV;
#endif
}

static int z_adc(void *ctx, uint8_t ch, uint16_t *raw)
{
	ARG_UNUSED(ctx);
	/* Board ports bind zephyr,user io-channels here (adc_read_dt). Without
	 * an ADC report a mid-scale reading: 25 degC sensors, 3.3 V supply. */
	static const uint16_t idle[OMT_ADC_COUNT] = { 19661, 43253, 200, 200, 200, 200, 19661, 32768 };

	*raw = ch < OMT_ADC_COUNT ? idle[ch] : 0;
	return 0;
}

static int z_dac(void *ctx, uint8_t ch, uint16_t val)
{
	ARG_UNUSED(ctx); ARG_UNUSED(ch); ARG_UNUSED(val);
	return 0;
}

static int z_gpio_get(void *ctx, uint8_t pin)
{
	ARG_UNUSED(ctx);
	return pin < OMT_PIN_COUNT ? vpin[pin] : 0;
}

static void z_gpio_set(void *ctx, uint8_t pin, int level)
{
	ARG_UNUSED(ctx);
	if (pin < OMT_PIN_COUNT) {
		vpin[pin] = level;
	}
}

static int z_nvm_read(void *ctx, uint32_t addr, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return nvm ? flash_area_read(nvm, addr, buf, len) : -ENODEV;
}

static int z_nvm_write(void *ctx, uint32_t addr, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	return nvm ? flash_area_write(nvm, addr, buf, len) : -ENODEV;
}

static int z_nvm_erase(void *ctx, uint32_t addr, size_t len)
{
	ARG_UNUSED(ctx);
	return nvm ? flash_area_erase(nvm, addr, len) : -ENODEV;
}

static void z_kick(void *ctx)
{
	ARG_UNUSED(ctx);
#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
	if (wdt_ch >= 0) {
		(void)wdt_feed(wdt, wdt_ch);
	}
#endif
}

static void z_uart(void *ctx, const char *s, size_t n)
{
	ARG_UNUSED(ctx);
	printk("%.*s", (int)n, s);
}

static void z_reset(void *ctx, uint8_t cause)
{
	ARG_UNUSED(ctx);
	printk("omt: reset requested (cause %u)\n", cause);
	sys_reboot(SYS_REBOOT_COLD);
}

/* The CMIS I2C target ISR must not observe half-updated multi-byte values. */
static void z_lock(void *ctx)
{
	ARG_UNUSED(ctx);
	unsigned int k = irq_lock();

	if (irq_depth++ == 0) {
		irq_key = k;
	}
}

static void z_unlock(void *ctx)
{
	ARG_UNUSED(ctx);
	if (--irq_depth == 0) {
		irq_unlock(irq_key);
	}
}

const struct omt_hal omt_zephyr_hal = {
	.millis = z_millis, .reset_cause = z_reset_cause, .spi_xfer = z_spi,
	.mdio_read = z_mdio_read, .mdio_write = z_mdio_write, .adc_read = z_adc,
	.dac_write = z_dac, .gpio_get = z_gpio_get, .gpio_set = z_gpio_set,
	.nvm_read = z_nvm_read, .nvm_write = z_nvm_write, .nvm_erase = z_nvm_erase,
	.wdt_kick = z_kick, .uart_tx = z_uart, .system_reset = z_reset,
	.irq_lock = z_lock, .irq_unlock = z_unlock,
};

int omt_zephyr_hal_init(void)
{
	int rc = flash_area_open(DT_FIXED_PARTITION_ID(DT_CHOSEN(omt_nvm)), &nvm);

	if (rc == 0 && nvm->fa_size < OMT_NVM_SIZE) {
		printk("omt: NVM partition too small (%zu < %u)\n", nvm->fa_size, OMT_NVM_SIZE);
		nvm = NULL;
		return -ENOSPC;
	}
#if DT_NODE_EXISTS(DT_ALIAS(watchdog0))
	struct wdt_timeout_cfg cfg = { .window.max = 300, .flags = WDT_FLAG_RESET_SOC };

	wdt_ch = wdt_install_timeout(wdt, &cfg);
	if (wdt_ch >= 0) {
		(void)wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	}
#endif
	return rc;
}
