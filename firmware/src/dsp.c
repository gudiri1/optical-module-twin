/*
 * PAM4 DSP supervision over MDIO (IEEE 802.3 Clause 45).
 *
 * Register map of the (simulated) DSP, device 30 = vendor specific:
 *   1.0002/1.0003  PMA device identifier
 *   30.0000 GCTRL     b15 soft reset, b0 start init, b1 mode (0 1x400G, 1 4x100G)
 *   30.0001 GSTAT     b0 ready, b15 internal fault
 *   30.0002 SNAPSHOT  write 1: latch BER/FEC counters, restart accumulation
 *   30.0003 HEARTBEAT free-running counter of the DSP firmware
 *   30.0010+l LCTRL   b0 TX (host->media) enable, b1 RX enable
 *   30.0020+l LSTAT   b0 host CDR lock, b1 media CDR lock, b2 FFE converged
 *   30.0030+l SNR     media input SNR, 0.01 dB
 *   30.0040+4l ERR_HI, ERR_LO, BITS_HI, BITS_LO  (bits in units of 2^16)
 *   30.0060+l FEC_CORR    corrected RS(544,514) codewords in snapshot
 *   30.0068+l FEC_UNCORR  uncorrectable codewords in snapshot
 *   30.0070 DIE_TEMP  0.1 degC
 *
 * Host side (module Tx input, 8 lanes h = 0..7, host lane h feeds media lane h/2):
 *   30.0100+h HCTRL   b0 adaptive CTLE, b7..4 fixed CTLE peaking dB
 *   30.0110+h HSTAT   b0 signal detect, b1 host CDR lock, b2 adaptation done
 *   30.0120+h HSNR    host input SNR after CTLE, 0.01 dB
 *   30.0130+h HEQ     CTLE peaking in use, dB
 *
 * PRBS generators/checkers and loopbacks (CMIS Page 13h/14h back end):
 *   30.0200 HGEN_EN  30.0201 HCHK_EN (bit per host lane)
 *   30.0202 MGEN_EN  30.0203 MCHK_EN (bit per media lane)
 *   30.0204 LB_MEDIA b3..0 media output loopback, b7..4 media input loopback
 *   30.0205 LB_HOST_OUT  30.0206 LB_HOST_IN (bit per host lane)
 *   30.0208+h host pattern, 30.0210+m media pattern (CMIS pattern codes)
 *   30.0218 CHK_LOCK b7..0 host checkers, b11..8 media checkers
 *   30.021A CHK_SNAP write b0: latch checker counters and restart, b1: discard
 *   30.0220+4h / 30.0240+4m  checker ERR_HI, ERR_LO, BITS_HI, BITS_LO (snapshot)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>

#include "omt/fw.h"

#define DSP_PRTAD 0
#define DEV_PMA   1
#define DEV_VS    30

#define R_GCTRL   0x0000
#define R_GSTAT   0x0001
#define R_SNAP    0x0002
#define R_HB      0x0003
#define R_LCTRL   0x0010
#define R_LSTAT   0x0020
#define R_SNR     0x0030
#define R_BER     0x0040
#define R_FECC    0x0060
#define R_FECU    0x0068
#define R_DTEMP   0x0070
#define R_HCTRL   0x0100
#define R_HSTAT   0x0110
#define R_HSNR    0x0120
#define R_HEQ     0x0130
#define R_HGEN    0x0200
#define R_HCHK    0x0201
#define R_MGEN    0x0202
#define R_MCHK    0x0203
#define R_LB_M    0x0204
#define R_LB_HO   0x0205
#define R_LB_HI   0x0206
#define R_PAT_H   0x0208
#define R_PAT_M   0x0210
#define R_CHKLOCK 0x0218
#define R_CHKSNAP 0x021A
#define R_CHK_H   0x0220
#define R_CHK_M   0x0240

#define DSP_ID1 0x7F44
#define DSP_ID2 0x5350

static int mdr(struct omt_fw *fw, uint8_t dev, uint16_t reg, uint16_t *v)
{
	for (int tries = 0; tries < 3; tries++) {
		if (fw->hal->mdio_read(fw->hal->ctx, DSP_PRTAD, dev, reg, v) == 0) {
			return 0;
		}
	}
	if ((fw->mdio_errors++ & 0x3F) == 0) {
		omt_event(fw, EV_MDIO_ERROR, reg); /* rate-limited */
	}
	return -1;
}

