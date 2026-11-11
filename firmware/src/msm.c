/*
 * CMIS module state machine and power sequencing.
 *
 *   reset -> MgmtInit -> ModuleLowPwr <-> ModulePwrUp -> ModuleReady
 *                              ^                           |
 *                              +------- ModulePwrDn <------+
 *   any state --(protection)--> ModuleFault (exit only by reset)
 *
 * Power-up sequence (board specific, see docs/bringup.md):
 *   0  laser driver rail + SPI probe, DSP rail on with reset asserted, TEC on
 *   1  +20 ms  release DSP reset
 *   2  +30 ms  probe DSP over MDIO, start DSP init
 *   3  DSP ready and TEC locked (TEC lock waited for at most 4 s)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "omt/fw.h"

#define DSP_INIT_TIMEOUT_MS  500u
#define TEC_LOCK_TIMEOUT_MS 4000u
#define PWRDN_MS              50u

static void set_state(struct omt_fw *fw, uint8_t st)
{
	fw->mstate = st;
	fw->mstate_t = omt_now(fw);
	fw->pwrup_step = 0;
	fw->hal->irq_lock(fw->hal->ctx);
	cmis_set_module_state(fw, st);
	fw->hal->irq_unlock(fw->hal->ctx);
	omt_log(fw, 0x100 | st, 0);
}

static void power_off_all(struct omt_fw *fw)
{
	dpsm_reset(fw);
	tx_shutdown(fw);
	dsp_power_down(fw);
	tec_enable(fw, false);
}

void msm_enter_fault(struct omt_fw *fw, uint8_t cause, uint16_t ev, uint16_t arg)
{
	if (fw->mstate == CMIS_MS_FAULT) {
		return;
	}
	omt_event(fw, ev, arg);
	power_off_all(fw);
	fw->fault_cause = cause;
	fw->m.lower[CMIS_FAULT_CAUSE] = cause;
	set_state(fw, CMIS_MS_FAULT);
}

static bool lowpwr_requested(struct omt_fw *fw)
{
	uint8_t ctrl = fw->m.lower[CMIS_MOD_CTRL];

	if (ctrl & CMIS_CTRL_LOWPWR_SW) {
		return true;
	}
	return (ctrl & CMIS_CTRL_LOWPWR_ALLOW_HW) &&
	       fw->hal->gpio_get(fw->hal->ctx, OMT_PIN_LPMODE);
}

static void pwrup_sequence(struct omt_fw *fw)
{
	uint32_t now = omt_now(fw);
	uint32_t dt = now - fw->mstate_t;

	switch (fw->pwrup_step) {
	case 0:
		if (tx_init(fw) < 0) {
			msm_enter_fault(fw, FC_LASER_DRIVER, EV_LASER_DRV_FAULT, 0xFF);
			return;
		}
		dsp_power_up(fw);
		tec_enable(fw, true);
		/* sequence delays count from the rails, not from entering PwrUp:
		 * step 0 runs one msm period after the state change, and timing
		 * from mstate_t cut the DSP reset hold to ~10 ms (findings F-009) */
		fw->pwrup_t0 = now;
		fw->pwrup_step = 1;
		break;
	case 1:
		if (now - fw->pwrup_t0 >= 20) {
			dsp_release_reset(fw);
			fw->pwrup_step = 2;
		}
		break;
	case 2:
		if (now - fw->pwrup_t0 >= 50) {
			if (dsp_start(fw) < 0) {
				msm_enter_fault(fw, FC_DSP, EV_DSP_NO_RESPONSE, 0);
				return;
			}
			fw->pwrup_step = 3;
		}
		break;
	case 3:
		if (!fw->dsp_ready && !dsp_poll_ready(fw)) {
			if (dt > 50 + DSP_INIT_TIMEOUT_MS) {
				msm_enter_fault(fw, FC_DSP, EV_DSP_INIT_TIMEOUT, 0);
			}
			return;
		}
		if (tec_locked(fw) || dt > TEC_LOCK_TIMEOUT_MS) {
			fw->ber_window_t = now;
			set_state(fw, CMIS_MS_READY);
		}
		break;
	default:
		break;
	}
}

void msm_task(struct omt_fw *fw)
{
	if (fw->m.dirty & DIRTY_MODCTRL) {
		fw->m.dirty &= ~DIRTY_MODCTRL;
		if (fw->m.lower[CMIS_MOD_CTRL] & CMIS_CTRL_SW_RESET) {
			omt_event(fw, EV_SW_RESET, 0);
			fw->hal->system_reset(fw->hal->ctx, OMT_RST_SOFTWARE);
			return;
		}
	}
	fw->lowpwr_req = lowpwr_requested(fw);

	switch (fw->mstate) {
	case CMIS_MS_LOWPWR:
		if (!fw->lowpwr_req) {
			set_state(fw, CMIS_MS_PWRUP);
		}
		break;
	case CMIS_MS_PWRUP:
		if (fw->lowpwr_req) {
			power_off_all(fw);
			set_state(fw, CMIS_MS_PWRDN);
		} else {
			pwrup_sequence(fw);
		}
		break;
	case CMIS_MS_READY:
		if (fw->lowpwr_req) {
			power_off_all(fw);
			set_state(fw, CMIS_MS_PWRDN);
		}
		break;
	case CMIS_MS_PWRDN:
		if (omt_now(fw) - fw->mstate_t >= PWRDN_MS) {
			set_state(fw, CMIS_MS_LOWPWR);
		}
		break;
	case CMIS_MS_FAULT:
	default:
		break;
	}
}
