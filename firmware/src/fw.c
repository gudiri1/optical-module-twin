/*
 * Firmware entry: initialization order, cooperative scheduler, watchdog
 * policy, event logging and housekeeping.
 *
 * On an RTOS port each task becomes a periodic work item / thread with the
 * same period; the bodies are run-to-completion and never block.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "omt/fw.h"

uint32_t omt_now(const struct omt_fw *fw)
{
	return fw->hal->millis(fw->hal->ctx);
}

void omt_log(struct omt_fw *fw, uint16_t code, uint16_t arg)
{
	struct omt_log_entry *e = &fw->log[fw->log_head];

	e->t_ms = omt_now(fw);
	e->code = code;
	e->arg = arg;
	fw->log_head = (uint8_t)((fw->log_head + 1) % OMT_LOG_LEN);
}

void omt_event(struct omt_fw *fw, uint16_t code, uint16_t arg)
{
	omt_log(fw, code, arg);
	flog_append(fw, code, arg);
	if (code != EV_BOOT && code != EV_BOOT_CONFIRMED && code != EV_FW_DOWNLOADED &&
	    code != EV_FW_COMMITTED && code != EV_CAL_COMMITTED) {
		fw->last_fault = code;
	}
}

void omt_printf(struct omt_fw *fw, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		fw->hal->uart_tx(fw->hal->ctx, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
	}
}

/* ---------------- calibration access over vendor page B1h ------------- */

static void cal_publish(struct omt_fw *fw)
{
	uint8_t *p = &fw->m.pb1[PB1_COEF - 128];
	const struct omt_calib *c = &fw->cal;

	cmis_set_f32(p + 0, c->temp_offset_c);
	cmis_set_f32(p + 4, c->vcc_gain);
	for (int l = 0; l < 4; l++) {
		cmis_set_f32(p + 8 + 4 * l, c->rssi_slope[l]);
		cmis_set_f32(p + 24 + 4 * l, c->rssi_offset[l]);
		cmis_set_f32(p + 40 + 4 * l, c->mpd_slope[l]);
	}
	cmis_set_f32(p + 56, c->laser_temp_offset_c);
	cmis_set_f32(p + 60, c->tx_target_dbm);
	memcpy(p + 64, c->serial, 16);
	memcpy(p + 80, c->date_code, 8);
}

static void cal_parse(struct omt_fw *fw, struct omt_calib *c)
{
	const uint8_t *p = &fw->m.pb1[PB1_COEF - 128];

	*c = fw->cal;
	c->temp_offset_c = cmis_get_f32(p + 0);
	c->vcc_gain = cmis_get_f32(p + 4);
	for (int l = 0; l < 4; l++) {
		c->rssi_slope[l] = cmis_get_f32(p + 8 + 4 * l);
		c->rssi_offset[l] = cmis_get_f32(p + 24 + 4 * l);
		c->mpd_slope[l] = cmis_get_f32(p + 40 + 4 * l);
	}
	c->laser_temp_offset_c = cmis_get_f32(p + 56);
	c->tx_target_dbm = cmis_get_f32(p + 60);
	memcpy(c->serial, p + 64, 16);
	memcpy(c->date_code, p + 80, 8);
}

static bool cal_sane(const struct omt_calib *c)
{
	if (!(c->vcc_gain > 0.9f && c->vcc_gain < 1.1f) ||
	    !(c->temp_offset_c > -10.0f && c->temp_offset_c < 10.0f) ||
	    !(c->tx_target_dbm > -3.0f && c->tx_target_dbm < 4.0f)) {
		return false;
	}
	for (int l = 0; l < 4; l++) {
		if (!(c->rssi_slope[l] > 0.0f && c->rssi_slope[l] < 1.0f) ||
		    !(c->mpd_slope[l] > 0.2f && c->mpd_slope[l] < 5.0f)) {
			return false;
		}
	}
	return true;
}

static void refresh_identity(struct omt_fw *fw)
{
	char sn[17] = { 0 }, dc[9] = { 0 };

	memcpy(sn, fw->cal.serial, 16);
	memcpy(dc, fw->cal.date_code, 8);
	memset(&fw->m.p00[P00_VENDOR_SN - 128], ' ', 16);
	memcpy(&fw->m.p00[P00_VENDOR_SN - 128], sn, strlen(sn));
	memset(&fw->m.p00[P00_DATE_CODE - 128], ' ', 8);
	memcpy(&fw->m.p00[P00_DATE_CODE - 128], dc, strlen(dc));
	cmis_update_checksums(fw);
}

