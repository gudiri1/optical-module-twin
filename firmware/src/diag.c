/*
 * Host-side signal integrity controls and PRBS diagnostics.
 *
 * Signal integrity (Page 10h staged control set 0 -> Page 11h active set):
 *   ApplyDPInit or ApplyImmediate copies bytes 153..173 for the selected
 *   host lanes, then the module's Tx input CTLE is programmed in the DSP
 *   (adaptive, or a fixed peaking in dB). The active set is re-applied when
 *   a data path starts and after a DSP hang recovery.
 *
 * Diagnostics (Page 13h controls -> DSP, results -> Page 14h):
 *   PRBS generators and checkers on both sides, four loopbacks. The diag
 *   task programs the DSP whenever the host wrote Page 13h (or the DSP lost
 *   its setup); checker counters are accumulated from the moment a checker
 *   was (re)configured. Page 14h shows
 *   BER (F16) or SNR depending on the selector, and latches checker LOL.
 *   Checkers have their own 1 s count window; a counter reset or a new
 *   configuration discards what the DSP counted before it.
 *
 * Offsets follow CMIS 5.x as modeled in cmis_map.h; verify against OIF-CMIS.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>

#include "omt/fw.h"

#define CHK_SYNC_MS 50u  /* checker pattern sync before LOL is judged */

/* CMIS F16: bits 15..11 exponent s, bits 10..0 mantissa m, value m * 10^(s-24).
 * Scale the mantissa into 204.7..2047 (the most digits that fit). Single
 * precision and no pow(): an M4F has no double FPU. */
uint16_t cmis_f16_encode(float v)
{
	int s = 24;

	if (!(v > 0.0f)) {
		return 0;
	}
	while (v > 2047.0f && s < 31) {
		v /= 10.0f;
		s++;
	}
	while (v < 204.7f && s > 0) {
		v *= 10.0f;
		s--;
	}
	if (v > 2047.0f) {
		return 0xFFFF;
	}
	return (uint16_t)((s << 11) | (uint16_t)lrintf(v));
}

static uint8_t nib(const uint8_t *base, uint8_t lane)
{
	uint8_t b = base[lane / 2];

	return (lane & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0xF);
}

static void set_nib(uint8_t *base, uint8_t lane, uint8_t v)
{
	uint8_t *b = &base[lane / 2];
	uint8_t sh = (lane & 1) ? 4 : 0;

	*b = (uint8_t)((*b & ~(0xF << sh)) | ((v & 0xF) << sh));
}

static void copy_bit(uint8_t *dst, const uint8_t *src, uint8_t lane)
{
	uint8_t bit = (uint8_t)(1u << lane);

	*dst = (uint8_t)((*dst & ~bit) | (*src & bit));
}

/* ---------------------------------------------------------------- SI */

void serdes_apply_staged(struct omt_fw *fw, uint8_t lanes)
{
	const uint8_t *s = fw->m.p10;
	uint8_t *a = fw->m.p11;
	uint8_t media = 0;

	fw->hal->irq_lock(fw->hal->ctx);
	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		if (!(lanes & (1u << h))) {
			continue;
		}
		copy_bit(&a[P11_SI_ADAPT_EQ - 128], &s[P10_SI_ADAPT_EQ - 128], h);
		copy_bit(&a[P11_SI_TX_CDR - 128], &s[P10_SI_TX_CDR - 128], h);
		copy_bit(&a[P11_SI_RX_CDR - 128], &s[P10_SI_RX_CDR - 128], h);
		set_nib(&a[P11_SI_TX_EQ - 128], h, nib(&s[P10_SI_TX_EQ - 128], h));
		set_nib(&a[P11_SI_RX_PRE - 128], h, nib(&s[P10_SI_RX_PRE - 128], h));
		set_nib(&a[P11_SI_RX_POST - 128], h, nib(&s[P10_SI_RX_POST - 128], h));
		set_nib(&a[P11_SI_RX_AMP - 128], h, nib(&s[P10_SI_RX_AMP - 128], h));
		media |= (uint8_t)(1u << (h / 2));
	}
	fw->hal->irq_unlock(fw->hal->ctx);
	if (fw->dsp_ready) {
		serdes_program(fw, media & fw->dsp_lane_on);
	}
}

