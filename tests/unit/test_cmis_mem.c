/* SPDX-License-Identifier: Apache-2.0 */
/* Unit tests: CMIS memory map access rules, paging, COR flags, IntL. */
#include <stdlib.h>

#include "fake_hal.h"
#include "tinytest.h"

static struct fake_hw hw;
static struct omt_fw *fw;

static void setup(void)
{
	fake_hal_init(&hw);
	fw = calloc(1, sizeof(*fw));
	omt_fw_init(fw, &hw.hal);
}

static void teardown(void) { free(fw); }

static uint8_t sum(const uint8_t *p, int a, int b)
{
	uint8_t s = 0;

	for (int i = a; i <= b; i++) {
		s = (uint8_t)(s + p[i - 128]);
	}
	return s;
}

static void test_identity_and_checksums(void)
{
	uint8_t pg[128];

	setup();
	CHECK_EQ(host_read1(fw, CMIS_ID), 0x18);
	CHECK_EQ(host_read1(fw, CMIS_REV), 0x52);
	CHECK_EQ((host_read1(fw, CMIS_MOD_STATE) >> 1) & 7, CMIS_MS_LOWPWR);
	host_write1(fw, CMIS_PAGE_SELECT, 0x00);
	host_read(fw, 128, pg, 128);
	CHECK_EQ(pg[P00_CHECKSUM - 128], sum(pg, 128, 221));
	CHECK(memcmp(&pg[P00_VENDOR_NAME - 128], "OMT TWIN        ", 16) == 0);
	CHECK_EQ(pg[P00_MAX_POWER - 128], 40);
	CHECK_EQ(pg[P00_POWER_CLASS - 128] >> 5, 7);
	host_write1(fw, CMIS_PAGE_SELECT, 0x01);
	host_read(fw, 128, pg, 128);
	CHECK_EQ(pg[P01_CHECKSUM - 128], sum(pg, 130, 254));
	CHECK_EQ(pg[P01_FW_INACTIVE_MAJOR - 128], 0); /* bank B empty */
	host_write1(fw, CMIS_PAGE_SELECT, 0x02);
	host_read(fw, 128, pg, 128);
	CHECK_EQ(pg[P02_CHECKSUM - 128], sum(pg, 128, 254));
	teardown();
}

static void test_readonly_and_paging(void)
{
	setup();
	uint16_t ro0 = fw->m.ro_write;

	host_write1(fw, CMIS_ID, 0x55);                 /* RO */
	CHECK_EQ(host_read1(fw, CMIS_ID), 0x18);
	CHECK_EQ(fw->m.ro_write, ro0 + 1);
	host_write1(fw, CMIS_PAGE_SELECT, 0x02);         /* thresholds are RO */
	uint8_t before = host_read1(fw, 128);

	host_write1(fw, 128, (uint8_t)~before);
	CHECK_EQ(host_read1(fw, 128), before);
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);         /* user EEPROM is RW */
	host_write1(fw, 200, 0xA5);
	CHECK_EQ(host_read1(fw, 200), 0xA5);
	host_write1(fw, CMIS_PAGE_SELECT, 0x42);         /* unsupported page */
	CHECK_EQ(host_read1(fw, 130), 0x00);
	CHECK(fw->m.bad_page >= 1);
	host_write1(fw, CMIS_BANK_SELECT, 1);            /* only bank 0 */
	host_write1(fw, CMIS_PAGE_SELECT, 0x11);
	CHECK_EQ(host_read1(fw, P11_DP_STATE), 0x00);
	host_write1(fw, CMIS_BANK_SELECT, 0);
	CHECK_EQ(host_read1(fw, P11_DP_STATE), 0x11);   /* lanes 1,2 Deactivated */
	teardown();
}

static void test_user_eeprom_persists_coalesced(void)
{
	setup();
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	uint32_t e0 = hw.erases;

	for (int i = 0; i < 16; i++) {
		uint8_t d[8] = { (uint8_t)i, 1, 2, 3, 4, 5, 6, 7 };

		host_write(fw, (uint8_t)(128 + 8 * (i % 16)), d, 8);
		fake_run(&hw, fw, 10);
	}
	fake_run(&hw, fw, 700);
	CHECK(hw.erases - e0 <= 1);                   /* 16 writes, one save */
	/* "power cycle": new firmware instance on the same flash */
	omt_fw_init(fw, &hw.hal);
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	CHECK_EQ(host_read1(fw, 128 + 8 * 5), 5);
	teardown();
}

static void save_burst(uint8_t v)
{
	uint8_t d[4] = { v, (uint8_t)(v + 1), (uint8_t)(v + 2), (uint8_t)(v + 3) };

	host_write(fw, 200, d, 4);
	fake_run(&hw, fw, 700);
}

/* F-010: log-structured, two sectors: one erase per ~29 saves, and a save
 * interrupted by power loss falls back to the previous copy. */
