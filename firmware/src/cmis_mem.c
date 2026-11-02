/*
 * CMIS management memory: static content, paging, host access rules,
 * clear-on-read latched flags and IntL generation.
 *
 * Concurrency: on a real MCU omt_i2c_read/write run in the I2C target
 * ISR. All multi-byte monitor updates done by tasks are wrapped in
 * hal->irq_lock()/irq_unlock() so the host never reads a torn U16.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "omt/fw.h"

static void put_ascii(uint8_t *dst, const char *s, size_t len)
{
	size_t n = strlen(s);

	memset(dst, ' ', len);
	memcpy(dst, s, n < len ? n : len);
}

void cmis_set_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

uint16_t cmis_get_u16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

void cmis_set_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

void cmis_set_f32(uint8_t *p, float v)
{
	uint32_t u;

	memcpy(&u, &v, sizeof(u));
	cmis_set_u32(p, u);
}

float cmis_get_f32(const uint8_t *p)
{
	uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		     ((uint32_t)p[2] << 8) | p[3];
	float v;

	memcpy(&v, &u, sizeof(v));
	return v;
}

uint8_t cmis_encode_power_class(uint32_t max_mw)
{
	static const uint16_t lim[] = { 1500, 2000, 2500, 3500, 4000, 4500, 5000 };

	for (uint8_t i = 0; i < 7; i++) {
		if (max_mw <= lim[i]) {
			return (uint8_t)(i << 5);
		}
	}
	return 7u << 5; /* class 8 */
}

uint8_t *cmis_page(struct omt_fw *fw, uint8_t page)
{
	switch (page) {
	case 0x00: return fw->m.p00;
	case 0x01: return fw->m.p01;
	case 0x02: return fw->m.p02;
	case 0x03: return fw->m.p03;
	case 0x10: return fw->m.p10;
	case 0x11: return fw->m.p11;
	case 0x9F: return fw->m.p9f;
	case PAGE_VENDOR_DIAG: return fw->m.pb0;
	case PAGE_VENDOR_CAL: return fw->m.pb1;
	default: return NULL;
	}
}

static uint8_t sum8(const uint8_t *page, uint8_t from, uint8_t to)
{
	uint8_t s = 0;

	for (unsigned i = from; i <= to; i++) {
		s = (uint8_t)(s + page[i - 128]);
	}
	return s;
}

void cmis_update_checksums(struct omt_fw *fw)
{
	fw->m.p00[P00_CHECKSUM - 128] = sum8(fw->m.p00, 128, 221);
	fw->m.p01[P01_CHECKSUM - 128] = sum8(fw->m.p01, 130, 254);
	fw->m.p02[P02_CHECKSUM - 128] = sum8(fw->m.p02, 128, 254);
}

static void set_app(uint8_t *d, uint8_t host_if, uint8_t media_if, uint8_t lanes, uint8_t assign)
{
	d[0] = host_if;
	d[1] = media_if;
	d[2] = lanes;
	d[3] = assign;
}