static int mdw(struct omt_fw *fw, uint8_t dev, uint16_t reg, uint16_t v)
{
	for (int tries = 0; tries < 3; tries++) {
		if (fw->hal->mdio_write(fw->hal->ctx, DSP_PRTAD, dev, reg, v) == 0) {
			return 0;
		}
	}
	if ((fw->mdio_errors++ & 0x3F) == 0) {
		omt_event(fw, EV_MDIO_ERROR, reg);
	}
	return -1;
}

void dsp_power_up(struct omt_fw *fw)
{
	fw->dsp_ready = false;
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_RSTN, 0);
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_PWR, 1);
}

void dsp_release_reset(struct omt_fw *fw)
{
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_RSTN, 1);
}

int dsp_start(struct omt_fw *fw)
{
	uint16_t id1 = 0, id2 = 0;

	if (mdr(fw, DEV_PMA, 0x0002, &id1) < 0 || mdr(fw, DEV_PMA, 0x0003, &id2) < 0 ||
	    id1 != DSP_ID1 || id2 != DSP_ID2) {
		return -1;
	}
	return mdw(fw, DEV_VS, R_GCTRL, (uint16_t)(0x0001 | (fw->dsp_mode ? 0x0002 : 0)));
}

bool dsp_poll_ready(struct omt_fw *fw)
{
	uint16_t st = 0;

	if (mdr(fw, DEV_VS, R_GSTAT, &st) < 0) {
		return false;
	}
	fw->dsp_ready = (st & 0x0001) && !(st & 0x8000);
	if (fw->dsp_ready) {
		(void)mdr(fw, DEV_VS, R_HB, &fw->dsp_heartbeat);
		fw->dsp_hb_stall = 0;
	}
	return fw->dsp_ready;
}

void dsp_power_down(struct omt_fw *fw)
{
	fw->dsp_ready = false;
	fw->dsp_lane_on = 0;
	fw->dsp_rec_step = 0;
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_RSTN, 0);
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_PWR, 0);
	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		fw->rx[l].lol = true;
		fw->rx[l].snr_db = 0;
		fw->rx[l].prefec_ber = 0;
	}
	memset(fw->host, 0, sizeof(fw->host));
	fw->diag_applied = false;
}

int dsp_set_mode(struct omt_fw *fw, uint8_t mode)
{
	if (fw->dsp_mode == mode) {
		return 0;
	}
	fw->dsp_mode = mode;
	if (!fw->dsp_ready) {
		return 0;
	}
	return mdw(fw, DEV_VS, R_GCTRL, (uint16_t)(0x0001 | (mode ? 0x0002 : 0)));
}

int dsp_lane_enable(struct omt_fw *fw, uint8_t lane, bool on)
{
	if (on) {
		fw->dsp_lane_on |= (uint8_t)(1u << lane);
	} else {
		fw->dsp_lane_on &= (uint8_t)~(1u << lane);
	}
	return mdw(fw, DEV_VS, (uint16_t)(R_LCTRL + lane), on ? 0x0003 : 0x0000);
}

bool dsp_lane_locked(struct omt_fw *fw, uint8_t lane, bool *host_lock)
{
	uint16_t st = 0;

	if (mdr(fw, DEV_VS, (uint16_t)(R_LSTAT + lane), &st) < 0) {
		if (host_lock) {
			*host_lock = false;
		}
		return false;
	}
	if (host_lock) {
		*host_lock = st & 0x1;
	}
	return (st & 0x2) != 0;
}

