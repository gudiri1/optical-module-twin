/*
 * Data path state machines and application (AppSel) configuration.
 *
 * Supported applications (lower memory application descriptors):
 *   AppSel 1: 400GAUI-8 -> 400GBASE-DR4, one data path, host lanes 1-8,
 *             media lanes 1-4, DSP mode 1x400G
 *   AppSel 2: 100GAUI-2 -> 100GBASE-DR, four data paths (breakout),
 *             host lanes {1,2},{3,4},{5,6},{7,8} -> media lane 1..4,
 *             DSP mode 4x100G
 *
 * Per data path:
 *   Deactivated -> DPInit -> DPInitialized -> DPTxTurnOn -> DPActivated
 *   DPActivated -> DPTxTurnOff -> DPInitialized          (all lanes disabled)
 *   any (DPDeinit requested) -> DPDeinit -> Deactivated
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "omt/fw.h"

#define DPINIT_TIMEOUT_MS 900u
#define TXTURN_MS          20u
#define DPDEINIT_MS        50u

static uint8_t lane_appsel(const uint8_t *cfg, uint8_t lane)
{
	return cfg[lane] >> 4;
}

/* Build data path table from the active control set (Page 11h 206..213) */
static void build_dps(struct omt_fw *fw)
{
	const uint8_t *act = &fw->m.p11[P11_ACTIVE_DPCONFIG - 128];
	uint8_t app = lane_appsel(act, 0);

	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		fw->dp[i] = (struct omt_dp){ .state = CMIS_DP_DEACTIVATED };
	}
	if (app == 2) {
		for (uint8_t i = 0; i < 4; i++) {
			fw->dp[i].used = true;
			fw->dp[i].appsel = 2;
			fw->dp[i].host_mask = (uint8_t)(0x3u << (2 * i));
			fw->dp[i].media_mask = (uint8_t)(1u << i);
		}
		fw->dsp_mode = 1;
	} else {
		fw->dp[0].used = true;
		fw->dp[0].appsel = 1;
		fw->dp[0].host_mask = 0xFF;
		fw->dp[0].media_mask = 0x0F;
		fw->dsp_mode = 0;
	}
}

static void dp_set_state(struct omt_fw *fw, struct omt_dp *d, uint8_t st)
{
	d->state = st;
	d->t_enter = omt_now(fw);
	fw->hal->irq_lock(fw->hal->ctx);
	for (uint8_t l = 0; l < OMT_N_HOST_LANES; l++) {
		if (d->host_mask & (1u << l)) {
			cmis_set_dp_state(fw, l, st);
		}
	}
	fw->hal->irq_unlock(fw->hal->ctx);
}

static void dp_lanes_off(struct omt_fw *fw, const struct omt_dp *d)
{
	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		if (d->media_mask & (1u << l)) {
			tx_lane_enable(fw, l, false);
			tx_lane_squelch(fw, l, true);
			if (fw->dsp_ready) {
				(void)dsp_lane_enable(fw, l, false);
			}
		}
	}
}

void dpsm_reset(struct omt_fw *fw)
{
	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		if (fw->dp[i].used && fw->dp[i].state != CMIS_DP_DEACTIVATED) {
			dp_lanes_off(fw, &fw->dp[i]);
		}
	}
	build_dps(fw);
	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		if (fw->dp[i].used) {
			dp_set_state(fw, &fw->dp[i], CMIS_DP_DEACTIVATED);
		}
	}
}

static void set_cfg_status(struct omt_fw *fw, uint8_t lanes, uint8_t code)
{
	for (uint8_t l = 0; l < OMT_N_HOST_LANES; l++) {
		if (lanes & (1u << l)) {
			uint8_t *b = &fw->m.p11[P11_CFG_STATUS - 128 + l / 2];
			uint8_t sh = (l & 1) ? 4 : 0;

			*b = (uint8_t)((*b & ~(0xF << sh)) | (code << sh));
		}
	}
}