void cmis_mem_init(struct omt_fw *fw)
{
	struct omt_cmis *m = &fw->m;
	uint8_t *lo = m->lower;

	memset(m, 0, sizeof(*m));

	/* ---- lower memory ---- */
	lo[CMIS_ID] = 0x18;              /* QSFP-DD */
	lo[CMIS_REV] = 0x52;             /* CMIS 5.2 */
	lo[CMIS_MEM_CHAR] = 0x00;        /* paged memory */
	lo[CMIS_MOD_CTRL] = CMIS_CTRL_LOWPWR_ALLOW_HW;
	lo[CMIS_FW_ACTIVE_MAJOR] = fw->running_major;
	lo[CMIS_FW_ACTIVE_MINOR] = fw->running_minor;
	lo[CMIS_MEDIA_TYPE] = 0x02;      /* SMF */
	/* App 1: 400GAUI-8 C2M / 400GBASE-DR4, 8 host + 4 media lanes, lane 1 */
	set_app(&lo[CMIS_APP_DESC + 0], 0x11, 0x1C, 0x84, 0x01);
	/* App 2: 100GAUI-2 C2M / 100GBASE-DR, 2 host + 1 media, lanes 1,3,5,7 */
	set_app(&lo[CMIS_APP_DESC + 4], 0x0D, 0x14, 0x21, 0x55);
	lo[CMIS_APP_DESC + 8] = 0xFF;    /* end of application list */
	/* Leaving MgmtInit into ModuleLowPwr latches ModuleStateChanged,
	 * so a host sees IntL after every module reset. */
	cmis_set_module_state(fw, CMIS_MS_LOWPWR);

	/* ---- page 00h ---- */
	uint8_t *p = m->p00;
	p[P00_ID - 128] = 0x18;
	put_ascii(&p[P00_VENDOR_NAME - 128], "OMT TWIN", 16);
	put_ascii(&p[P00_VENDOR_PN - 128], "OMT-400G-DR4", 16);
	put_ascii(&p[P00_VENDOR_REV - 128], "A1", 2);
	put_ascii(&p[P00_VENDOR_SN - 128], fw->cal.serial[0] ? fw->cal.serial : "UNCALIBRATED", 16);
	{
		char dc[9] = { 0 };

		memcpy(dc, fw->cal.date_code, 8);
		put_ascii(&p[P00_DATE_CODE - 128], dc[0] ? dc : "000000  ", 8);
	}
	put_ascii(&p[P00_CLEI - 128], "", 10);
	p[P00_POWER_CLASS - 128] = cmis_encode_power_class(10000);
	p[P00_MAX_POWER - 128] = 40;     /* 10.0 W */
	p[P00_CONNECTOR - 128] = 0x0C;   /* MPO 1x12 */

	/* ---- page 01h ---- */
	p = m->p01;
	p[P01_HW_REV_MAJOR - 128] = 1;
	p[P01_HW_REV_MINOR - 128] = 0;
	cmis_set_u16(&p[P01_WAVELENGTH - 128], 26220);   /* 1311.0 nm */
	cmis_set_u16(&p[P01_WAVELENGTH_TOL - 128], 1300); /* +/- 6.5 nm */
	p[P01_PAGES_SUPPORTED - 128] = 0x04;             /* page 03h present */
	p[P01_DUR_DP - 128] = (0x5 << 4) | 0x6;          /* deinit 100-500 ms, init 0.5-1 s */
	p[P01_MOD_MON_SUPPORT - 128] = 0x0F;             /* temp, vcc, aux1, aux2 */
	p[P01_LANE_MON_SUPPORT - 128] = 0x07;            /* bias x1, txpwr, rxpwr */
	p[P01_CDB_SUPPORT - 128] = 0x41;                 /* 1 CDB instance, FW mgmt */
	p[P01_DUR_MODULE - 128] = (0x5 << 4) | 0x7;      /* pwrdn 100-500 ms, pwrup 1-5 s */

	/* ---- page 02h thresholds (factory, read-only) ---- */
	mon_load_thresholds(fw);

	/* ---- page 10h defaults ---- */
	m->p10[P10_DP_DEINIT - 128] = 0x00;
	for (int i = 0; i < 8; i++) {
		/* AppSel 1, DataPathID 0 */
		m->p10[P10_DPCONFIG - 128 + i] = (1u << 4);
		m->p11[P11_ACTIVE_DPCONFIG - 128 + i] = (1u << 4);
	}

	/* ---- page 11h ---- */
	for (uint8_t l = 0; l < 8; l++) {
		cmis_set_dp_state(fw, l, CMIS_DP_DEACTIVATED);
	}
	for (int i = 0; i < 4; i++) {
		m->p11[P11_CFG_STATUS - 128 + i] = (CMIS_CFG_SUCCESS << 4) | CMIS_CFG_SUCCESS;
	}

	nvm_load_user_eeprom(fw);
	cmis_update_checksums(fw);
	cmis_update_intl(fw);
}

void cmis_set_module_state(struct omt_fw *fw, uint8_t st)
{
	uint8_t *b = &fw->m.lower[CMIS_MOD_STATE];
	uint8_t old = (*b >> 1) & 0x7;

	*b = (uint8_t)((*b & ~0x0E) | ((st & 0x7) << 1));
	if (old != st && (st == CMIS_MS_LOWPWR || st == CMIS_MS_READY || st == CMIS_MS_FAULT)) {
		fw->m.lower[CMIS_FLAG_MODULE] |= CMIS_F_STATE_CHANGED;
		cmis_update_intl(fw);
	}
}

