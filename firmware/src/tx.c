/*
 * Transmitter: quad laser driver on SPI and automatic power control (APC).
 *
 * Laser driver register map (3-byte SPI frames: [R/W|addr][hi][lo]):
 *   0x00 ID        0x4C44
 *   0x01 CTRL      b0..3 laser enable, b4..7 output enable, b15 soft reset
 *   0x02 STATUS    b0..3 channel fault (over-current / open), clear on read
 *   0x10+ch BIAS_SET  12 bit, 120 mA full scale
 *   0x14+ch BIAS_MON  12 bit, 120 mA full scale
 *   0x18+ch MPD_MON   12 bit, back-facet monitor photodiode
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>

#include "omt/fw.h"

#define LD_ID          0x4C44
#define LD_REG_ID      0x00
#define LD_REG_CTRL    0x01
#define LD_REG_STATUS  0x02
#define LD_REG_BIAS    0x10
#define LD_REG_BIASMON 0x14
#define LD_REG_MPD     0x18

#define BIAS_FS_MA      120.0f
#define BIAS_PRE_MA      30.0f
#define BIAS_MAX_MA     105.0f
#define APC_PERIOD_MS     5u
#define EYE_SAFETY_UW  3981.0f  /* +6 dBm */

int ld_read(struct omt_fw *fw, uint8_t reg, uint16_t *val)
{
	uint8_t tx[3] = { (uint8_t)(0x80 | reg), 0, 0 }, rx[3] = { 0 };

	if (fw->hal->spi_xfer(fw->hal->ctx, tx, rx, 3) < 0) {
		return -1;
	}
	*val = (uint16_t)((rx[1] << 8) | rx[2]);
	return 0;
}

int ld_write(struct omt_fw *fw, uint8_t reg, uint16_t val)
{
	uint8_t tx[3] = { (uint8_t)(reg & 0x7F), (uint8_t)(val >> 8), (uint8_t)val }, rx[3];

	return fw->hal->spi_xfer(fw->hal->ctx, tx, rx, 3);
}

static uint16_t ctrl_word(const struct omt_fw *fw)
{
	uint16_t w = 0;

	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		if (fw->tx[l].enabled) {
			w |= (uint16_t)(1u << l);
			if (!fw->tx[l].squelched) {
				w |= (uint16_t)(0x10u << l);
			}
		}
	}
	return w;
}

static uint16_t ma_to_code(float ma)
{
	float c = ma / BIAS_FS_MA * 4095.0f;

	return (uint16_t)(c < 0 ? 0 : (c > 4095 ? 4095 : c));
}

int tx_init(struct omt_fw *fw)
{
	uint16_t id = 0;

	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_TX_PWR, 1);
	if (ld_read(fw, LD_REG_ID, &id) < 0 || id != LD_ID) {
		return -1;
	}
	(void)ld_write(fw, LD_REG_CTRL, 0x8000);
	(void)ld_write(fw, LD_REG_CTRL, 0);
	(void)ld_read(fw, LD_REG_STATUS, &id); /* clear stale faults */
	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		struct omt_tx_lane *x = &fw->tx[l];

		*x = (struct omt_tx_lane){ .squelched = true };
		x->target_uw = 1000.0f * powf(10.0f, fw->cal.tx_target_dbm / 10.0f);
	}
	return 0;
}

void tx_shutdown(struct omt_fw *fw)
{
	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		fw->tx[l].enabled = false;
		fw->tx[l].squelched = true;
		fw->tx[l].converged = false;
		fw->tx[l].bias_cmd_ma = 0;
		fw->tx[l].tx_uw = 0;
		fw->tx[l].bias_meas_ma = 0;
	}
	(void)ld_write(fw, LD_REG_CTRL, 0);
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_TX_PWR, 0);
}