static uint8_t validate(struct omt_fw *fw, uint8_t lanes, const uint8_t *cfg)
{
	uint8_t app = 0;

	for (uint8_t l = 0; l < OMT_N_HOST_LANES; l++) {
		if (!(lanes & (1u << l))) {
			continue;
		}
		uint8_t a = lane_appsel(cfg, l);

		if (a != 1 && a != 2) {
			return CMIS_CFG_REJ_APPSEL;
		}
		if (app && a != app) {
			return CMIS_CFG_REJ_PARTIAL_DP;
		}
		app = a;
	}
	if (app == 1 && lanes != 0xFF) {
		return CMIS_CFG_REJ_PARTIAL_DP;
	}
	if (app == 2) {
		for (uint8_t p = 0; p < 4; p++) {
			uint8_t pair = (uint8_t)((lanes >> (2 * p)) & 0x3);

			if (pair == 0x1 || pair == 0x2) {
				return CMIS_CFG_REJ_PARTIAL_DP;
			}
		}
	}
	/* every currently configured data path touching these lanes - and,
	 * when the DSP mode changes, every data path - must be deactivated */
	uint8_t cur_app = lane_appsel(&fw->m.p11[P11_ACTIVE_DPCONFIG - 128], 0);

	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		const struct omt_dp *d = &fw->dp[i];

		if (!d->used || d->state == CMIS_DP_DEACTIVATED) {
			continue;
		}
		if ((d->host_mask & lanes) || cur_app != app) {
			return CMIS_CFG_REJ_IN_USE;
		}
	}
	return CMIS_CFG_SUCCESS;
}

void dpsm_apply_config(struct omt_fw *fw, uint8_t lanes)
{
	const uint8_t *staged = &fw->m.p10[P10_DPCONFIG - 128];
	uint8_t res;

	if (!lanes) {
		return;
	}
	set_cfg_status(fw, lanes, CMIS_CFG_IN_PROGRESS);
	res = validate(fw, lanes, staged);
	if (res == CMIS_CFG_SUCCESS) {
		uint8_t app = lane_appsel(staged, (uint8_t)__builtin_ctz(lanes));

		for (uint8_t l = 0; l < OMT_N_HOST_LANES; l++) {
			if (lanes & (1u << l)) {
				fw->m.p11[P11_ACTIVE_DPCONFIG - 128 + l] = staged[l];
			} else if (app == 1 ||
				   lane_appsel(&fw->m.p11[P11_ACTIVE_DPCONFIG - 128], l) != app) {
				/* mode change re-assigns the remaining lanes too */
				fw->m.p11[P11_ACTIVE_DPCONFIG - 128 + l] = (uint8_t)(app << 4);
			}
		}
		build_dps(fw);
		for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
			if (fw->dp[i].used) {
				dp_set_state(fw, &fw->dp[i], CMIS_DP_DEACTIVATED);
			}
		}
		serdes_apply_staged(fw, lanes);
		if (fw->dsp_ready) {
			(void)dsp_set_mode(fw, fw->dsp_mode);
		}
	}
	set_cfg_status(fw, lanes, res);
	omt_log(fw, 0x200 | res, lanes);
}

/* ApplyImmediate: signal-integrity controls take effect without a data
 * path re-init. A changed DPConfig needs DPInit, so it goes the
 * ApplyDPInit way (and is rejected while the data path is in use). */
static void dpsm_apply_immediate(struct omt_fw *fw, uint8_t lanes)
{
	const uint8_t *staged = &fw->m.p10[P10_DPCONFIG - 128];
	const uint8_t *active = &fw->m.p11[P11_ACTIVE_DPCONFIG - 128];

	if (!lanes) {
		return;
	}
	for (uint8_t l = 0; l < OMT_N_HOST_LANES; l++) {
		if ((lanes & (1u << l)) && staged[l] != active[l]) {
			dpsm_apply_config(fw, lanes);
			return;
		}
	}
	set_cfg_status(fw, lanes, CMIS_CFG_IN_PROGRESS);
	serdes_apply_staged(fw, lanes);
	set_cfg_status(fw, lanes, CMIS_CFG_SUCCESS);
	omt_log(fw, 0x210, lanes);
}

static bool dp_init_done(struct omt_fw *fw, const struct omt_dp *d)
{
	for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
		if ((d->media_mask & (1u << l)) && !fw->tx[l].fault && !fw->tx[l].converged) {
			return false;
		}
	}
	return true;
}

static void enter_deinit(struct omt_fw *fw, struct omt_dp *d)
{
	dp_lanes_off(fw, d);
	dp_set_state(fw, d, CMIS_DP_DEINIT);
}

