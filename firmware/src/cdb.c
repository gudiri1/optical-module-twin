/*
 * CMIS Command Data Block (CDB), instance 1, LPL only (Page 9Fh).
 *
 * Host protocol: write LPL payload to 136.., then header bytes 130..135,
 * then CMDID 128..129 (the write of byte 129 triggers execution).
 * CdbStatus1 (byte 37) is busy until completion, then the result is
 * posted and the latched CdbCmdComplete flag (byte 8 bit 6) raises IntL.
 *
 * CdbChkCode (byte 133) = ones' complement of the 8-bit sum of bytes
 * 128..132 and the LPL payload.
 *
 * Implemented commands (firmware management subset):
 *   0000h Query status          0040h FW management features
 *   0100h Get firmware info     0101h Start FW download (size BE32)
 *   0102h Abort FW download     0103h Write FW block LPL (offset BE32 + data)
 *   0107h Complete FW download  0109h Run FW image   010Ah Commit FW image
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "omt/fw.h"

#define CMD_QUERY      0x0000
#define CMD_FEATURES   0x0040
#define CMD_FW_INFO    0x0100
#define CMD_DL_START   0x0101
#define CMD_DL_ABORT   0x0102
#define CMD_DL_WRITE   0x0103
#define CMD_DL_DONE    0x0107
#define CMD_RUN        0x0109
#define CMD_COMMIT     0x010A

uint8_t cdb_chkcode(const uint8_t *p9f)
{
	uint8_t s = 0;
	uint8_t lpl = p9f[P9F_LPL_LEN - 128];

	if (lpl > P9F_PAYLOAD_MAX) {
		lpl = P9F_PAYLOAD_MAX;
	}
	for (int i = P9F_CMD; i <= P9F_LPL_LEN; i++) {
		s = (uint8_t)(s + p9f[i - 128]);
	}
	for (int i = 0; i < lpl; i++) {
		s = (uint8_t)(s + p9f[P9F_PAYLOAD - 128 + i]);
	}
	return (uint8_t)~s;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t bank_addr(uint8_t bank)
{
	return bank ? OMT_NVM_IMAGE_B : OMT_NVM_IMAGE_A;
}

static uint8_t fw_info(struct omt_fw *fw, uint8_t *r)
{
	struct omt_img_hdr h[2];
	int ok[2];
	uint8_t st = 0;

	memset(r, 0, 74);
	for (uint8_t b = 0; b < 2; b++) {
		ok[b] = img_validate(fw, b, &h[b]) == 0;
		uint8_t sh = b ? 4 : 0;

		if (fw->running_bank == b) {
			st |= (uint8_t)(0x1 << sh);
		}
		if ((fw->boot.committed_bank & 1u) == b) {
			st |= (uint8_t)(0x2 << sh);
		}
		if (!ok[b]) {
			st |= (uint8_t)(0x4 << sh);
		}
		uint8_t *d = &r[2 + 36 * b];

		if (ok[b]) {
			d[0] = h[b].ver_major;
			d[1] = h[b].ver_minor;
			d[2] = (uint8_t)(h[b].build >> 8);
			d[3] = (uint8_t)h[b].build;
			memcpy(&d[4], h[b].tag, 16);
		}
	}
	r[0] = st;
	r[1] = 0x03; /* both images described */
	return 74;
}

