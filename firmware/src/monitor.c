/*
 * Digital diagnostic monitoring: sampling, calibration, CMIS encoding,
 * threshold comparison with hysteresis, latched flags and protection.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <string.h>

#include "omt/fw.h"

#define ADC_VREF 2.5f
#define ADC_FS   65535.0f
#define TEC_I_MAX_A 1.5f

#define T_SHUTDOWN_C        85.0f
#define LASER_T_SHUTDOWN_C  72.0f
#define VCC_BROWNOUT_V      2.90f

static float adc_v(struct omt_fw *fw, uint8_t ch, uint16_t *raw_out)
{
	uint16_t raw = 0;

	if (fw->hal->adc_read(fw->hal->ctx, ch, &raw) < 0) {
		raw = 0;
	}
	if (raw_out) {
		*raw_out = raw;
	}
	return (float)raw * ADC_VREF / ADC_FS;
}

uint16_t mon_uw_to_cmis(float uw)
{
	float v = uw * 10.0f + 0.5f;

	if (v < 0.0f) {
		return 0;
	}
	return v > 65535.0f ? 65535 : (uint16_t)v;
}

int16_t mon_c_to_cmis(float c)
{
	float v = c * 256.0f;

	if (v > 32767.0f) {
		return 32767;
	}
	if (v < -32768.0f) {
		return -32768;
	}
	return (int16_t)lrintf(v);
}

static uint16_t dbm_to_cmis(float dbm)
{
	return mon_uw_to_cmis(1000.0f * powf(10.0f, dbm / 10.0f));
}

/* Factory thresholds, page 02h. Single source of truth for the alarm engine. */
void mon_load_thresholds(struct omt_fw *fw)
{
	uint8_t *p = fw->m.p02;

#define T4(off, a, b, c, d)                                                \
	do {                                                               \
		cmis_set_u16(&p[(off) - 128 + 0], (uint16_t)(a));          \
		cmis_set_u16(&p[(off) - 128 + 2], (uint16_t)(b));          \
		cmis_set_u16(&p[(off) - 128 + 4], (uint16_t)(c));          \
		cmis_set_u16(&p[(off) - 128 + 6], (uint16_t)(d));          \
	} while (0)

	T4(P02_TEMP, mon_c_to_cmis(80), mon_c_to_cmis(-10), mon_c_to_cmis(75), mon_c_to_cmis(-5));
	T4(P02_VCC, 36300, 29700, 34650, 31350);
	T4(P02_AUX1, mon_c_to_cmis(60), mon_c_to_cmis(40), mon_c_to_cmis(55), mon_c_to_cmis(45));
	T4(P02_AUX2, 31129, (uint16_t)(int16_t)-31129, 27852, (uint16_t)(int16_t)-27852);
	T4(P02_TXPWR, dbm_to_cmis(4.0f), dbm_to_cmis(-6.0f), dbm_to_cmis(3.0f), dbm_to_cmis(-5.0f));
	/* Bias alarm must sit below the APC clamp (105 mA, tx.c) or it can never
	 * fire on an end-of-life laser (docs/findings.md F-002). */
	T4(P02_TXBIAS, 50000, 10000, 45000, 12500); /* 100/20/90/25 mA, 2 uA LSB */
	T4(P02_RXPWR, dbm_to_cmis(5.0f), dbm_to_cmis(-12.0f), dbm_to_cmis(4.0f), dbm_to_cmis(-10.0f));
#undef T4
}

static void read_thr(const struct omt_fw *fw, uint8_t off, bool is_signed, int32_t hyst,
		     struct omt_threshold *t)
{
	const uint8_t *p = &fw->m.p02[off - 128];
	int32_t v[4];

	for (int i = 0; i < 4; i++) {
		uint16_t u = cmis_get_u16(&p[2 * i]);

		v[i] = is_signed ? (int32_t)(int16_t)u : (int32_t)u;
	}
	t->ha = v[0];
	t->la = v[1];
	t->hw = v[2];
	t->lw = v[3];
	t->hyst = hyst;
}

/*
 * Returns condition bits: b0 high alarm, b1 low alarm, b2 high warning,
 * b3 low warning. A condition that is active stays active until the value
 * has moved back by `hyst`, which prevents flag chatter around a limit.
 */
uint8_t alarm_eval(const struct omt_threshold *t, int32_t v, uint8_t prev)
{
	uint8_t c = 0;

	if (v > t->ha || ((prev & 0x1) && v > t->ha - t->hyst)) {
		c |= 0x1;
	}
	if (v < t->la || ((prev & 0x2) && v < t->la + t->hyst)) {
		c |= 0x2;
	}
	if (v > t->hw || ((prev & 0x4) && v > t->hw - t->hyst)) {
		c |= 0x4;
	}
	if (v < t->lw || ((prev & 0x8) && v < t->lw + t->hyst)) {
		c |= 0x8;
	}
	return c;
}

