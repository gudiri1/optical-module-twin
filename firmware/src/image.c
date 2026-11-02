/*
 * Firmware images, A/B banks and fail-safe boot selection.
 *
 * Boot policy:
 *   - CDB "Run" stores trial_bank = X, attempts = 0 and resets.
 *   - Boot sees a trial with attempts == 0: attempts = 1, boot X.
 *   - Boot sees a trial with attempts >= 1: the trial never confirmed
 *     (watchdog, crash, power loss) -> roll back to the committed bank.
 *   - 2 s of healthy operation confirm the boot; the trial is cleared, so a
 *     later reset returns to the committed image unless the host commits.
 *   - An image failing header/payload CRC is never booted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "omt/fw.h"

#define CONFIRM_MS 2000u

static uint32_t bank_addr(uint8_t bank)
{
	return bank ? OMT_NVM_IMAGE_B : OMT_NVM_IMAGE_A;
}

void img_default_tunables(struct omt_tunables *t)
{
	t->apc_ki = 0.010f;
	t->tec_kp = 0.35f;
	t->tec_ki = 0.08f;
	t->tec_setpoint_c = 50.0f;
	t->los_assert_dbm = -14.0f;
	t->los_deassert_dbm = -13.0f;
}

size_t omt_img_build(uint8_t *buf, size_t cap, uint8_t major, uint8_t minor, uint16_t build,
		     uint32_t flags, const char *tag, const struct omt_tunables *tun)
{
	struct omt_img_hdr h;
	size_t plen = sizeof(*tun) + 256; /* tunables + filler "code" */

	if (cap < sizeof(h) + plen) {
		return 0;
	}
	uint8_t *pl = buf + sizeof(h);

	memcpy(pl, tun, sizeof(*tun));
	for (size_t i = sizeof(*tun); i < plen; i++) {
		pl[i] = (uint8_t)(i * 37u + major * 11u + minor);
	}
	memset(&h, 0, sizeof(h));
	h.magic = OMT_IMG_MAGIC;
	h.ver_major = major;
	h.ver_minor = minor;
	h.build = build;
	h.payload_len = (uint32_t)plen;
	h.payload_crc = omt_crc32(0, pl, plen);
	h.flags = flags;
	memcpy(h.tag, tag, strlen(tag) < sizeof(h.tag) ? strlen(tag) : sizeof(h.tag));
	h.hdr_crc = omt_crc32(0, &h, offsetof(struct omt_img_hdr, hdr_crc));
	memcpy(buf, &h, sizeof(h));
	return sizeof(h) + plen;
}

int img_validate(struct omt_fw *fw, uint8_t bank, struct omt_img_hdr *hdr)
{
	const struct omt_hal *hal = fw->hal;
	uint32_t base = bank_addr(bank);
	uint32_t crc = 0;
	uint8_t chunk[64];

	hal->nvm_read(hal->ctx, base, hdr, sizeof(*hdr));
	if (hdr->magic != OMT_IMG_MAGIC ||
	    omt_crc32(0, hdr, offsetof(struct omt_img_hdr, hdr_crc)) != hdr->hdr_crc ||
	    hdr->payload_len == 0 || hdr->payload_len > OMT_NVM_IMAGE_SIZE - sizeof(*hdr)) {
		return -1;
	}
	for (uint32_t off = 0; off < hdr->payload_len; off += sizeof(chunk)) {
		uint32_t n = hdr->payload_len - off;

		n = n > sizeof(chunk) ? sizeof(chunk) : n;
		hal->nvm_read(hal->ctx, base + (uint32_t)sizeof(*hdr) + off, chunk, n);
		crc = omt_crc32(crc, chunk, n);
	}
	return crc == hdr->payload_crc ? 0 : -2;
}

static void publish_inactive(struct omt_fw *fw)
{
	struct omt_img_hdr h;
	uint8_t other = fw->running_bank ^ 1u;

	if (img_validate(fw, other, &h) == 0) {
		fw->m.p01[P01_FW_INACTIVE_MAJOR - 128] = h.ver_major;
		fw->m.p01[P01_FW_INACTIVE_MINOR - 128] = h.ver_minor;
	} else {
		fw->m.p01[P01_FW_INACTIVE_MAJOR - 128] = 0;
		fw->m.p01[P01_FW_INACTIVE_MINOR - 128] = 0;
	}
	cmis_update_checksums(fw);
}

void img_boot(struct omt_fw *fw)
{
	struct omt_img_hdr h;
	uint8_t cause = fw->hal->reset_cause(fw->hal->ctx);
	uint8_t bank;
	bool rollback = false;

	(void)boot_load(fw);
	fw->boot.reset_count++;
	fw->boot.last_reset_reason = cause;

	bank = fw->boot.committed_bank & 1u;
	if (fw->boot.trial_bank != 0xFF) {
		if (fw->boot.trial_attempts == 0) {
			fw->boot.trial_attempts = 1;
			bank = fw->boot.trial_bank & 1u;
		} else {
			rollback = true;
			fw->boot.trial_bank = 0xFF;
			fw->boot.trial_attempts = 0;
		}
	}

	img_default_tunables(&fw->tun);
	if (img_validate(fw, bank, &h) != 0) {
		omt_event(fw, EV_IMAGE_INVALID, bank);
		bank ^= 1u;
		if (img_validate(fw, bank, &h) != 0) {
			omt_event(fw, EV_IMAGE_INVALID, bank);
			memset(&h, 0, sizeof(h));
			h.ver_major = OMT_FW_VERSION_MAJOR;
			h.ver_minor = OMT_FW_VERSION_MINOR;
			bank = fw->boot.committed_bank & 1u;
		}
	}
	if (h.magic == OMT_IMG_MAGIC && h.payload_len >= sizeof(struct omt_tunables)) {
		fw->hal->nvm_read(fw->hal->ctx, bank_addr(bank) + sizeof(h), &fw->tun, sizeof(fw->tun));
	}
	fw->running_bank = bank;
	fw->running_major = h.ver_major;
	fw->running_minor = h.ver_minor;
	fw->running_build = h.build;
	fw->hung = (h.flags & OMT_IMG_FLAG_CRASH_ON_BOOT) != 0;

	(void)boot_store(fw);

	if (cause == OMT_RST_WATCHDOG) {
		omt_event(fw, EV_WDT_RESET, fw->boot.reset_count);
	}
	if (rollback) {
		omt_event(fw, EV_ROLLBACK, bank);
	}
	omt_event(fw, EV_BOOT, (uint16_t)((h.ver_major << 8) | h.ver_minor));
}

void img_confirm_task(struct omt_fw *fw)
{
	if (fw->boot_confirmed || omt_now(fw) - fw->boot_ms < CONFIRM_MS) {
		return;
	}
	fw->boot_confirmed = true;
	if (fw->boot.trial_bank != 0xFF) {
		fw->boot.trial_bank = 0xFF;
		fw->boot.trial_attempts = 0;
		(void)boot_store(fw);
	}
	omt_log(fw, EV_BOOT_CONFIRMED, fw->running_bank);
	publish_inactive(fw);
}

/* exported for cmis_mem init ordering */
void img_publish_versions(struct omt_fw *fw)
{
	fw->m.lower[CMIS_FW_ACTIVE_MAJOR] = fw->running_major;
	fw->m.lower[CMIS_FW_ACTIVE_MINOR] = fw->running_minor;
	publish_inactive(fw);
}