void cmis_set_dp_state(struct omt_fw *fw, uint8_t host_lane, uint8_t st)
{
	uint8_t *b = &fw->m.p11[P11_DP_STATE - 128 + host_lane / 2];
	uint8_t sh = (host_lane & 1) ? 4 : 0;
	uint8_t old = (*b >> sh) & 0xF;

	*b = (uint8_t)((*b & ~(0xF << sh)) | ((st & 0xF) << sh));
	if (old != st && old != 0 &&
	    (st == CMIS_DP_ACTIVATED || st == CMIS_DP_DEACTIVATED || st == CMIS_DP_INITIALIZED)) {
		fw->m.p11[P11_FLAG_DP_CHANGED - 128] |= (uint8_t)(1u << host_lane);
		cmis_update_intl(fw);
	}
}

/*
 * Latched-flag policy (open spec question, docs/findings.md F-004):
 *   OMT_FLAGS_RELATCH = 0  set on the rising edge of a condition only; after
 *                          a clear-on-read the flag stays clear while the
 *                          condition persists (no interrupt storm)
 *   OMT_FLAGS_RELATCH = 1  re-set on every evaluation while the condition
 *                          persists (level semantics after the read)
 */
#ifndef OMT_FLAGS_RELATCH
#define OMT_FLAGS_RELATCH 0
#endif

static uint8_t latch_bits(uint8_t cond, uint8_t prev)
{
	return OMT_FLAGS_RELATCH ? cond : (uint8_t)(cond & (uint8_t)~prev);
}

/* Latch a condition byte (lower memory bytes 9..11). */
void cmis_latch_module(struct omt_fw *fw, uint8_t byte_off, uint8_t cond)
{
	uint8_t *prev = &fw->m.cond_module[byte_off - CMIS_FLAG_MODULE];
	uint8_t rising = latch_bits(cond, *prev);

	fw->m.lower[byte_off] |= rising;
	*prev = cond;
}

void cmis_latch_lane(struct omt_fw *fw, uint8_t flag_off, uint8_t cond)
{
	uint8_t *prev = &fw->m.cond_lane[flag_off - P11_FLAG_FIRST];
	uint8_t rising = latch_bits(cond, *prev);

	fw->m.p11[flag_off - 128] |= rising;
	*prev = cond;
}

void cmis_update_intl(struct omt_fw *fw)
{
	struct omt_cmis *m = &fw->m;
	bool irq = false;

	for (int i = 0; i < 4; i++) {
		if (m->lower[CMIS_FLAG_MODULE + i] & (uint8_t)~m->lower[CMIS_MASK_MODULE + i]) {
			irq = true;
		}
	}
	for (int off = P11_FLAG_FIRST; off <= P11_FLAG_LAST; off++) {
		uint8_t mask = m->p10[P10_MASK_BASE - 128 + (off - P11_FLAG_FIRST)];

		if (m->p11[off - 128] & (uint8_t)~mask) {
			irq = true;
		}
	}
	if (irq) {
		m->lower[CMIS_MOD_STATE] &= (uint8_t)~0x01;
	} else {
		m->lower[CMIS_MOD_STATE] |= 0x01;
	}
	if (fw->hal && fw->hal->gpio_set) {
		fw->hal->gpio_set(fw->hal->ctx, OMT_PIN_INTL, irq ? 0 : 1);
	}
}

/* ------------------------------------------------------------------ */
/* Host access                                                         */
/* ------------------------------------------------------------------ */

static uint8_t cur_page(const struct omt_fw *fw)
{
	return fw->m.lower[CMIS_PAGE_SELECT];
}

static bool page_banked(uint8_t page)
{
	return page >= 0x10 && page <= 0x1F;
}

static uint8_t *upper_ptr(struct omt_fw *fw, bool count_bad)
{
	uint8_t page = cur_page(fw);
	uint8_t *p;

	if (page_banked(page) && fw->m.lower[CMIS_BANK_SELECT] != 0) {
		p = NULL;
	} else {
		p = cmis_page(fw, page);
	}
	if (!p && count_bad) {
		fw->m.bad_page++;
	}
	return p;
}

static bool lower_writable(uint8_t off)
{
	return off == CMIS_MOD_CTRL ||
	       (off >= CMIS_MASK_MODULE && off <= CMIS_MASK_AUX3) ||
	       (off >= CMIS_PWD_CHANGE && off <= CMIS_PWD_ENTRY + 3) ||
	       off == CMIS_BANK_SELECT || off == CMIS_PAGE_SELECT;
}