int dsp_host_eq(struct omt_fw *fw, uint8_t h, bool adaptive, uint8_t fixed_db)
{
	if (fixed_db > 12) {
		fixed_db = 12; /* advertised maximum, Page 01h byte 153 */
	}
	return mdw(fw, DEV_VS, (uint16_t)(R_HCTRL + h), (uint16_t)((adaptive ? 1u : 0u) | (fixed_db << 4)));
}

void dsp_read_host(struct omt_fw *fw, uint8_t h)
{
	struct omt_host_lane *x = &fw->host[h];
	uint16_t st = 0, snr = 0, eq = 0, ctrl = 0;

	if (mdr(fw, DEV_VS, (uint16_t)(R_HSTAT + h), &st) < 0) {
		x->sig = x->lock = false;
		return;
	}
	(void)mdr(fw, DEV_VS, (uint16_t)(R_HSNR + h), &snr);
	(void)mdr(fw, DEV_VS, (uint16_t)(R_HEQ + h), &eq);
	(void)mdr(fw, DEV_VS, (uint16_t)(R_HCTRL + h), &ctrl);
	x->sig = st & 0x1;
	x->lock = (st & 0x2) != 0;
	x->adapt_done = (st & 0x4) != 0;
	x->snr_db = snr / 100.0f;
	x->eq_db = (uint8_t)eq;
	/* adaptation finished on a present signal but the CDR cannot lock:
	 * the channel is outside what the CTLE can equalize */
	x->adapt_fault = (ctrl & 0x1) && x->sig && x->adapt_done && !x->lock;
}

static uint16_t pat_nibble(const uint8_t *base, uint8_t lane)
{
	uint8_t b = base[lane / 2];

	return (lane & 1) ? (uint16_t)(b >> 4) : (uint16_t)(b & 0xF);
}

/* Program generators, checkers and loopbacks from the Page 13h controls.
 * Lanes asking for an unsupported pattern are left disabled. */
int dsp_diag_program(struct omt_fw *fw)
{
	const uint8_t *p = fw->m.p13;
	uint8_t hgen = p[P13_HOST_GEN_EN - 128], hchk = p[P13_HOST_CHK_EN - 128];
	uint8_t mgen = p[P13_MEDIA_GEN_EN - 128] & 0x0F, mchk = p[P13_MEDIA_CHK_EN - 128] & 0x0F;
	int rc = 0;

	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		uint16_t pg = pat_nibble(&p[P13_HOST_GEN_PAT - 128], h);
		uint16_t pc = pat_nibble(&p[P13_HOST_CHK_PAT - 128], h);

		if (!(OMT_PATTERNS_SUPPORTED & (1u << pg))) {
			hgen &= (uint8_t)~(1u << h);
		}
		if (!(OMT_PATTERNS_SUPPORTED & (1u << pc))) {
			hchk &= (uint8_t)~(1u << h);
		}
		/* one pattern register per lane: the checker's when it runs */
		rc |= mdw(fw, DEV_VS, (uint16_t)(R_PAT_H + h), (hchk & (1u << h)) ? pc : pg);
	}
	for (uint8_t m = 0; m < OMT_N_MEDIA_LANES; m++) {
		uint16_t pg = pat_nibble(&p[P13_MEDIA_GEN_PAT - 128], m);
		uint16_t pc = pat_nibble(&p[P13_MEDIA_CHK_PAT - 128], m);

		if (!(OMT_PATTERNS_SUPPORTED & (1u << pg))) {
			mgen &= (uint8_t)~(1u << m);
		}
		if (!(OMT_PATTERNS_SUPPORTED & (1u << pc))) {
			mchk &= (uint8_t)~(1u << m);
		}
		rc |= mdw(fw, DEV_VS, (uint16_t)(R_PAT_M + m), (mchk & (1u << m)) ? pc : pg);
	}
	rc |= mdw(fw, DEV_VS, R_HGEN, hgen);
	rc |= mdw(fw, DEV_VS, R_HCHK, hchk);
	rc |= mdw(fw, DEV_VS, R_MGEN, mgen);
	rc |= mdw(fw, DEV_VS, R_MCHK, mchk);
	rc |= mdw(fw, DEV_VS, R_LB_M, (uint16_t)((p[P13_LB_MEDIA_OUT - 128] & 0x0F) |
						  ((p[P13_LB_MEDIA_IN - 128] & 0x0F) << 4)));
	rc |= mdw(fw, DEV_VS, R_LB_HO, p[P13_LB_HOST_OUT - 128]);
	rc |= mdw(fw, DEV_VS, R_LB_HI, p[P13_LB_HOST_IN - 128]);
	return rc ? -1 : 0;
}

