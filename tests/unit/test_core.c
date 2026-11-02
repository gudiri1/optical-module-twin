/* SPDX-License-Identifier: Apache-2.0 */
/* Unit tests: alarm engine, CRC, images, CDB check code, AppSel validation,
 * fault log wrap-around, boot record. */
#include <stdlib.h>

#include "fake_hal.h"
#include "tinytest.h"

static void test_alarm_hysteresis(void)
{
	struct omt_threshold t = { .ha = 1000, .la = 100, .hw = 900, .lw = 200, .hyst = 20 };
	uint8_t c = 0;

	c = alarm_eval(&t, 500, c);
	CHECK_EQ(c, 0);
	c = alarm_eval(&t, 905, c);
	CHECK_EQ(c, 0x4);                 /* high warning */
	c = alarm_eval(&t, 890, c);
	CHECK_EQ(c, 0x4);                 /* inside hysteresis band: stays */
	c = alarm_eval(&t, 879, c);
	CHECK_EQ(c, 0);                   /* cleared */
	c = alarm_eval(&t, 1001, c);
	CHECK_EQ(c, 0x5);                 /* alarm implies warning */
	c = alarm_eval(&t, 99, 0);
	CHECK_EQ(c, 0xA);
}

static void test_crc32_vector(void)
{
	CHECK_EQ(omt_crc32(0, "123456789", 9), 0xCBF43926u);
	/* incremental == one shot */
	uint32_t a = omt_crc32(0, "12345", 5);

	CHECK_EQ(omt_crc32(a, "6789", 4), 0xCBF43926u);
}

static void test_image_validation(void)
{
	struct fake_hw *hw = calloc(1, sizeof(*hw));
	struct omt_fw *fw = calloc(1, sizeof(*fw));
	struct omt_tunables tun;
	struct omt_img_hdr h;
	uint8_t img[1024];

	fake_hal_init(hw);
	img_default_tunables(&tun);
	size_t n = omt_img_build(img, sizeof(img), 2, 3, 77, 0, "unit", &tun);

	CHECK(n > sizeof(h));
	memcpy(&hw->flash[OMT_NVM_IMAGE_B], img, n);
	omt_fw_init(fw, &hw->hal);
	CHECK_EQ(img_validate(fw, 1, &h), 0);
	CHECK_EQ(h.ver_major, 2);
	CHECK_EQ(h.ver_minor, 3);
	CHECK(img_validate(fw, 0, &h) != 0);          /* bank A blank */
	/* booting with blank committed bank falls back to the valid one */
	CHECK_EQ(fw->running_bank, 1);
	CHECK_EQ(fw->running_major, 2);
	hw->flash[OMT_NVM_IMAGE_B + sizeof(h) + 10] ^= 0x01; /* flip a payload bit */
	CHECK_EQ(img_validate(fw, 1, &h), -2);
	free(fw);
	free(hw);
}

static void test_cdb_chkcode(void)
{
	uint8_t p[128] = { 0 };

	p[P9F_CMD - 128] = 0x01;
	p[P9F_CMD - 128 + 1] = 0x01;
	p[P9F_LPL_LEN - 128] = 2;
	p[P9F_PAYLOAD - 128] = 0x10;
	p[P9F_PAYLOAD - 128 + 1] = 0x20;
	CHECK_EQ(cdb_chkcode(p), (uint8_t)~(0x01 + 0x01 + 2 + 0x10 + 0x20));
}