static bool upper_writable(const struct omt_fw *fw, uint8_t page, uint8_t off)
{
	switch (page) {
	case 0x03:
		return true;
	case 0x10:
		return off <= P10_TX_SQ_DISABLE ||
		       (off >= P10_APPLY_DPINIT && off < P10_DPCONFIG + 8) ||
		       (off >= P10_MASK_BASE && off <= P10_MASK_BASE + (P11_FLAG_LAST - P11_FLAG_FIRST));
	case 0x9F:
		return true;
	case PAGE_VENDOR_CAL:
		return fw->m.cal_unlocked && off >= PB1_COEF;
	default:
		return false;
	}
}

static bool is_clear_on_read(uint8_t page, uint8_t off)
{
	if (off < 128) {
		return off >= CMIS_FLAG_MODULE && off <= CMIS_FLAG_AUX3;
	}
	return page == 0x11 && off >= P11_FLAG_FIRST && off <= P11_FLAG_LAST;
}

int omt_i2c_write(struct omt_fw *fw, const uint8_t *buf, size_t len)
{
	if (len == 0) {
		return -1;
	}
	fw->m.addr = buf[0];
	for (size_t i = 1; i < len; i++) {
		uint8_t off = fw->m.addr;

		if (off < 128) {
			if (!lower_writable(off)) {
				fw->m.ro_write++;
			} else {
				fw->m.lower[off] = buf[i];
				if (off == CMIS_MOD_CTRL) {
					fw->m.dirty |= DIRTY_MODCTRL;
				} else if (off == CMIS_PWD_ENTRY + 3) {
					fw->m.dirty |= DIRTY_PASSWORD;
				}
			}
		} else {
			uint8_t page = cur_page(fw);
			uint8_t *p = upper_ptr(fw, true);

			if (p && upper_writable(fw, page, off)) {
				p[off - 128] = buf[i];
				if (page == 0x10) {
					fw->m.dirty |= DIRTY_LANECTRL;
					if (off == P10_APPLY_DPINIT) {
						fw->m.dirty |= DIRTY_APPLY_DPINIT;
					}
				} else if (page == 0x03) {
					fw->m.dirty |= DIRTY_USER_EEPROM;
					fw->m.user_eeprom_dirty_ms = omt_now(fw);
				} else if (page == 0x9F && off == P9F_CMD + 1) {
					fw->m.dirty |= DIRTY_CDB;
					fw->m.lower[CMIS_CDB_STATUS1] = CDB_BUSY_PROCESSING;
				} else if (page == PAGE_VENDOR_CAL && off == PB1_COMMAND) {
					fw->m.dirty |= DIRTY_CAL_CMD;
				}
			} else if (p) {
				fw->m.ro_write++;
			}
		}
		fw->m.addr = (uint8_t)(off + 1);
	}

	if (fw->m.dirty & DIRTY_PASSWORD) {
		uint32_t pw = ((uint32_t)fw->m.lower[CMIS_PWD_ENTRY] << 24) |
			      ((uint32_t)fw->m.lower[CMIS_PWD_ENTRY + 1] << 16) |
			      ((uint32_t)fw->m.lower[CMIS_PWD_ENTRY + 2] << 8) |
			      fw->m.lower[CMIS_PWD_ENTRY + 3];

		fw->m.cal_unlocked = (pw == OMT_FACTORY_PASSWORD);
		fw->m.dirty &= ~DIRTY_PASSWORD;
	}
	return 0;
}

int omt_i2c_read(struct omt_fw *fw, uint8_t *buf, size_t len)
{
	bool cleared = false;

	for (size_t i = 0; i < len; i++) {
		uint8_t off = fw->m.addr;
		uint8_t page = cur_page(fw);
		uint8_t v;

		if (off < 128) {
			/* passwords are write-only */
			v = (off >= CMIS_PWD_CHANGE && off <= CMIS_PWD_ENTRY + 3) ? 0 : fw->m.lower[off];
			if (is_clear_on_read(page, off) && v) {
				fw->m.lower[off] = 0;
				cleared = true;
			}
		} else {
			uint8_t *p = upper_ptr(fw, i == 0);

			v = p ? p[off - 128] : 0;
			if (p && is_clear_on_read(page, off) && v) {
				p[off - 128] = 0;
				cleared = true;
			}
		}
		buf[i] = v;
		fw->m.addr = (uint8_t)(off + 1);
	}
	if (cleared) {
		cmis_update_intl(fw);
	}
	return 0;
}