static void chk_accumulate(struct omt_fw *fw, struct omt_chk *c, uint16_t base, bool lock)
{
	uint16_t r[4] = { 0 };

	c->lock = lock;
	for (int i = 0; i < 4; i++) {
		if (mdr(fw, DEV_VS, (uint16_t)(base + i), &r[i]) < 0) {
			return;
		}
	}
	if (!lock) {
		return; /* errors of an unlocked checker are meaningless */
	}
	c->err += ((uint32_t)r[0] << 16) | r[1];
	c->bits += (uint64_t)(((uint32_t)r[2] << 16) | r[3]) << 16;
}

/* Live checker lock: b7..0 host lanes, b11..8 media lanes; -1 on MDIO error */
int dsp_diag_lock(struct omt_fw *fw)
{
	uint16_t lock = 0;

	return mdr(fw, DEV_VS, R_CHKLOCK, &lock) < 0 ? -1 : (int)lock;
}

/* Drop whatever the checkers counted so far (reset, reconfiguration). */
void dsp_diag_discard(struct omt_fw *fw)
{
	(void)mdw(fw, DEV_VS, R_CHKSNAP, 2);
}

/* Latch the checker counters of the last window and accumulate them. */
void dsp_diag_read(struct omt_fw *fw)
{
	uint16_t lock = 0;
	uint8_t hchk = fw->m.p13[P13_HOST_CHK_EN - 128], mchk = fw->m.p13[P13_MEDIA_CHK_EN - 128];

	if (!fw->diag_applied || !(hchk | (mchk & 0x0F)) || mdw(fw, DEV_VS, R_CHKSNAP, 1) < 0 ||
	    mdr(fw, DEV_VS, R_CHKLOCK, &lock) < 0) {
		return;
	}
	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		if (hchk & (1u << h)) {
			chk_accumulate(fw, &fw->hchk[h], (uint16_t)(R_CHK_H + 4 * h), (lock >> h) & 1);
		}
	}
	for (uint8_t m = 0; m < OMT_N_MEDIA_LANES; m++) {
		if (mchk & (1u << m)) {
			chk_accumulate(fw, &fw->mchk[m], (uint16_t)(R_CHK_M + 4 * m), (lock >> (8 + m)) & 1);
		}
	}
}

static void read_ber_window(struct omt_fw *fw)
{
	(void)mdw(fw, DEV_VS, R_SNAP, 1);
	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		uint16_t r[4] = { 0 }, fc = 0, fu = 0;
		struct omt_rx_lane *x = &fw->rx[l];

		if (!(fw->dsp_lane_on & (1u << l))) {
			continue;
		}
		for (int i = 0; i < 4; i++) {
			(void)mdr(fw, DEV_VS, (uint16_t)(R_BER + 4 * l + i), &r[i]);
		}
		uint32_t err = ((uint32_t)r[0] << 16) | r[1];
		uint32_t bits16 = ((uint32_t)r[2] << 16) | r[3];

		if (bits16) {
			x->prefec_ber = (float)err / ((float)bits16 * 65536.0f);
		}
		(void)mdr(fw, DEV_VS, (uint16_t)(R_FECC + l), &fc);
		(void)mdr(fw, DEV_VS, (uint16_t)(R_FECU + l), &fu);
		x->fec_corr = fc;
		x->fec_uncorr += fu;
	}
}