static void test_appsel_validation(void)
{
	struct fake_hw *hw = calloc(1, sizeof(*hw));
	struct omt_fw *fw = calloc(1, sizeof(*fw));

	fake_hal_init(hw);
	omt_fw_init(fw, &hw->hal);
	uint8_t *st = &fw->m.p10[P10_DPCONFIG - 128];

	/* breakout 4x100G, all lanes */
	for (int i = 0; i < 8; i++) {
		st[i] = 2 << 4;
	}
	dpsm_apply_config(fw, 0xFF);
	CHECK_EQ(fw->m.p11[P11_CFG_STATUS - 128] & 0xF, CMIS_CFG_SUCCESS);
	CHECK_EQ(fw->dsp_mode, 1);
	CHECK(fw->dp[3].used);
	CHECK_EQ(fw->dp[2].host_mask, 0x30);

	/* half a pair -> partial data path */
	dpsm_apply_config(fw, 0x01);
	CHECK_EQ(fw->m.p11[P11_CFG_STATUS - 128] & 0xF, CMIS_CFG_REJ_PARTIAL_DP);

	/* undefined AppSel */
	st[0] = st[1] = 5 << 4;
	dpsm_apply_config(fw, 0x03);
	CHECK_EQ(fw->m.p11[P11_CFG_STATUS - 128] & 0xF, CMIS_CFG_REJ_APPSEL);

	/* AppSel 1 needs all eight host lanes */
	for (int i = 0; i < 8; i++) {
		st[i] = 1 << 4;
	}
	dpsm_apply_config(fw, 0x0F);
	CHECK_EQ(fw->m.p11[P11_CFG_STATUS - 128] & 0xF, CMIS_CFG_REJ_PARTIAL_DP);

	/* lanes in use: pretend DP0 is active */
	fw->dp[0].state = CMIS_DP_ACTIVATED;
	dpsm_apply_config(fw, 0xFF);
	CHECK_EQ(fw->m.p11[P11_CFG_STATUS - 128] & 0xF, CMIS_CFG_REJ_IN_USE);
	free(fw);
	free(hw);
}

static void test_fault_log_wrap(void)
{
	struct fake_hw *hw = calloc(1, sizeof(*hw));
	struct omt_fw *fw = calloc(1, sizeof(*fw));
	uint8_t e[16];
	uint16_t code;

	fake_hal_init(hw);
	omt_fw_init(fw, &hw->hal);
	for (int i = 0; i < 700; i++) {          /* > 2 sectors of 256 entries */
		flog_append(fw, 0x7000 | (uint16_t)i, (uint16_t)i);
	}
	CHECK(flog_read(fw, 0, e) == 0);
	memcpy(&code, e + 8, 2);
	CHECK_EQ(code, 0x7000 | 699);
	/* survives re-init (power cycle) */
	uint32_t seq_before = fw->flog_seq;

	omt_fw_init(fw, &hw->hal);
	CHECK(fw->flog_seq > seq_before);       /* boot events appended */
	CHECK(fw->flog_count >= 256 && fw->flog_count <= 512);
	free(fw);
	free(hw);
}

static void test_boot_record_log_structured(void)
{
	struct fake_hw *hw = calloc(1, sizeof(*hw));
	struct omt_fw *fw = calloc(1, sizeof(*fw));

	fake_hal_init(hw);
	for (int i = 0; i < 400; i++) {          /* forces sector recycling */
		omt_fw_init(fw, &hw->hal);
	}
	CHECK_EQ(fw->boot.reset_count, 400);
	free(fw);
	free(hw);
}

static void test_console_smoke(void)
{
	struct fake_hw *hw = calloc(1, sizeof(*hw));
	struct omt_fw *fw = calloc(1, sizeof(*fw));

	fake_hal_init(hw);
	omt_fw_init(fw, &hw->hal);
	omt_console_rx(fw, "ver\nstate\n", 10);
	fake_run(hw, fw, 20);
	CHECK(strstr(hw->uart, "fw ") != NULL);
	CHECK(strstr(hw->uart, "module LowPwr") != NULL);
	CHECK_EQ(hw->irq_depth, 0);
	free(fw);
	free(hw);
}

int main(void)
{
	RUN(test_alarm_hysteresis);
	RUN(test_crc32_vector);
	RUN(test_image_validation);
	RUN(test_cdb_chkcode);
	RUN(test_appsel_validation);
	RUN(test_fault_log_wrap);
	RUN(test_boot_record_log_structured);
	RUN(test_console_smoke);
	TT_END();
}
