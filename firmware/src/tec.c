/*
 * Thermo-electric cooler loop: keeps the laser plate at the setpoint so the
 * emitted wavelength stays inside the advertised tolerance over case
 * temperature. PI controller with conditional-integration anti-windup.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>

#include "omt/fw.h"

#define TEC_PERIOD_S 0.010f

void tec_enable(struct omt_fw *fw, bool on)
{
	fw->tec_enabled = on;
	fw->tec_integ = 0.0f;
	fw->tec_drive = 0.0f;
	fw->tec_sat_ms = 0;
	fw->tec_alarm_logged = false;
	fw->hal->dac_write(fw->hal->ctx, OMT_DAC_TEC, 0x8000);
	fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_TEC_EN, on ? 1 : 0);
}

bool tec_locked(const struct omt_fw *fw)
{
	return fw->tec_enabled && fabsf(fw->laser_temp_c - fw->tun.tec_setpoint_c) < 1.0f;
}

void tec_task(struct omt_fw *fw)
{
	if (!fw->tec_enabled) {
		return;
	}
	float e = fw->laser_temp_c - fw->tun.tec_setpoint_c; /* + : too hot -> cool */
	float u_unsat = fw->tun.tec_kp * e + fw->tec_integ;
	bool sat = fabsf(u_unsat) >= 1.0f;

	/* integrate only if it does not push further into saturation */
	if (!sat || (u_unsat > 0) != (e > 0)) {
		fw->tec_integ += fw->tun.tec_ki * e * TEC_PERIOD_S;
		fw->tec_integ = fmaxf(-1.0f, fminf(1.0f, fw->tec_integ));
	}
	float u = fmaxf(-1.0f, fminf(1.0f, u_unsat));

	fw->tec_drive = u;
	fw->hal->dac_write(fw->hal->ctx, OMT_DAC_TEC, (uint16_t)lrintf(32768.0f + u * 32767.0f));

	if (sat && fabsf(e) > 3.0f) {
		fw->tec_sat_ms += 10;
		if (fw->tec_sat_ms > 10000 && !fw->tec_alarm_logged) {
			fw->tec_alarm_logged = true;
			omt_event(fw, EV_TEC_SATURATED, (uint16_t)(int16_t)lrintf(e * 10.0f));
		}
	} else {
		fw->tec_sat_ms = 0;
	}
}
