/*
 * Non-volatile storage on NOR flash: factory calibration, user EEPROM
 * (CMIS Page 03h), a power-fail-safe fault log and the boot record.
 *
 * Flash rules honoured everywhere: erase sets 0xFF, programming can only
 * clear bits, so every record carries a CRC and log-structured areas are
 * scanned for the newest valid entry at boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "omt/fw.h"

/* CRC-32/ISO-HDLC (zlib compatible), bitwise: small code, no table */
uint32_t omt_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;

	crc = ~crc;
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
	uint8_t c = 0;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++) {
			c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
		}
	}
	return c;
}

#define HAL(fw) ((fw)->hal)

/* ------------------------------ calibration ------------------------- */

void nvm_default_calibration(struct omt_calib *c)
{
	memset(c, 0, sizeof(*c));
	c->magic = OMT_CAL_MAGIC;
	c->version = 1;
	c->len = sizeof(*c);
	c->vcc_gain = 1.0f;
	for (int l = 0; l < 4; l++) {
		c->rssi_slope[l] = 1000.0f / 19000.0f;  /* nominal TIA/ADC chain */
		c->rssi_offset[l] = 200.0f;
		c->mpd_slope[l] = 1.0f;
	}
	c->tx_target_dbm = 1.0f;
}

void nvm_load_calibration(struct omt_fw *fw)
{
	struct omt_calib c;

	HAL(fw)->nvm_read(HAL(fw)->ctx, OMT_NVM_CAL, &c, sizeof(c));
	if (c.magic != OMT_CAL_MAGIC || c.len != sizeof(c) ||
	    omt_crc32(0, &c, offsetof(struct omt_calib, crc)) != c.crc) {
		nvm_default_calibration(&fw->cal);
		omt_event(fw, EV_CAL_DEFAULTS, c.magic == 0xFFFFFFFFu ? 0 : 1);
		return;
	}
	fw->cal = c;
}

int nvm_save_calibration(struct omt_fw *fw)
{
	struct omt_calib rb;

	fw->cal.magic = OMT_CAL_MAGIC;
	fw->cal.len = sizeof(fw->cal);
	fw->cal.crc = omt_crc32(0, &fw->cal, offsetof(struct omt_calib, crc));
	if (HAL(fw)->nvm_erase(HAL(fw)->ctx, OMT_NVM_CAL, OMT_NVM_SECTOR) < 0 ||
	    HAL(fw)->nvm_write(HAL(fw)->ctx, OMT_NVM_CAL, &fw->cal, sizeof(fw->cal)) < 0) {
		return -1;
	}
	/* read-back verify: a production rule, flash can fail silently */
	HAL(fw)->nvm_read(HAL(fw)->ctx, OMT_NVM_CAL, &rb, sizeof(rb));
	return memcmp(&rb, &fw->cal, sizeof(rb)) == 0 ? 0 : -1;
}

/* ------------------------------ user EEPROM ------------------------- */
#define UEEP_MAGIC 0x50454555u /* "UEEP" */

void nvm_load_user_eeprom(struct omt_fw *fw)
{
	struct {
		uint32_t magic, crc;
		uint8_t data[128];
	} r;

	HAL(fw)->nvm_read(HAL(fw)->ctx, OMT_NVM_USER_EEPROM, &r, sizeof(r));
	if (r.magic == UEEP_MAGIC && omt_crc32(0, r.data, 128) == r.crc) {
		memcpy(fw->m.p03, r.data, 128);
	} else {
		memset(fw->m.p03, 0, 128);
	}
}

void nvm_save_user_eeprom(struct omt_fw *fw)
{
	struct {
		uint32_t magic, crc;
		uint8_t data[128];
	} r;

	r.magic = UEEP_MAGIC;
	memcpy(r.data, fw->m.p03, 128);
	r.crc = omt_crc32(0, r.data, 128);
	HAL(fw)->nvm_erase(HAL(fw)->ctx, OMT_NVM_USER_EEPROM, OMT_NVM_SECTOR);
	HAL(fw)->nvm_write(HAL(fw)->ctx, OMT_NVM_USER_EEPROM, &r, sizeof(r));
}

/* ------------------------------ fault log --------------------------- */
#define FLOG_ENTRY 16u
#define FLOG_PER_SECTOR (OMT_NVM_SECTOR / FLOG_ENTRY)

struct flog_rec {
	uint32_t seq;
	uint32_t t_ms;
	uint16_t code;
	uint16_t arg;
	int16_t temp_q8;
	uint8_t mstate;
	uint8_t crc;
};

static bool flog_valid(const struct flog_rec *r)
{
	return r->seq != 0xFFFFFFFFu && crc8((const uint8_t *)r, FLOG_ENTRY - 1) == r->crc;
}