static int32_t pct_hyst(const struct omt_threshold *t)
{
	/* ~0.2 dB on linear power thresholds */
	return (t->lw > 0 ? t->lw : 1) * 5 / 100 + 1;
}

static void lane_alarms(struct omt_fw *fw, uint8_t first_flag, uint8_t thr_off,
			const uint16_t *val, uint8_t active_mask, int32_t fixed_hyst)
{
	struct omt_threshold t;
	uint8_t bits[4] = { 0 };

	read_thr(fw, thr_off, false, 0, &t);
	t.hyst = fixed_hyst ? fixed_hyst : pct_hyst(&t);
	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		uint8_t prev = 0;

		for (int k = 0; k < 4; k++) {
			if (fw->m.cond_lane[first_flag + k - P11_FLAG_FIRST] & (1u << l)) {
				prev |= (uint8_t)(1u << k);
			}
		}
		uint8_t c = (active_mask & (1u << l)) ? alarm_eval(&t, val[l], prev) : 0;

		for (int k = 0; k < 4; k++) {
			if (c & (1u << k)) {
				bits[k] |= (uint8_t)(1u << l);
			}
		}
	}
	for (int k = 0; k < 4; k++) {
		cmis_latch_lane(fw, (uint8_t)(first_flag + k), bits[k]);
	}
}

static uint8_t rx_active_mask(const struct omt_fw *fw)
{
	uint8_t mask = 0;

	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		const struct omt_dp *d = &fw->dp[i];

		if (d->used && (d->state == CMIS_DP_ACTIVATED || d->state == CMIS_DP_INITIALIZED ||
				d->state == CMIS_DP_TXTURNON || d->state == CMIS_DP_TXTURNOFF)) {
			mask |= d->media_mask;
		}
	}
	return mask;
}

static void update_vendor_pages(struct omt_fw *fw)
{
	uint8_t *b0 = fw->m.pb0;
	uint8_t apc = 0;

	cmis_set_u32(&b0[PB0_UPTIME_S - 128], (omt_now(fw) - fw->boot_ms) / 1000u);
	cmis_set_u16(&b0[PB0_RESET_COUNT - 128], fw->boot.reset_count);
	b0[PB0_RESET_REASON - 128] = fw->boot.last_reset_reason;
	b0[PB0_BOOT_BANK - 128] = fw->running_bank;
	cmis_set_u16(&b0[PB0_FAULTLOG_COUNT - 128], fw->flog_count);
	cmis_set_u16(&b0[PB0_I2C_BAD_PAGE - 128], fw->m.bad_page);
	cmis_set_u16(&b0[PB0_I2C_RO_WRITE - 128], fw->m.ro_write);
	cmis_set_u16(&b0[PB0_DSP_RECOVERIES - 128], fw->dsp_recoveries);
	for (int l = 0; l < 4; l++) {
		float snr = fw->rx[l].snr_db * 100.0f;

		cmis_set_f32(&b0[PB0_PREFEC_BER - 128 + 4 * l], fw->rx[l].prefec_ber);
		cmis_set_u16(&b0[PB0_SNR - 128 + 2 * l], (uint16_t)(snr < 0 ? 0 : snr));
		cmis_set_u32(&b0[PB0_FEC_CORR - 128 + 4 * l], fw->rx[l].fec_corr);
		cmis_set_u32(&b0[PB0_FEC_UNCORR - 128 + 4 * l], fw->rx[l].fec_uncorr);
		if (fw->tx[l].converged) {
			apc |= (uint8_t)(1u << l);
		}
	}
	cmis_set_u16(&b0[PB0_TEC_SETPOINT - 128], (uint16_t)mon_c_to_cmis(fw->tun.tec_setpoint_c));
	b0[PB0_APC_STATE - 128] = apc;
	cmis_set_u16(&b0[PB0_DSP_TEMP - 128], (uint16_t)fw->dsp_temp_dc);
	cmis_set_u16(&b0[PB0_LAST_FAULT - 128], fw->last_fault);

	uint8_t *b1 = fw->m.pb1;

	cmis_set_u16(&b1[PB1_RAW_TEMP - 128], fw->raw_temp);
	cmis_set_u16(&b1[PB1_RAW_VCC - 128], fw->raw_vcc);
	for (int l = 0; l < 4; l++) {
		cmis_set_u16(&b1[PB1_RAW_RSSI - 128 + 2 * l], fw->rx[l].rssi_raw);
		cmis_set_u16(&b1[PB1_RAW_MPD - 128 + 2 * l], fw->tx[l].mpd_raw);
	}
}

