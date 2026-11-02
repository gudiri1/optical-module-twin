/*
 * Zephyr CMIS library vs. Optical Module Twin: decode every captured module
 * state with lib/cmis and compare with the independent Python decoder.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include <zephyr/cmis/cmis.h>
#include <zephyr/ztest.h>

#include "cmis_vectors.h"

#define N_VECTORS ARRAY_SIZE(cmis_vectors)

static void check_thr(const char *vec, const char *name, const struct cmis_threshold *t,
		      const int32_t exp[4], int32_t tol)
{
	const int32_t got[4] = {t->high_alarm, t->low_alarm, t->high_warning, t->low_warning};

	for (int i = 0; i < 4; i++) {
		zassert_true(abs(got[i] - exp[i]) <= tol, "%s %s[%d]: zephyr %d, twin %d", vec, name,
			     i, got[i], exp[i]);
	}
}

ZTEST(cmis_xcheck, test_identity_and_monitors)
{
	for (size_t v = 0; v < N_VECTORS; v++) {
		const struct cmis_vector *x = &cmis_vectors[v];
		uint8_t flat[CMIS_PAGE_00H_SIZE];
		struct cmis_monitors mon;
		struct cmis_id id;

		memcpy(flat, x->lower, 128);
		memcpy(&flat[128], x->page00, 128);
		zassert_ok(cmis_parse_id(flat, &id));
		zassert_ok(cmis_parse_monitors(x->lower, &mon));

		zassert_equal(id.identifier, x->identifier, "%s", x->name);
		zassert_equal(id.module_state, x->module_state, "%s", x->name);
		zassert_equal(id.power_class, x->power_class, "%s: %u vs %u", x->name,
			      id.power_class, x->power_class);
		zassert_equal(id.max_power_mw, x->max_power_mw, "%s", x->name);
		zassert_str_equal(id.vendor_pn, x->vendor_pn, "%s", x->name);
		zassert_str_equal(id.vendor_sn, x->vendor_sn, "%s", x->name);
		zassert_true(abs(mon.temp_mdegc - x->temp_mdegc) <= 1, "%s temp %d vs %d", x->name,
			     mon.temp_mdegc, x->temp_mdegc);
		zassert_equal(mon.vcc_uv, x->vcc_uv, "%s", x->name);
		zassert_equal(mon.flags, x->monitor_flags, "%s flags 0x%02x vs 0x%02x", x->name,
			      mon.flags, x->monitor_flags);
	}
}

ZTEST(cmis_xcheck, test_thresholds)
{
	for (size_t v = 0; v < N_VECTORS; v++) {
		const struct cmis_vector *x = &cmis_vectors[v];
		struct cmis_thresholds thr;
		uint8_t mult;

		zassert_ok(cmis_parse_tx_bias_multiplier(x->page01, &mult));
		zassert_equal(mult, x->tx_bias_multiplier, "%s", x->name);
		zassert_ok(cmis_parse_thresholds(x->page02, mult, &thr));
		check_thr(x->name, "temp", &thr.temp_mdegc, x->thr_temp_mdegc, 1);
		check_thr(x->name, "vcc", &thr.vcc_uv, x->thr_vcc_uv, 0);
		check_thr(x->name, "tx_power", &thr.tx_power_nw, x->thr_tx_power_nw, 0);
		check_thr(x->name, "tx_bias", &thr.tx_bias_ua, x->thr_tx_bias_ua, 0);
		check_thr(x->name, "rx_power", &thr.rx_power_nw, x->thr_rx_power_nw, 0);
	}
}

ZTEST(cmis_xcheck, test_lanes)
{
	for (size_t v = 0; v < N_VECTORS; v++) {
		const struct cmis_vector *x = &cmis_vectors[v];
		struct cmis_lane_status lanes[CMIS_LANES_PER_BANK];

		zassert_ok(cmis_parse_lane_status(x->page11, x->tx_bias_multiplier, lanes));
		for (int l = 0; l < CMIS_LANES_PER_BANK; l++) {
			const struct cmis_vector_lane *e = &x->lanes[l];

			zassert_equal(lanes[l].dp_state, e->dp_state, "%s lane %d", x->name, l + 1);
			zassert_equal(lanes[l].tx_power_nw, e->tx_power_nw, "%s lane %d", x->name, l + 1);
			zassert_equal(lanes[l].tx_bias_ua, e->tx_bias_ua, "%s lane %d", x->name, l + 1);
			zassert_equal(lanes[l].rx_power_nw, e->rx_power_nw, "%s lane %d", x->name, l + 1);
			zassert_equal(lanes[l].flags, e->flags, "%s lane %d flags 0x%x vs 0x%x",
				      x->name, l + 1, lanes[l].flags, e->flags);
		}
	}
	TC_PRINT("%u twin states cross-checked\n", (unsigned int)N_VECTORS);
}

ZTEST_SUITE(cmis_xcheck, NULL, NULL, NULL, NULL, NULL);