void flog_init(struct omt_fw *fw)
{
	const uint32_t sect[2] = { OMT_NVM_FAULTLOG_A, OMT_NVM_FAULTLOG_B };
	uint32_t best_seq = 0, best_addr = 0;
	bool any = false;

	fw->flog_count = 0;
	for (int s = 0; s < 2; s++) {
		for (uint32_t i = 0; i < FLOG_PER_SECTOR; i++) {
			struct flog_rec r;
			uint32_t a = sect[s] + i * FLOG_ENTRY;

			HAL(fw)->nvm_read(HAL(fw)->ctx, a, &r, sizeof(r));
			if (!flog_valid(&r)) {
				continue;
			}
			fw->flog_count++;
			if (!any || r.seq > best_seq) {
				best_seq = r.seq;
				best_addr = a;
				any = true;
			}
		}
	}
	if (!any) {
		fw->flog_seq = 1;
		fw->flog_addr = OMT_NVM_FAULTLOG_A;
		return;
	}
	fw->flog_seq = best_seq + 1;
	fw->flog_addr = best_addr + FLOG_ENTRY;
}

void flog_append(struct omt_fw *fw, uint16_t code, uint16_t arg)
{
	struct flog_rec r;
	uint32_t a = fw->flog_addr;
	uint32_t in_sector = a & (OMT_NVM_SECTOR - 1);

	if (in_sector == 0 && a != OMT_NVM_FAULTLOG_A && a != OMT_NVM_FAULTLOG_B) {
		a = OMT_NVM_FAULTLOG_A; /* wrapped past sector B */
	}
	if (a >= OMT_NVM_FAULTLOG_B + OMT_NVM_SECTOR) {
		a = OMT_NVM_FAULTLOG_A;
	}
	if ((a & (OMT_NVM_SECTOR - 1)) == 0) {
		/* entering a sector: it holds the oldest history, recycle it */
		uint8_t probe[4];

		HAL(fw)->nvm_read(HAL(fw)->ctx, a, probe, 4);
		if (probe[0] != 0xFF || probe[1] != 0xFF || probe[2] != 0xFF || probe[3] != 0xFF) {
			HAL(fw)->nvm_erase(HAL(fw)->ctx, a, OMT_NVM_SECTOR);
			fw->flog_count = (uint16_t)(fw->flog_count > FLOG_PER_SECTOR ?
						    fw->flog_count - FLOG_PER_SECTOR : 0);
		}
	}
	r.seq = fw->flog_seq++;
	r.t_ms = omt_now(fw) - fw->boot_ms;
	r.code = code;
	r.arg = arg;
	r.temp_q8 = mon_c_to_cmis(fw->temp_c);
	r.mstate = fw->mstate;
	r.crc = crc8((const uint8_t *)&r, FLOG_ENTRY - 1);
	HAL(fw)->nvm_write(HAL(fw)->ctx, a, &r, sizeof(r));
	fw->flog_addr = a + FLOG_ENTRY;
	fw->flog_count++;
}

/* idx 0 = newest. out: seq(4) t_ms(4) code(2) arg(2) temp(2) state(1) pad(1) */
int flog_read(struct omt_fw *fw, uint16_t idx, uint8_t out[16])
{
	uint32_t want = fw->flog_seq - 1u - idx;
	const uint32_t sect[2] = { OMT_NVM_FAULTLOG_A, OMT_NVM_FAULTLOG_B };

	if (idx >= fw->flog_count || fw->flog_seq <= 1u + idx) {
		return -1;
	}
	for (int s = 0; s < 2; s++) {
		for (uint32_t i = 0; i < FLOG_PER_SECTOR; i++) {
			struct flog_rec r;

			HAL(fw)->nvm_read(HAL(fw)->ctx, sect[s] + i * FLOG_ENTRY, &r, sizeof(r));
			if (flog_valid(&r) && r.seq == want) {
				memcpy(out, &r, 16);
				return 0;
			}
		}
	}
	return -1;
}

/* ------------------------------ boot record ------------------------- */
#define BOOT_SLOTS (OMT_NVM_SECTOR / sizeof(struct omt_boot_rec))

int boot_load(struct omt_fw *fw)
{
	bool found = false;

	for (uint32_t i = 0; i < BOOT_SLOTS; i++) {
		struct omt_boot_rec r;

		HAL(fw)->nvm_read(HAL(fw)->ctx, OMT_NVM_BOOT + i * (uint32_t)sizeof(r), &r, sizeof(r));
		if (r.magic != OMT_BOOT_MAGIC ||
		    omt_crc32(0, &r, offsetof(struct omt_boot_rec, crc)) != r.crc) {
			continue;
		}
		if (!found || r.seq > fw->boot.seq) {
			fw->boot = r;
			found = true;
		}
	}
	if (!found) {
		memset(&fw->boot, 0, sizeof(fw->boot));
		fw->boot.magic = OMT_BOOT_MAGIC;
		fw->boot.trial_bank = 0xFF;
		return -1;
	}
	return 0;
}

int boot_store(struct omt_fw *fw)
{
	struct omt_boot_rec r = fw->boot;
	uint32_t slot = r.seq + 1;

	r.magic = OMT_BOOT_MAGIC;
	r.seq = slot;
	r.crc = omt_crc32(0, &r, offsetof(struct omt_boot_rec, crc));
	slot %= BOOT_SLOTS;
	if (slot == 0) {
		HAL(fw)->nvm_erase(HAL(fw)->ctx, OMT_NVM_BOOT, OMT_NVM_SECTOR);
	}
	fw->boot = r;
	return HAL(fw)->nvm_write(HAL(fw)->ctx, OMT_NVM_BOOT + slot * (uint32_t)sizeof(r), &r, sizeof(r));
}