void serdes_program(struct omt_fw *fw, uint8_t media_mask)
{
	const uint8_t *a = fw->m.p11;

	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		if (!(media_mask & (1u << (h / 2)))) {
			continue;
		}
		bool adaptive = (a[P11_SI_ADAPT_EQ - 128] >> h) & 1;

		(void)dsp_host_eq(fw, h, adaptive, nib(&a[P11_SI_TX_EQ - 128], h));
	}
}

/* ------------------------------------------------------- diagnostics */

void diag_init(struct omt_fw *fw)
{
	uint8_t *p = fw->m.p13;

	p[P13_LOOPBACK_CAP - 128] = 0x0F | 0x20 | 0x40; /* all four, per lane */
	p[P13_PATTERN_CAP - 128] = 0x80;                /* checker LOL reported */
	p[P13_REPORT_CAP - 128] = 0x01 | 0x20;          /* BER, SNR */
	cmis_set_u16(&p[P13_HOST_GEN_PATTERNS - 128], OMT_PATTERNS_SUPPORTED);
	cmis_set_u16(&p[P13_MEDIA_GEN_PATTERNS - 128], OMT_PATTERNS_SUPPORTED);
	cmis_set_u16(&p[P13_HOST_CHK_PATTERNS - 128], OMT_PATTERNS_SUPPORTED);
	cmis_set_u16(&p[P13_MEDIA_CHK_PATTERNS - 128], OMT_PATTERNS_SUPPORTED);
	fw->m.p14[P14_SELECTOR - 128] = P14_SEL_BER;
}

static void reset_counters(struct omt_fw *fw)
{
	memset(fw->hchk, 0, sizeof(fw->hchk));
	memset(fw->mchk, 0, sizeof(fw->mchk));
}

/* A checker whose enable or pattern changed starts counting from zero. */
static void restart_changed(struct omt_fw *fw)
{
	const uint8_t *now = &fw->m.p13[P13_CTRL_FIRST - 128];
	const uint8_t *was = fw->diag_shadow;
#define CTL(off) (now[(off) - P13_CTRL_FIRST])
#define OLD(off) (was[(off) - P13_CTRL_FIRST])

	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		bool en = (CTL(P13_HOST_CHK_EN) >> h) & 1, en0 = (OLD(P13_HOST_CHK_EN) >> h) & 1;

		if (en != en0 || nib(&CTL(P13_HOST_CHK_PAT), h) != nib(&OLD(P13_HOST_CHK_PAT), h) ||
		    ((CTL(P13_LB_HOST_OUT) ^ OLD(P13_LB_HOST_OUT)) >> h) & 1) {
			memset(&fw->hchk[h], 0, sizeof(fw->hchk[h]));
		}
	}
	for (uint8_t m = 0; m < OMT_N_MEDIA_LANES; m++) {
		bool en = (CTL(P13_MEDIA_CHK_EN) >> m) & 1, en0 = (OLD(P13_MEDIA_CHK_EN) >> m) & 1;

		if (en != en0 || nib(&CTL(P13_MEDIA_CHK_PAT), m) != nib(&OLD(P13_MEDIA_CHK_PAT), m) ||
		    ((CTL(P13_LB_MEDIA_OUT) ^ OLD(P13_LB_MEDIA_OUT)) >> m) & 1) {
			memset(&fw->mchk[m], 0, sizeof(fw->mchk[m]));
		}
	}
#undef CTL
#undef OLD
}

static uint16_t ber_f16(const struct omt_chk *c)
{
	return c->bits ? cmis_f16_encode((float)c->err / (float)c->bits) : 0;
}

static uint16_t snr_u16(float db)
{
	float v = db * 256.0f;

	return (uint16_t)(v < 0 ? 0 : (v > 65535.0f ? 65535.0f : v));
}