static void dp_step(struct omt_fw *fw, struct omt_dp *d)
{
	uint8_t deinit = fw->m.p10[P10_DP_DEINIT - 128] & d->host_mask;
	uint8_t txdis = fw->m.p10[P10_TX_DISABLE - 128] & d->media_mask;
	uint32_t dt = omt_now(fw) - d->t_enter;

	switch (d->state) {
	case CMIS_DP_DEACTIVATED:
		if (!deinit && fw->mstate == CMIS_MS_READY && fw->dsp_ready) {
			for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
				if (d->media_mask & (1u << l)) {
					(void)dsp_lane_enable(fw, l, true);
					tx_lane_squelch(fw, l, true);
					tx_lane_enable(fw, l, true);
				}
			}
			serdes_program(fw, d->media_mask);
			dp_set_state(fw, d, CMIS_DP_INIT);
		}
		break;
	case CMIS_DP_INIT:
		if (deinit) {
			enter_deinit(fw, d);
		} else if (dp_init_done(fw, d) || dt > DPINIT_TIMEOUT_MS) {
			dp_set_state(fw, d, CMIS_DP_INITIALIZED);
		}
		break;
	case CMIS_DP_INITIALIZED:
		if (deinit) {
			enter_deinit(fw, d);
		} else if (txdis != d->media_mask) {
			for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
				if ((d->media_mask & ~txdis) & (1u << l)) {
					tx_lane_squelch(fw, l, false);
				}
			}
			dp_set_state(fw, d, CMIS_DP_TXTURNON);
		}
		break;
	case CMIS_DP_TXTURNON:
		if (dt >= TXTURN_MS) {
			dp_set_state(fw, d, CMIS_DP_ACTIVATED);
		}
		break;
	case CMIS_DP_ACTIVATED:
		if (deinit || txdis == d->media_mask) {
			for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
				if (d->media_mask & (1u << l)) {
					tx_lane_squelch(fw, l, true);
				}
			}
			dp_set_state(fw, d, CMIS_DP_TXTURNOFF);
		} else {
			/* lane-level output disable inside an active data path */
			for (uint8_t l = 0; l < OMT_N_MEDIA_LANES; l++) {
				if (!(d->media_mask & (1u << l))) {
					continue;
				}
				bool sq = (txdis & (1u << l)) != 0;

				if (fw->tx[l].squelched != sq) {
					tx_lane_squelch(fw, l, sq);
				}
			}
		}
		break;
	case CMIS_DP_TXTURNOFF:
		if (dt >= TXTURN_MS) {
			if (deinit) {
				enter_deinit(fw, d);
			} else {
				dp_set_state(fw, d, CMIS_DP_INITIALIZED);
			}
		}
		break;
	case CMIS_DP_DEINIT:
		if (dt >= DPDEINIT_MS) {
			dp_set_state(fw, d, CMIS_DP_DEACTIVATED);
		}
		break;
	default:
		break;
	}
}

void dpsm_task(struct omt_fw *fw)
{
	if (fw->m.dirty & DIRTY_APPLY_DPINIT) {
		uint8_t lanes;

		fw->hal->irq_lock(fw->hal->ctx);
		lanes = fw->m.p10[P10_APPLY_DPINIT - 128];
		fw->m.p10[P10_APPLY_DPINIT - 128] = 0; /* self-clearing trigger */
		fw->m.dirty &= ~(DIRTY_APPLY_DPINIT | DIRTY_LANECTRL);
		fw->hal->irq_unlock(fw->hal->ctx);
		dpsm_apply_config(fw, lanes);
	}
	if (fw->m.dirty & DIRTY_APPLY_IMMED) {
		uint8_t lanes;

		fw->hal->irq_lock(fw->hal->ctx);
		lanes = fw->m.p10[P10_APPLY_IMMEDIATE - 128];
		fw->m.p10[P10_APPLY_IMMEDIATE - 128] = 0; /* self-clearing trigger */
		fw->m.dirty &= ~DIRTY_APPLY_IMMED;
		fw->hal->irq_unlock(fw->hal->ctx);
		dpsm_apply_immediate(fw, lanes);
	}
	fw->m.dirty &= ~DIRTY_LANECTRL;
	if (fw->mstate != CMIS_MS_READY) {
		return;
	}
	for (int i = 0; i < OMT_N_MEDIA_LANES; i++) {
		if (fw->dp[i].used) {
			dp_step(fw, &fw->dp[i]);
		}
	}
}