void tx_lane_enable(struct omt_fw *fw, uint8_t lane, bool on)
{
	struct omt_tx_lane *x = &fw->tx[lane];

	if (on && x->fault) {
		return; /* latched laser fault: only a module reset re-arms it */
	}
	x->enabled = on;
	x->converged = false;
	x->armed = false;
	x->conv_ms = 0;
	x->sat_ms = 0;
	x->bias_cmd_ma = on ? BIAS_PRE_MA : 0.0f;
	x->target_uw = 1000.0f * powf(10.0f, fw->cal.tx_target_dbm / 10.0f);
	(void)ld_write(fw, (uint8_t)(LD_REG_BIAS + lane), ma_to_code(x->bias_cmd_ma));
	(void)ld_write(fw, LD_REG_CTRL, ctrl_word(fw));
}

void tx_lane_squelch(struct omt_fw *fw, uint8_t lane, bool sq)
{
	fw->tx[lane].squelched = sq;
	(void)ld_write(fw, LD_REG_CTRL, ctrl_word(fw));
}

static void lane_fault(struct omt_fw *fw, uint8_t lane, uint16_t ev)
{
	struct omt_tx_lane *x = &fw->tx[lane];

	x->fault = true;
	x->enabled = false;
	x->converged = false;
	(void)ld_write(fw, (uint8_t)(LD_REG_BIAS + lane), 0);
	(void)ld_write(fw, LD_REG_CTRL, ctrl_word(fw));
	omt_event(fw, ev, lane);
}

void apc_task(struct omt_fw *fw)
{
	uint16_t status = 0;
	bool any = false;

	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		any |= fw->tx[l].enabled;
	}
	if (!any) {
		return;
	}
	if (ld_read(fw, LD_REG_STATUS, &status) == 0 && (status & 0xF)) {
		for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
			if ((status & (1u << l)) && fw->tx[l].enabled) {
				lane_fault(fw, l, EV_LASER_DRV_FAULT);
			}
		}
	}

	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		struct omt_tx_lane *x = &fw->tx[l];
		uint16_t bm = 0, mpd = 0;

		if (!x->enabled) {
			continue;
		}
		if (ld_read(fw, (uint8_t)(LD_REG_BIASMON + l), &bm) < 0 ||
		    ld_read(fw, (uint8_t)(LD_REG_MPD + l), &mpd) < 0) {
			continue;
		}
		x->mpd_raw = mpd;
		x->bias_meas_ma = (float)bm * BIAS_FS_MA / 4095.0f;
		x->tx_uw = (float)mpd * fw->cal.mpd_slope[l];

		if (x->tx_uw > EYE_SAFETY_UW) {
			lane_fault(fw, l, EV_EYE_SAFETY);
			continue;
		}

		/* integral controller on the back-facet monitor */
		float err = x->target_uw - x->tx_uw;

		x->bias_cmd_ma += fw->tun.apc_ki * err;
		if (x->bias_cmd_ma > BIAS_MAX_MA) {
			x->bias_cmd_ma = BIAS_MAX_MA;
		} else if (x->bias_cmd_ma < 5.0f) {
			x->bias_cmd_ma = 5.0f;
		}
		(void)ld_write(fw, (uint8_t)(LD_REG_BIAS + l), ma_to_code(x->bias_cmd_ma));

		if (fabsf(err) < 0.03f * x->target_uw) {
			if (x->conv_ms < 1000) {
				x->conv_ms = (uint16_t)(x->conv_ms + APC_PERIOD_MS);
			}
			if (x->conv_ms >= 50) {
				x->converged = true;
				x->armed = true;
			}
		} else if (fabsf(err) > 0.10f * x->target_uw) {
			x->conv_ms = 0;
			x->converged = false;
		}

		/* end of life: pinned at max bias and > 3 dB short for 200 ms */
		if (x->bias_cmd_ma >= BIAS_MAX_MA && x->tx_uw < 0.5f * x->target_uw) {
			x->sat_ms = (uint16_t)(x->sat_ms + APC_PERIOD_MS);
			if (x->sat_ms >= 200) {
				lane_fault(fw, l, EV_LASER_EOL);
			}
		} else {
			x->sat_ms = 0;
		}
	}
}
