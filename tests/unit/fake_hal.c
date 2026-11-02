/* SPDX-License-Identifier: Apache-2.0 */
/* Deterministic in-memory HAL for unit tests (no physics). */
#include <string.h>

#include "fake_hal.h"

static struct fake_hw *F(void *c) { return (struct fake_hw *)c; }

static uint32_t f_millis(void *c) { return F(c)->now; }
static uint8_t f_cause(void *c) { (void)c; return OMT_RST_POWER_ON; }
static int f_spi(void *c, const uint8_t *tx, uint8_t *rx, size_t n)
{
	(void)c;
	memset(rx, 0, n);
	if ((tx[0] & 0x7F) == 0 && (tx[0] & 0x80)) { /* ID */
		rx[1] = 0x4C;
		rx[2] = 0x44;
	}
	return 0;
}
static int f_mdr(void *c, uint8_t p, uint8_t d, uint16_t r, uint16_t *v)
{
	(void)c; (void)p; (void)d; (void)r;
	*v = 0;
	return -1; /* no DSP in unit tests */
}
static int f_mdw(void *c, uint8_t p, uint8_t d, uint16_t r, uint16_t v)
{
	(void)c; (void)p; (void)d; (void)r; (void)v;
	return -1;
}
static int f_adc(void *c, uint8_t ch, uint16_t *raw)
{
	*raw = F(c)->adc[ch];
	return 0;
}
static int f_dac(void *c, uint8_t ch, uint16_t v) { (void)c; (void)ch; (void)v; return 0; }
static int f_gget(void *c, uint8_t pin) { return F(c)->pin[pin]; }
static void f_gset(void *c, uint8_t pin, int v) { F(c)->pin[pin] = v; }
static int f_nr(void *c, uint32_t a, void *b, size_t n)
{
	memcpy(b, &F(c)->flash[a], n);
	return 0;
}
static int f_nw(void *c, uint32_t a, const void *b, size_t n)
{
	const uint8_t *p = b;

	for (size_t i = 0; i < n; i++) {
		F(c)->flash[a + i] &= p[i];
	}
	return 0;
}
static int f_ne(void *c, uint32_t a, size_t n)
{
	memset(&F(c)->flash[a], 0xFF, n);
	F(c)->erases++;
	return 0;
}
static void f_kick(void *c) { F(c)->kicks++; }
static void f_uart(void *c, const char *s, size_t n)
{
	struct fake_hw *f = F(c);

	if (f->uart_len + n < sizeof(f->uart) - 1) {
		memcpy(f->uart + f->uart_len, s, n);
		f->uart_len += n;
		f->uart[f->uart_len] = 0;
	}
}
static void f_reset(void *c, uint8_t cause) { F(c)->reset_req = cause + 1; }
static void f_lock(void *c) { F(c)->irq_depth++; }
static void f_unlock(void *c) { F(c)->irq_depth--; }

void fake_hal_init(struct fake_hw *f)
{
	memset(f, 0, sizeof(*f));
	memset(f->flash, 0xFF, sizeof(f->flash));
	f->pin[OMT_PIN_LPMODE] = 1;
	f->pin[OMT_PIN_RESETL] = 1;
	/* 30 degC module, 3.3 V */
	f->adc[OMT_ADC_TEMP] = (uint16_t)((0.5 + 0.30) / 2.5 * 65535);
	f->adc[OMT_ADC_VCC] = (uint16_t)(1.65 / 2.5 * 65535);
	f->adc[OMT_ADC_LASER_TEMP] = (uint16_t)((0.5 + 0.50) / 2.5 * 65535);
	f->adc[OMT_ADC_TEC_I] = (uint16_t)(1.25 / 2.5 * 65535);
	f->hal = (struct omt_hal){
		.ctx = f, .millis = f_millis, .reset_cause = f_cause, .spi_xfer = f_spi,
		.mdio_read = f_mdr, .mdio_write = f_mdw, .adc_read = f_adc, .dac_write = f_dac,
		.gpio_get = f_gget, .gpio_set = f_gset, .nvm_read = f_nr, .nvm_write = f_nw,
		.nvm_erase = f_ne, .wdt_kick = f_kick, .uart_tx = f_uart, .system_reset = f_reset,
		.irq_lock = f_lock, .irq_unlock = f_unlock,
	};
}

void fake_run(struct fake_hw *f, struct omt_fw *fw, uint32_t ms)
{
	while (ms--) {
		f->now++;
		omt_fw_tick(fw);
	}
}

void host_write(struct omt_fw *fw, uint8_t off, const uint8_t *d, size_t n)
{
	uint8_t buf[130];

	buf[0] = off;
	memcpy(buf + 1, d, n);
	omt_i2c_write(fw, buf, n + 1);
}

void host_write1(struct omt_fw *fw, uint8_t off, uint8_t v)
{
	host_write(fw, off, &v, 1);
}

void host_read(struct omt_fw *fw, uint8_t off, uint8_t *d, size_t n)
{
	omt_i2c_write(fw, &off, 1);
	omt_i2c_read(fw, d, n);
}

uint8_t host_read1(struct omt_fw *fw, uint8_t off)
{
	uint8_t v;

	host_read(fw, off, &v, 1);
	return v;
}