static void cal_command(struct omt_fw *fw)
{
	uint8_t cmd = fw->m.pb1[PB1_COMMAND - 128];
	uint8_t res = 0x01;

	if (cmd == 0xA5) {
		struct omt_calib c;

		cal_parse(fw, &c);
		if (!cal_sane(&c)) {
			res = 0xFE;
		} else {
			fw->cal = c;
			if (nvm_save_calibration(fw) == 0) {
				omt_event(fw, EV_CAL_COMMITTED, 0);
				refresh_identity(fw);
				for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
					fw->tx[l].target_uw = 1000.0f * powf(10.0f, c.tx_target_dbm / 10.0f);
				}
			} else {
				res = 0xFF;
			}
		}
	} else if (cmd == 0xC3) {
		nvm_default_calibration(&fw->cal);
	} else if (cmd != 0x5A) {
		res = 0xFD;
	}
	cal_publish(fw);
	fw->m.pb1[PB1_COMMAND - 128] = res;
}

void housekeeping_task(struct omt_fw *fw)
{
	uint32_t now = omt_now(fw);

	if ((fw->m.dirty & DIRTY_USER_EEPROM) && now - fw->m.user_eeprom_dirty_ms > 500) {
		/* coalesce bursts of host writes into one erase/program cycle */
		fw->m.dirty &= ~DIRTY_USER_EEPROM;
		nvm_save_user_eeprom(fw);
	}
	if (fw->m.dirty & DIRTY_CAL_CMD) {
		fw->m.dirty &= ~DIRTY_CAL_CMD;
		cal_command(fw);
	}
	img_confirm_task(fw);
	if (fw->pending_reset_t && (int32_t)(now - fw->pending_reset_t) >= 0) {
		fw->pending_reset_t = 0;
		fw->hal->system_reset(fw->hal->ctx, OMT_RST_SOFTWARE);
	}
}

/* ---------------------------- scheduler ------------------------------- */

static void add_task(struct omt_fw *fw, const char *name, uint16_t period,
		     void (*fn)(struct omt_fw *))
{
	struct omt_task *t = &fw->tasks[fw->n_tasks++];

	t->name = name;
	t->period_ms = period;
	t->fn = fn;
	t->last = omt_now(fw);
}

void omt_fw_init(struct omt_fw *fw, const struct omt_hal *hal)
{
	memset(fw, 0, sizeof(*fw));
	fw->hal = hal;
	fw->boot_ms = omt_now(fw);

	hal->gpio_set(hal->ctx, OMT_PIN_TX_PWR, 0);
	hal->gpio_set(hal->ctx, OMT_PIN_DSP_PWR, 0);
	hal->gpio_set(hal->ctx, OMT_PIN_DSP_RSTN, 0);
	hal->gpio_set(hal->ctx, OMT_PIN_TEC_EN, 0);

	flog_init(fw);
	nvm_default_calibration(&fw->cal);
	mon_prime_temp(fw);          /* uncalibrated, for events logged during load */
	nvm_load_calibration(fw);
	mon_prime_temp(fw);
	img_boot(fw);
	cmis_mem_init(fw);
	img_publish_versions(fw);
	cal_publish(fw);
	fw->mstate = CMIS_MS_LOWPWR;
	fw->mstate_t = omt_now(fw);
	dpsm_reset(fw);
	for (int l = 0; l < OMT_N_MEDIA_LANES; l++) {
		fw->rx[l].lol = true;
	}

	add_task(fw, "cdb", 1, cdb_task);
	add_task(fw, "apc", 5, apc_task);
	add_task(fw, "msm", 10, msm_task);
	add_task(fw, "dpsm", 10, dpsm_task);
	add_task(fw, "tec", 10, tec_task);
	add_task(fw, "mon", 20, mon_task);
	add_task(fw, "dsp", 100, dsp_task);
	add_task(fw, "house", 50, housekeeping_task);
	add_task(fw, "console", 10, console_task);
	mon_task(fw); /* prime monitors before the host can read them */
}

void omt_fw_tick(struct omt_fw *fw)
{
	uint32_t now = omt_now(fw);

	if (fw->hung) {
		return; /* image test hook: a hung main loop starves the watchdog */
	}
	for (uint8_t i = 0; i < fw->n_tasks; i++) {
		struct omt_task *t = &fw->tasks[i];

		if (now - t->last >= t->period_ms) {
			t->last += t->period_ms;
			if (now - t->last >= t->period_ms) {
				t->last = now; /* overrun: do not burst */
			}
			t->fn(fw);
			t->runs++;
		}
	}
	fw->hal->wdt_kick(fw->hal->ctx);
}