static uint8_t execute(struct omt_fw *fw, uint16_t cmd, const uint8_t *lpl, uint8_t lpl_len,
		       uint8_t *rpl, uint8_t *rpl_len, uint32_t *busy_ms)
{
	struct omt_cdb *c = &fw->cdb;

	*rpl_len = 0;
	*busy_ms = 1;
	switch (cmd) {
	case CMD_QUERY:
		rpl[0] = 0;
		*rpl_len = 1;
		return CDB_SUCCESS;

	case CMD_FEATURES:
		memset(rpl, 0, 12);
		rpl[1] = 0x01;              /* commit required after run          */
		rpl[2] = 8;                 /* start command payload size         */
		rpl[3] = 0xFF;              /* erased byte value                  */
		rpl[4] = 0x01;              /* write mechanism: LPL               */
		rpl[5] = P9F_PAYLOAD_MAX - 4; /* max data per write block         */
		*rpl_len = 12;
		return CDB_SUCCESS;

	case CMD_FW_INFO:
		*rpl_len = fw_info(fw, rpl);
		return CDB_SUCCESS;

	case CMD_DL_START: {
		if (lpl_len < 8) {
			return CDB_FAIL_PARAM;
		}
		uint32_t size = be32(lpl);

		if (size < sizeof(struct omt_img_hdr) || size > OMT_NVM_IMAGE_SIZE) {
			return CDB_FAIL_PARAM;
		}
		c->dl_bank = fw->running_bank ^ 1u;
		c->dl_size = size;
		c->dl_written = 0;
		c->dl_active = true;
		c->dl_complete = false;
		fw->hal->nvm_erase(fw->hal->ctx, bank_addr(c->dl_bank), OMT_NVM_IMAGE_SIZE);
		*busy_ms = 40;              /* sector erase time                  */
		return CDB_SUCCESS;
	}

	case CMD_DL_ABORT:
		c->dl_active = false;
		return CDB_SUCCESS;

	case CMD_DL_WRITE: {
		if (!c->dl_active) {
			return CDB_FAIL_STATE;
		}
		if (lpl_len < 5) {
			return CDB_FAIL_PARAM;
		}
		uint32_t off = be32(lpl);
		uint32_t n = (uint32_t)lpl_len - 4u;

		if (off + n > c->dl_size) {
			return CDB_FAIL_PARAM;
		}
		fw->hal->nvm_write(fw->hal->ctx, bank_addr(c->dl_bank) + off, lpl + 4, n);
		c->dl_written += n;
		*busy_ms = 2;
		return CDB_SUCCESS;
	}

	case CMD_DL_DONE: {
		struct omt_img_hdr h;

		if (!c->dl_active) {
			return CDB_FAIL_STATE;
		}
		c->dl_active = false;
		if (img_validate(fw, c->dl_bank, &h) != 0) {
			return CDB_FAIL_IMAGE;
		}
		c->dl_complete = true;
		omt_event(fw, EV_FW_DOWNLOADED, (uint16_t)((h.ver_major << 8) | h.ver_minor));
		img_publish_versions(fw);
		*busy_ms = 10;
		return CDB_SUCCESS;
	}

	case CMD_RUN: {
		struct omt_img_hdr h;
		uint8_t target = fw->running_bank ^ 1u;

		if (c->dl_active || img_validate(fw, target, &h) != 0) {
			return CDB_FAIL_STATE;
		}
		fw->boot.trial_bank = target;
		fw->boot.trial_attempts = 0;
		(void)boot_store(fw);
		fw->pending_reset_t = omt_now(fw) + 20; /* let the host see success */
		return CDB_SUCCESS;
	}

	case CMD_COMMIT: {
		struct omt_img_hdr h;

		if (!fw->boot_confirmed || img_validate(fw, fw->running_bank, &h) != 0) {
			return CDB_FAIL_STATE;
		}
		fw->boot.committed_bank = fw->running_bank;
		fw->boot.trial_bank = 0xFF;
		(void)boot_store(fw);
		omt_event(fw, EV_FW_COMMITTED, fw->running_bank);
		img_publish_versions(fw);
		return CDB_SUCCESS;
	}

	default:
		return CDB_FAIL_UNSUPPORTED;
	}
}

static void complete(struct omt_fw *fw)
{
	struct omt_cdb *c = &fw->cdb;
	uint8_t *p = fw->m.p9f;
	uint8_t s = 0;

	fw->hal->irq_lock(fw->hal->ctx);
	if (c->rpl_len) {
		memcpy(&p[P9F_PAYLOAD - 128], c->rpl, c->rpl_len);
		for (int i = 0; i < c->rpl_len; i++) {
			s = (uint8_t)(s + c->rpl[i]);
		}
	}
	p[P9F_RPL_LEN - 128] = c->rpl_len;
	p[P9F_RPL_CHK - 128] = (uint8_t)~s;
	fw->m.lower[CMIS_CDB_STATUS1] = c->result;
	fw->m.lower[CMIS_FLAG_MODULE] |= CMIS_F_CDB1_COMPLETE;
	cmis_update_intl(fw);
	fw->hal->irq_unlock(fw->hal->ctx);
	c->busy = false;
}

void cdb_task(struct omt_fw *fw)
{
	struct omt_cdb *c = &fw->cdb;

	if (c->busy) {
		if ((int32_t)(omt_now(fw) - c->busy_until) >= 0) {
			complete(fw);
		}
		return;
	}
	if (!(fw->m.dirty & DIRTY_CDB)) {
		return;
	}

	uint8_t lpl[P9F_PAYLOAD_MAX];
	uint8_t lpl_len, chk_ok;
	uint32_t busy_ms = 1;

	fw->hal->irq_lock(fw->hal->ctx);
	fw->m.dirty &= ~DIRTY_CDB;
	c->cmd = cmis_get_u16(&fw->m.p9f[P9F_CMD - 128]);
	lpl_len = fw->m.p9f[P9F_LPL_LEN - 128];
	chk_ok = cdb_chkcode(fw->m.p9f) == fw->m.p9f[P9F_CHKCODE - 128];
	if (lpl_len > P9F_PAYLOAD_MAX) {
		lpl_len = P9F_PAYLOAD_MAX;
	}
	memcpy(lpl, &fw->m.p9f[P9F_PAYLOAD - 128], lpl_len);
	fw->hal->irq_unlock(fw->hal->ctx);

	c->busy = true;
	if (!chk_ok) {
		c->result = CDB_FAIL_CHKCODE;
		c->rpl_len = 0;
	} else {
		c->result = execute(fw, c->cmd, lpl, lpl_len, c->rpl, &c->rpl_len, &busy_ms);
	}
	c->busy_until = omt_now(fw) + busy_ms;
	omt_log(fw, 0x300, c->cmd);
}