/* DSP hang recovery: reset, re-init, restore lanes. Bounded retries. */
static void dsp_recover(struct omt_fw *fw)
{
	uint32_t now = omt_now(fw);

	switch (fw->dsp_rec_step) {
	case 1:
		fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_DSP_RSTN, 0);
		fw->dsp_rec_t = now;
		fw->dsp_rec_step = 2;
		break;
	case 2:
		if (now - fw->dsp_rec_t >= 20) {
			dsp_release_reset(fw);
			fw->dsp_rec_t = now;
			fw->dsp_rec_step = 3;
		}
		break;
	case 3:
		if (now - fw->dsp_rec_t >= 30) {
			if (dsp_start(fw) < 0) {
				msm_enter_fault(fw, FC_DSP, EV_DSP_NO_RESPONSE, 1);
				return;
			}
			fw->dsp_rec_t = now;
			fw->dsp_rec_step = 4;
		}
		break;
	case 4:
		if (dsp_poll_ready(fw)) {
			uint8_t lanes = fw->dsp_lane_on;

			for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
				if (lanes & (1u << l)) {
					(void)dsp_lane_enable(fw, l, true);
				}
			}
			serdes_program(fw, lanes); /* a DSP reset lost the host Eq */
			fw->diag_applied = false;  /* ... and the PRBS setup        */
			fw->dsp_rec_step = 0;
			fw->dsp_recoveries++;
			omt_event(fw, EV_DSP_RECOVERED, fw->dsp_recoveries_in_row);
		} else if (now - fw->dsp_rec_t > 500) {
			msm_enter_fault(fw, FC_DSP, EV_DSP_INIT_TIMEOUT, 1);
		}
		break;
	default:
		break;
	}
}

void dsp_task(struct omt_fw *fw)
{
	if (fw->dsp_rec_step) {
		dsp_recover(fw);
		return;
	}
	if (!fw->dsp_ready) {
		return;
	}

	uint16_t hb = 0;

	if (mdr(fw, DEV_VS, R_HB, &hb) < 0 || hb == fw->dsp_heartbeat) {
		if (++fw->dsp_hb_stall >= 3) {
			omt_event(fw, EV_DSP_HANG, hb);
			fw->dsp_last_hang_t = omt_now(fw);
			if (++fw->dsp_recoveries_in_row > 3) {
				msm_enter_fault(fw, FC_DSP, EV_DSP_HANG, fw->dsp_recoveries_in_row);
				return;
			}
			fw->dsp_ready = false;
			fw->dsp_rec_step = 1;
			for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
				fw->rx[l].lol = true;
			}
			return;
		}
	} else {
		fw->dsp_hb_stall = 0;
		fw->dsp_heartbeat = hb;
		if (omt_now(fw) - fw->dsp_last_hang_t > 10000) {
			fw->dsp_recoveries_in_row = 0; /* healthy for a while */
		}
	}

	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		struct omt_rx_lane *x = &fw->rx[l];
		uint16_t snr = 0;

		if (!(fw->dsp_lane_on & (1u << l))) {
			x->lol = true;
			memset(&fw->host[2 * l], 0, 2 * sizeof(fw->host[0]));
			continue;
		}
		x->lol = !dsp_lane_locked(fw, l, NULL);
		dsp_read_host(fw, (uint8_t)(2 * l));
		dsp_read_host(fw, (uint8_t)(2 * l + 1));
		if (mdr(fw, DEV_VS, (uint16_t)(R_SNR + l), &snr) == 0) {
			x->snr_db = snr / 100.0f;
		}
	}
	uint16_t t = 0;

	if (mdr(fw, DEV_VS, R_DTEMP, &t) == 0) {
		fw->dsp_temp_dc = (int16_t)t;
	}
	if (omt_now(fw) - fw->ber_window_t >= 1000) {
		fw->ber_window_t = omt_now(fw);
		read_ber_window(fw);
	}
}