static void publish(struct omt_fw *fw)
{
	uint8_t *p14 = fw->m.p14;
	const uint8_t *p13 = fw->m.p13;
	uint8_t sel = p14[P14_SELECTOR - 128];
	uint8_t hlol = 0, mlol = 0;
	uint8_t hchk = fw->diag_applied ? p13[P13_HOST_CHK_EN - 128] : 0;
	uint8_t mchk = fw->diag_applied ? (p13[P13_MEDIA_CHK_EN - 128] & 0x0F) : 0;

	fw->hal->irq_lock(fw->hal->ctx);
	for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
		uint16_t v = 0;

		if (sel == P14_SEL_BER) {
			v = ber_f16(&fw->hchk[h]);
		} else if (sel == P14_SEL_SNR) {
			v = snr_u16(fw->host[h].snr_db);
		}
		cmis_set_u16(&p14[P14_RESULT_HOST - 128 + 2 * h], v);
		if ((hchk >> h) & 1 && !fw->hchk[h].lock) {
			hlol |= (uint8_t)(1u << h);
		}
	}
	for (uint8_t m = 0; m < OMT_N_MEDIA_LANES; m++) {
		uint16_t v = 0;

		if (sel == P14_SEL_BER) {
			v = ber_f16(&fw->mchk[m]);
		} else if (sel == P14_SEL_SNR) {
			v = snr_u16(fw->rx[m].snr_db);
		}
		cmis_set_u16(&p14[P14_RESULT_MEDIA - 128 + 2 * m], v);
		if ((mchk >> m) & 1 && !fw->mchk[m].lock) {
			mlol |= (uint8_t)(1u << m);
		}
	}
	cmis_latch_diag(fw, P14_FLAG_HOST_CHK_LOL, hlol);
	cmis_latch_diag(fw, P14_FLAG_MEDIA_CHK_LOL, mlol);
	fw->hal->irq_unlock(fw->hal->ctx);
}

void diag_task(struct omt_fw *fw)
{
	uint8_t *ctrl = &fw->m.p13[P13_DIAG_CTRL - 128];

	if (*ctrl & 0x80) {
		fw->hal->irq_lock(fw->hal->ctx);
		*ctrl &= (uint8_t)~0x80; /* self-clearing */
		fw->hal->irq_unlock(fw->hal->ctx);
		reset_counters(fw);
		if (fw->diag_applied) {
			dsp_diag_discard(fw);
		}
		fw->diag_win_t = omt_now(fw);
	}
	if (fw->mstate != CMIS_MS_READY || !fw->dsp_ready || fw->dsp_rec_step) {
		fw->diag_applied = false; /* program again once the DSP is back */
	} else if ((fw->m.dirty & DIRTY_DIAG) || !fw->diag_applied) {
		fw->hal->irq_lock(fw->hal->ctx);
		fw->m.dirty &= ~DIRTY_DIAG;
		fw->hal->irq_unlock(fw->hal->ctx);
		restart_changed(fw);
		memcpy(fw->diag_shadow, &fw->m.p13[P13_CTRL_FIRST - 128], sizeof(fw->diag_shadow));
		fw->diag_applied = dsp_diag_program(fw) == 0;
		dsp_diag_discard(fw);
		fw->diag_t = fw->diag_win_t = omt_now(fw);
		omt_log(fw, 0x300, fw->m.p13[P13_MEDIA_CHK_EN - 128]);
	}
	if (fw->diag_applied && omt_now(fw) - fw->diag_t < CHK_SYNC_MS) {
		return; /* checkers still synchronising: no LOL verdict yet */
	}
	if (fw->diag_applied && omt_now(fw) - fw->diag_win_t >= 1000u) {
		fw->diag_win_t = omt_now(fw);
		dsp_diag_read(fw);
	}
	if (fw->diag_applied &&
	    (fw->m.p13[P13_HOST_CHK_EN - 128] | (fw->m.p13[P13_MEDIA_CHK_EN - 128] & 0x0F))) {
		int lock = dsp_diag_lock(fw);

		for (uint8_t h = 0; h < OMT_N_HOST_LANES; h++) {
			fw->hchk[h].lock = lock >= 0 && ((lock >> h) & 1);
		}
		for (uint8_t m = 0; m < OMT_N_MEDIA_LANES; m++) {
			fw->mchk[m].lock = lock >= 0 && ((lock >> (8 + m)) & 1);
		}
	}
	publish(fw);
}