static void protection(struct omt_fw *fw, uint16_t dt_ms)
{
	if (fw->mstate == CMIS_MS_FAULT) {
		return;
	}
	if (fw->temp_c > T_SHUTDOWN_C) {
		msm_enter_fault(fw, FC_MODULE_OVERTEMP, EV_MODULE_OVERTEMP,
				(uint16_t)(int16_t)fw->temp_c);
	} else if (fw->tec_enabled && fw->laser_temp_c > LASER_T_SHUTDOWN_C) {
		msm_enter_fault(fw, FC_LASER_OVERTEMP, EV_LASER_OVERTEMP,
				(uint16_t)(int16_t)fw->laser_temp_c);
	}
	/* brown-out: Vcc low for > 100 ms while the power-hungry blocks run */
	if (fw->vcc_v < VCC_BROWNOUT_V && fw->mstate == CMIS_MS_READY) {
		fw->brownout_ms = (uint16_t)(fw->brownout_ms + dt_ms);
		if (fw->brownout_ms > 100) {
			msm_enter_fault(fw, FC_VCC, EV_VCC_UNDERVOLT, (uint16_t)(fw->vcc_v * 1000.0f));
		}
	} else {
		fw->brownout_ms = 0;
	}
}

/* Boot-time temperature so early fault-log entries carry a real value */
void mon_prime_temp(struct omt_fw *fw)
{
	float v = adc_v(fw, OMT_ADC_TEMP, &fw->raw_temp);

	fw->temp_c = (v - 0.5f) / 0.01f + fw->cal.temp_offset_c;
}