static void test_user_eeprom_wear_and_power_fail(void)
{
	setup();
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	uint32_t e0 = hw.erases;

	for (int i = 0; i < 100; i++) {
		save_burst((uint8_t)i);
	}
	CHECK(hw.erases - e0 <= 4);                   /* was 100 */
	omt_fw_init(fw, &hw.hal);
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	CHECK_EQ(host_read1(fw, 200), 99);

	/* power lost while programming save #101: corrupt the newest slot */
	save_burst(0xA0);
	/* 29 slots per sector: saves 1-29 A, 30-58 B, 59-87 A, 88-101 B, so the
	 * newest record ends at the last programmed byte of sector B */
	uint32_t newest = 0;

	for (uint32_t addr = OMT_NVM_USER_EEPROM_B; addr < OMT_NVM_USER_EEPROM_B + OMT_NVM_SECTOR; addr++) {
		if (hw.flash[addr] != 0xFF) {
			newest = addr;
		}
	}
	CHECK(newest != 0);
	hw.flash[newest] = 0x00;                      /* torn CRC */
	omt_fw_init(fw, &hw.hal);
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	CHECK_EQ(host_read1(fw, 200), 99);            /* previous copy */
	save_burst(0xB0);                             /* skips the torn slot */
	omt_fw_init(fw, &hw.hal);
	host_write1(fw, CMIS_PAGE_SELECT, 0x03);
	CHECK_EQ(host_read1(fw, 200), 0xB0);
	teardown();
}

static void test_clear_on_read_and_intl(void)
{
	setup();
	/* reset left ModuleStateChanged latched -> IntL asserted */
	CHECK_EQ(hw.pin[OMT_PIN_INTL], 0);
	CHECK_EQ(host_read1(fw, CMIS_MOD_STATE) & 1, 0);
	CHECK(host_read1(fw, CMIS_FLAG_MODULE) & CMIS_F_STATE_CHANGED); /* reading clears */
	CHECK_EQ(host_read1(fw, CMIS_FLAG_MODULE), 0);
	CHECK_EQ(hw.pin[OMT_PIN_INTL], 1);
	CHECK_EQ(host_read1(fw, CMIS_MOD_STATE) & 1, 1);

	/* latch a condition: rising edge sets, persists across reads? no: COR */
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x01);
	cmis_update_intl(fw);
	CHECK_EQ(hw.pin[OMT_PIN_INTL], 0);
	CHECK_EQ(host_read1(fw, CMIS_FLAG_TEMP_VCC), 0x01);
	CHECK_EQ(host_read1(fw, CMIS_FLAG_TEMP_VCC), 0x00); /* still active, no new edge */
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x01);
	CHECK_EQ(host_read1(fw, CMIS_FLAG_TEMP_VCC), 0x00);
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x00);
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x01);   /* new edge */
	CHECK_EQ(host_read1(fw, CMIS_FLAG_TEMP_VCC), 0x01);

	/* masked flag latches but does not assert IntL */
	host_write1(fw, CMIS_MASK_TEMP_VCC, 0xFF);
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x00);
	cmis_latch_module(fw, CMIS_FLAG_TEMP_VCC, 0x02);
	cmis_update_intl(fw);
	CHECK_EQ(hw.pin[OMT_PIN_INTL], 1);
	CHECK_EQ(host_read1(fw, CMIS_FLAG_TEMP_VCC), 0x02);
	teardown();
}

static void test_password_write_only_and_unlock(void)
{
	uint8_t pw[4] = { 0x4F, 0x4D, 0x54, 0x01 }, bad[4] = { 0, 0, 0x10, 0x11 };

	setup();
	host_write(fw, CMIS_PWD_ENTRY, bad, 4);
	CHECK(!fw->m.cal_unlocked);
	host_write1(fw, CMIS_PAGE_SELECT, PAGE_VENDOR_CAL);
	host_write1(fw, PB1_COEF, 0x12);
	CHECK(fw->m.pb1[PB1_COEF - 128] != 0x12 || fw->m.ro_write > 0);
	host_write(fw, CMIS_PWD_ENTRY, pw, 4);
	CHECK(fw->m.cal_unlocked);
	CHECK_EQ(host_read1(fw, CMIS_PWD_ENTRY), 0);       /* write-only */
	teardown();
}

static void test_sequential_read_crosses_into_upper(void)
{
	uint8_t b[4];

	setup();
	host_write1(fw, CMIS_PAGE_SELECT, 0x00);
	host_read(fw, 126, b, 4);
	CHECK_EQ(b[1], 0x00);            /* page select */
	CHECK_EQ(b[2], 0x18);            /* page 00h byte 128 */
	teardown();
}

int main(void)
{
	RUN(test_identity_and_checksums);
	RUN(test_readonly_and_paging);
	RUN(test_user_eeprom_persists_coalesced);
	RUN(test_user_eeprom_wear_and_power_fail);
	RUN(test_clear_on_read_and_intl);
	RUN(test_password_write_only_and_unlock);
	RUN(test_sequential_read_crosses_into_upper);
	TT_END();
}
