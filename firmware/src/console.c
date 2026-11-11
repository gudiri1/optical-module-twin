/*
 * UART debug console for bring-up and failure analysis.
 * Lines are collected by omt_console_rx (ISR side) and executed in the
 * console task. Register peek/poke goes through the same drivers the
 * firmware uses, so the console never bypasses retries or locking.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "omt/fw.h"

static const char *ms_name(uint8_t s)
{
	static const char *const n[] = { "MgmtInit", "LowPwr", "PwrUp", "Ready", "PwrDn", "Fault" };

	return s < 6 ? n[s] : "?";
}

static const char *dp_name(uint8_t s)
{
	static const char *const n[] = { "?", "Deactivated", "Init", "Deinit", "Activated",
					 "TxTurnOn", "TxTurnOff", "Initialized" };

	return s < 8 ? n[s] : "?";
}

void omt_console_rx(struct omt_fw *fw, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (fw->con_len < sizeof(fw->con_buf) - 1) {
			fw->con_buf[fw->con_len++] = s[i];
		}
	}
	fw->con_buf[fw->con_len] = 0;
}

static void cmd_mon(struct omt_fw *fw)
{
	omt_printf(fw, "temp %.2f C  vcc %.4f V  laser %.2f C  tec %+.0f %%\r\n", (double)fw->temp_c,
		   (double)fw->vcc_v, (double)fw->laser_temp_c, (double)(fw->tec_drive * 100.0f));
	for (int l = 0; l < 4; l++) {
		const struct omt_tx_lane *t = &fw->tx[l];
		const struct omt_rx_lane *r = &fw->rx[l];

		omt_printf(fw, "L%d tx %7.1f uW bias %6.2f mA %s%s%s | rx %7.1f uW %s%s snr %5.2f dB ber %.2e\r\n",
			   l + 1, (double)t->tx_uw, (double)t->bias_meas_ma, t->enabled ? "on" : "off",
			   t->squelched ? ",sq" : "", t->fault ? ",FAULT" : "", (double)r->rx_uw,
			   r->los ? "LOS " : "", r->lol ? "LOL " : "", (double)r->snr_db,
			   (double)r->prefec_ber);
	}
}

static void cmd_faults(struct omt_fw *fw, int n)
{
	uint8_t e[16];

	omt_printf(fw, "fault log: %u entries (newest first)\r\n", fw->flog_count);
	for (int i = 0; i < n && flog_read(fw, (uint16_t)i, e) == 0; i++) {
		uint32_t seq, t;
		uint16_t code, arg;
		int16_t tq;

		memcpy(&seq, e, 4);
		memcpy(&t, e + 4, 4);
		memcpy(&code, e + 8, 2);
		memcpy(&arg, e + 10, 2);
		memcpy(&tq, e + 12, 2);
		omt_printf(fw, "#%lu t=%lu ms code=0x%02X arg=0x%04X temp=%.1f state=%s\r\n",
			   (unsigned long)seq, (unsigned long)t, code, arg, tq / 256.0, ms_name(e[14]));
	}
}

static void exec(struct omt_fw *fw, char *line)
{
	char *argv[6] = { 0 };
	int argc = 0;

	for (char *tok = strtok(line, " \t"); tok && argc < 6; tok = strtok(NULL, " \t")) {
		argv[argc++] = tok;
	}
	if (!argc) {
		return;
	}
	if (!strcmp(argv[0], "help")) {
		omt_printf(fw, "help ver state mon lanes dsp serdes faults [n] log tasks tec cal\r\n"
			       "mdio r <dev> <reg> | mdio w <dev> <reg> <val> | ld r <reg> | reset\r\n");
	} else if (!strcmp(argv[0], "ver")) {
		omt_printf(fw, "fw %u.%u build %u bank %c%s, cmis 5.2, resets %u (last cause %u)\r\n",
			   fw->running_major, fw->running_minor, fw->running_build,
			   'A' + fw->running_bank, fw->boot_confirmed ? "" : " (unconfirmed)",
			   fw->boot.reset_count, fw->boot.last_reset_reason);
	} else if (!strcmp(argv[0], "state")) {
		omt_printf(fw, "module %s fault_cause %u lowpwr_req %d dsp_ready %d dsp_mode %s\r\n",
			   ms_name(fw->mstate), fw->fault_cause, fw->lowpwr_req, fw->dsp_ready,
			   fw->dsp_mode ? "4x100G" : "1x400G");
	} else if (!strcmp(argv[0], "mon")) {
		cmd_mon(fw);
	} else if (!strcmp(argv[0], "lanes")) {
		for (int i = 0; i < 4; i++) {
			const struct omt_dp *d = &fw->dp[i];

			if (d->used) {
				omt_printf(fw, "DP%d app %u host 0x%02X media 0x%X %s\r\n", i, d->appsel,
					   d->host_mask, d->media_mask, dp_name(d->state));
			}
		}
	} else if (!strcmp(argv[0], "dsp")) {
		omt_printf(fw, "ready %d hb %u stalls %u recoveries %u mdio_err %u die %.1f C\r\n",
			   fw->dsp_ready, fw->dsp_heartbeat, fw->dsp_hb_stall, fw->dsp_recoveries,
			   fw->mdio_errors, fw->dsp_temp_dc / 10.0);
		for (int l = 0; l < 4; l++) {
			omt_printf(fw, "L%d snr %.2f dB preFEC %.3e corr %lu uncorr %lu\r\n", l + 1,
				   (double)fw->rx[l].snr_db, (double)fw->rx[l].prefec_ber,
				   (unsigned long)fw->rx[l].fec_corr, (unsigned long)fw->rx[l].fec_uncorr);
		}
	} else if (!strcmp(argv[0], "serdes")) {
		/* host-side input lanes and PRBS checkers, for bring-up without a host */
		uint8_t hchk = fw->m.p13[P13_HOST_CHK_EN - 128], mchk = fw->m.p13[P13_MEDIA_CHK_EN - 128];

		for (int h = 0; h < OMT_N_HOST_LANES; h++) {
			const struct omt_host_lane *x = &fw->host[h];

			omt_printf(fw, "H%d %s %s ctle %2u dB snr %5.2f dB%s\r\n", h + 1, x->sig ? "sig" : "LOS",
				   x->lock ? "lock" : "LOL ", x->eq_db, (double)x->snr_db,
				   x->adapt_fault ? " EQ-FAULT" : "");
			if ((hchk >> h) & 1) {
				omt_printf(fw, "   chk %s err %lu bits %.3e\r\n", fw->hchk[h].lock ? "lock" : "LOL",
					   (unsigned long)fw->hchk[h].err, (double)fw->hchk[h].bits);
			}
		}
		for (int m = 0; m < OMT_N_MEDIA_LANES; m++) {
			if ((mchk >> m) & 1) {
				omt_printf(fw, "M%d chk %s err %lu bits %.3e\r\n", m + 1, fw->mchk[m].lock ? "lock" : "LOL",
					   (unsigned long)fw->mchk[m].err, (double)fw->mchk[m].bits);
			}
		}
	} else if (!strcmp(argv[0], "faults")) {
		cmd_faults(fw, argc > 1 ? atoi(argv[1]) : 10);
	} else if (!strcmp(argv[0], "log")) {
		for (int i = 0; i < OMT_LOG_LEN; i++) {
			const struct omt_log_entry *e = &fw->log[(fw->log_head + i) % OMT_LOG_LEN];

			if (e->code) {
				omt_printf(fw, "%8lu 0x%03X 0x%04X\r\n", (unsigned long)e->t_ms, e->code, e->arg);
			}
		}
	} else if (!strcmp(argv[0], "tasks")) {
		for (int i = 0; i < fw->n_tasks; i++) {
			omt_printf(fw, "%-8s %4u ms runs %lu\r\n", fw->tasks[i].name, fw->tasks[i].period_ms,
				   (unsigned long)fw->tasks[i].runs);
		}
	} else if (!strcmp(argv[0], "tec")) {
		omt_printf(fw, "en %d set %.2f C act %.2f C drive %+.3f integ %+.3f locked %d\r\n",
			   fw->tec_enabled, (double)fw->tun.tec_setpoint_c, (double)fw->laser_temp_c,
			   (double)fw->tec_drive, (double)fw->tec_integ, tec_locked(fw));
	} else if (!strcmp(argv[0], "cal")) {
		const struct omt_calib *c = &fw->cal;

		omt_printf(fw, "sn %.16s temp_off %.3f vcc_gain %.5f tx_target %.2f dBm\r\n", c->serial,
			   (double)c->temp_offset_c, (double)c->vcc_gain, (double)c->tx_target_dbm);
		for (int l = 0; l < 4; l++) {
			omt_printf(fw, "L%d rssi %.6f uW/cnt off %.1f  mpd %.4f uW/cnt\r\n", l + 1,
				   (double)c->rssi_slope[l], (double)c->rssi_offset[l], (double)c->mpd_slope[l]);
		}
	} else if (!strcmp(argv[0], "mdio") && argc >= 4) {
		uint8_t dev = (uint8_t)strtoul(argv[2], NULL, 0);
		uint16_t reg = (uint16_t)strtoul(argv[3], NULL, 0);
		uint16_t v = 0;

		if (argv[1][0] == 'r') {
			int rc = fw->hal->mdio_read(fw->hal->ctx, 0, dev, reg, &v);

			omt_printf(fw, rc ? "mdio error\r\n" : "%u.0x%04X = 0x%04X\r\n", dev, reg, v);
		} else if (argc >= 5) {
			v = (uint16_t)strtoul(argv[4], NULL, 0);
			omt_printf(fw, fw->hal->mdio_write(fw->hal->ctx, 0, dev, reg, v) ? "mdio error\r\n"
											      : "ok\r\n");
		}
	} else if (!strcmp(argv[0], "ld") && argc >= 3) {
		uint16_t v = 0;
		uint8_t reg = (uint8_t)strtoul(argv[2], NULL, 0);
		/* rc first: argument evaluation order is unspecified in C, passing
		 * `v` next to `ld_read(.., &v)` printed stale data (findings F-006) */
		int rc = ld_read(fw, reg, &v);

		omt_printf(fw, rc ? "spi error\r\n" : "ld 0x%02X = 0x%04X\r\n", reg, v);
	} else if (!strcmp(argv[0], "reset")) {
		fw->hal->system_reset(fw->hal->ctx, OMT_RST_SOFTWARE);
	} else {
		omt_printf(fw, "unknown command '%s'\r\n", argv[0]);
	}
}

void console_task(struct omt_fw *fw)
{
	char line[96];
	char *nl;

	while ((nl = strpbrk(fw->con_buf, "\r\n")) != NULL) {
		size_t n = (size_t)(nl - fw->con_buf);

		memcpy(line, fw->con_buf, n);
		line[n] = 0;
		memmove(fw->con_buf, nl + 1, fw->con_len - n);
		fw->con_len = (uint8_t)(fw->con_len - n - 1);
		fw->con_buf[fw->con_len] = 0;
		exec(fw, line);
		omt_printf(fw, "omt> ");
	}
}