void mon_task(struct omt_fw *fw)
{
	struct omt_cmis *m = &fw->m;
	const struct omt_calib *c = &fw->cal;
	struct omt_threshold t;
	float v;

	/* --- sample & convert (first-order IIR; first sample primes it) --- */
	float a = fw->mon_primed ? 0.25f : 1.0f;

	v = adc_v(fw, OMT_ADC_TEMP, &fw->raw_temp);
	fw->temp_c += a * (((v - 0.5f) / 0.01f + c->temp_offset_c) - fw->temp_c);
	v = adc_v(fw, OMT_ADC_VCC, &fw->raw_vcc);
	fw->vcc_v += a * (v * 2.0f * c->vcc_gain - fw->vcc_v);
	v = adc_v(fw, OMT_ADC_LASER_TEMP, NULL);
	fw->laser_temp_c += (fw->mon_primed ? 0.5f : 1.0f) *
			    (((v - 0.5f) / 0.01f + c->laser_temp_offset_c) - fw->laser_temp_c);
	fw->mon_primed = true;
	v = adc_v(fw, OMT_ADC_TEC_I, NULL);
	float tec_i = (v - 1.25f) / 0.5f;

	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		struct omt_rx_lane *r = &fw->rx[l];
		uint16_t raw;

		(void)adc_v(fw, (uint8_t)(OMT_ADC_RSSI0 + l), &raw);
		r->rssi_raw = raw;
		float uw = ((float)raw - c->rssi_offset[l]) * c->rssi_slope[l];

		r->rx_uw = uw < 0.0f ? 0.0f : uw;
		float dbm = 10.0f * log10f(r->rx_uw / 1000.0f + 1e-9f);

		if (!r->los && dbm < fw->tun.los_assert_dbm) {
			r->los = true;
		} else if (r->los && dbm > fw->tun.los_deassert_dbm) {
			r->los = false;
		}
	}

	/* --- publish to CMIS memory atomically w.r.t. the I2C ISR --- */
	uint16_t txp[4], bias[4], rxp[4];
	uint8_t tx_mask = 0, rx_mask = rx_active_mask(fw);
	uint8_t los = 0, lol = 0, txf = 0, txlol = 0, out_tx = 0, out_rx = 0;

	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		const struct omt_tx_lane *x = &fw->tx[l];

		txp[l] = x->enabled ? mon_uw_to_cmis(x->tx_uw) : 0;
		bias[l] = x->enabled ? (uint16_t)fminf(65535.0f, x->bias_meas_ma * 500.0f) : 0;
		rxp[l] = mon_uw_to_cmis(fw->rx[l].rx_uw);
		/* Arm lane alarms once the lane first converged after enable and keep
		 * them armed while the APC loses control - gating on "converged"
		 * hid the alarms of a degrading laser (docs/findings.md F-003). */
		if (x->enabled && !x->squelched && x->armed) {
			tx_mask |= (uint8_t)(1u << l);
		}
		if (x->enabled && !x->squelched && x->converged) {
			out_tx |= (uint8_t)(1u << l);
		}
		if (x->fault) {
			txf |= (uint8_t)(1u << l);
		}
		if (rx_mask & (1u << l)) {
			if (fw->rx[l].los) {
				los |= (uint8_t)(1u << l);
			}
			if (fw->rx[l].lol) {
				lol |= (uint8_t)(1u << l);
			} else if (!fw->rx[l].los) {
				out_rx |= (uint8_t)(1u << l);
			}
		}
	}

	fw->hal->irq_lock(fw->hal->ctx);
	cmis_set_u16(&m->lower[CMIS_MON_TEMP], (uint16_t)mon_c_to_cmis(fw->temp_c));
	cmis_set_u16(&m->lower[CMIS_MON_VCC], (uint16_t)fminf(65535.0f, fw->vcc_v * 10000.0f));
	cmis_set_u16(&m->lower[CMIS_MON_AUX1], (uint16_t)mon_c_to_cmis(fw->laser_temp_c));
	cmis_set_u16(&m->lower[CMIS_MON_AUX2],
		     (uint16_t)(int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, tec_i / TEC_I_MAX_A)) * 32767.0f));
	for (int l = 0; l < 4; l++) {
		cmis_set_u16(&m->p11[P11_MON_TXPWR - 128 + 2 * l], txp[l]);
		cmis_set_u16(&m->p11[P11_MON_BIAS - 128 + 2 * l], bias[l]);
		cmis_set_u16(&m->p11[P11_MON_RXPWR - 128 + 2 * l], rxp[l]);
	}
	m->p11[P11_OUT_STATUS_TX - 128] = out_tx;
	m->p11[P11_OUT_STATUS_RX - 128] = out_rx;

	/* --- module-level alarms --- */
	uint8_t prev = m->cond_module[CMIS_FLAG_TEMP_VCC - CMIS_FLAG_MODULE];

	read_thr(fw, P02_TEMP, true, 256, &t);
	uint8_t ct = alarm_eval(&t, (int16_t)cmis_get_u16(&m->lower[CMIS_MON_TEMP]), prev & 0xF);

	read_thr(fw, P02_VCC, false, 100, &t);
	uint8_t cv = alarm_eval(&t, cmis_get_u16(&m->lower[CMIS_MON_VCC]), prev >> 4);

	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, (uint8_t)(ct | (cv << 4)));

	prev = m->cond_module[CMIS_FLAG_AUX12 - CMIS_FLAG_MODULE];
	uint8_t c1 = 0, c2 = 0;

	/* Laser plate temperature and TEC drive are only meaningful once the
	 * TEC loop had its power-up settling time (ModuleReady); evaluating them
	 * earlier latched spurious low alarms on every bring-up (found by
	 * `omt lint`, see docs/findings.md F-001). */
	if (fw->tec_enabled && fw->mstate == CMIS_MS_READY) {
		read_thr(fw, P02_AUX1, true, 128, &t);
		c1 = alarm_eval(&t, (int16_t)cmis_get_u16(&m->lower[CMIS_MON_AUX1]), prev & 0xF);
		read_thr(fw, P02_AUX2, true, 655, &t);
		c2 = alarm_eval(&t, (int16_t)cmis_get_u16(&m->lower[CMIS_MON_AUX2]), prev >> 4);
	}
	cmis_latch_module(fw, CMIS_FLAG_AUX12, (uint8_t)(c1 | (c2 << 4)));

	/* --- lane alarms & status flags --- */
	lane_alarms(fw, P11_FLAG_TXPWR_HA, P02_TXPWR, txp, tx_mask, 0);
	lane_alarms(fw, P11_FLAG_BIAS_HA, P02_TXBIAS, bias, tx_mask, 500);
	lane_alarms(fw, P11_FLAG_RXPWR_HA, P02_RXPWR, rxp, rx_mask, 0);
	cmis_latch_lane(fw, P11_FLAG_TX_FAULT, txf);
	cmis_latch_lane(fw, P11_FLAG_RX_LOS, los);
	cmis_latch_lane(fw, P11_FLAG_RX_LOL, lol);
	for (int l = 0; l < 4; l++) {
		if ((rx_mask & (1u << l)) && fw->rx[l].host_lol) {
			txlol |= (uint8_t)(1u << l);
		}
	}
	cmis_latch_lane(fw, P11_FLAG_TX_LOL, txlol);
	update_vendor_pages(fw);
	cmis_update_intl(fw);
	fw->hal->irq_unlock(fw->hal->ctx);

	protection(fw, 20);
}
