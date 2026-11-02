/*
 * CMIS memory map used by the Optical Module Twin firmware.
 *
 * Layout modeled on CMIS 5.x (OIF-CMIS) for a paged-memory QSFP-DD module
 * with one bank (8 host lanes, 4 media lanes). Byte offsets marked
 * "vendor" are specific to this design. Verify every offset against the
 * current OIF-CMIS specification before reusing this file for a product.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OMT_CMIS_MAP_H
#define OMT_CMIS_MAP_H

/* ---------------- Lower memory (bytes 0..127, page independent) ------- */
#define CMIS_ID                     0   /* SFF-8024 identifier */
#define CMIS_REV                    1   /* CMIS revision, BCD-ish 0x52 = 5.2 */
#define CMIS_MEM_CHAR               2   /* bit7: flat memory */
#define CMIS_MOD_STATE              3   /* bits3..1 module state, bit0 IntL deasserted */
#define CMIS_FLAG_MODULE            8   /* latched module-level flags */
#define   CMIS_F_STATE_CHANGED      0x01
#define   CMIS_F_DP_FW_FAULT        0x02
#define   CMIS_F_MOD_FW_FAULT       0x04
#define   CMIS_F_CDB1_COMPLETE      0x40
#define CMIS_FLAG_TEMP_VCC          9   /* b0 THA b1 TLA b2 THW b3 TLW, b4..7 same for Vcc */
#define CMIS_FLAG_AUX12            10   /* Aux1 bits0..3, Aux2 bits4..7 */
#define CMIS_FLAG_AUX3             11
#define CMIS_MON_TEMP              14   /* S16, 1/256 degC */
#define CMIS_MON_VCC               16   /* U16, 100 uV */
#define CMIS_MON_AUX1              18   /* S16, laser temperature 1/256 degC */
#define CMIS_MON_AUX2              20   /* S16, TEC drive, 32767 = +100 % */
#define CMIS_MON_AUX3              22
#define CMIS_MOD_CTRL              26
#define   CMIS_CTRL_SW_RESET        0x08
#define   CMIS_CTRL_LOWPWR_SW       0x10
#define   CMIS_CTRL_LOWPWR_ALLOW_HW 0x40
#define CMIS_MASK_MODULE           31   /* masks for bytes 8..11 at 31..34 */
#define CMIS_MASK_TEMP_VCC         32
#define CMIS_MASK_AUX12            33
#define CMIS_MASK_AUX3             34
#define CMIS_CDB_STATUS1           37
#define   CDB_STS_BUSY              0x80
#define   CDB_STS_FAIL              0x40
#define   CDB_SUCCESS               0x01
#define   CDB_BUSY_PROCESSING       (CDB_STS_BUSY | 0x01)
#define   CDB_FAIL_UNSUPPORTED      (CDB_STS_FAIL | 0x01)
#define   CDB_FAIL_PARAM            (CDB_STS_FAIL | 0x02)
#define   CDB_FAIL_STATE            (CDB_STS_FAIL | 0x03)
#define   CDB_FAIL_CHKCODE          (CDB_STS_FAIL | 0x05)
#define   CDB_FAIL_IMAGE            (CDB_STS_FAIL | 0x07)
#define CMIS_FW_ACTIVE_MAJOR       39
#define CMIS_FW_ACTIVE_MINOR       40
#define CMIS_FAULT_CAUSE           41   /* vendor use in this design */
#define CMIS_MEDIA_TYPE            85
#define CMIS_APP_DESC              86   /* 8 x 4 bytes */
#define CMIS_APP_DESC_LEN           4
#define CMIS_PWD_CHANGE           118
#define CMIS_PWD_ENTRY            122
#define CMIS_BANK_SELECT          126
#define CMIS_PAGE_SELECT          127

/* Module states (byte 3 bits 3..1) */
enum cmis_module_state {
	CMIS_MS_LOWPWR = 1,
	CMIS_MS_PWRUP  = 2,
	CMIS_MS_READY  = 3,
	CMIS_MS_PWRDN  = 4,
	CMIS_MS_FAULT  = 5,
};

/* Data path states (Page 11h, 4 bits per lane) */
enum cmis_dp_state {
	CMIS_DP_DEACTIVATED = 1,
	CMIS_DP_INIT        = 2,
	CMIS_DP_DEINIT      = 3,
	CMIS_DP_ACTIVATED   = 4,
	CMIS_DP_TXTURNON    = 5,
	CMIS_DP_TXTURNOFF   = 6,
	CMIS_DP_INITIALIZED = 7,
};

/* Config status codes (Page 11h, 4 bits per lane) */
enum cmis_cfg_status {
	CMIS_CFG_UNDEFINED      = 0x0,
	CMIS_CFG_SUCCESS        = 0x1,
	CMIS_CFG_REJECTED       = 0x2,
	CMIS_CFG_REJ_APPSEL     = 0x3,
	CMIS_CFG_REJ_DATAPATH   = 0x4,
	CMIS_CFG_REJ_IN_USE     = 0x6,
	CMIS_CFG_REJ_PARTIAL_DP = 0x7,
	CMIS_CFG_IN_PROGRESS    = 0xC,
};

/* ---------------- Page 00h (upper) ---------------- */
#define P00_ID                    128
#define P00_VENDOR_NAME           129  /* 16 */
#define P00_VENDOR_OUI            145  /* 3 */
#define P00_VENDOR_PN             148  /* 16 */
#define P00_VENDOR_REV            164  /* 2 */
#define P00_VENDOR_SN             166  /* 16 */
#define P00_DATE_CODE             182  /* 8 */
#define P00_CLEI                  190  /* 10 */
#define P00_POWER_CLASS           200  /* bits 7..5 */
#define P00_MAX_POWER             201  /* 0.25 W units */
#define P00_CONNECTOR             203
#define P00_CHECKSUM              222  /* sum 128..221 */

/* ---------------- Page 01h (advertising) ---------------- */
#define P01_FW_INACTIVE_MAJOR     128
#define P01_FW_INACTIVE_MINOR     129
#define P01_HW_REV_MAJOR          130
#define P01_HW_REV_MINOR          131
#define P01_WAVELENGTH            138  /* U16, 0.05 nm */
#define P01_WAVELENGTH_TOL        140  /* U16, 0.005 nm */
#define P01_PAGES_SUPPORTED       142
#define P01_DUR_DP                144  /* bits3..0 DPInit, bits7..4 DPDeinit */
#define P01_MOD_MON_SUPPORT       159  /* b0 temp b1 vcc b2 aux1 b3 aux2 */
#define P01_LANE_MON_SUPPORT      160  /* b0 bias b1 txpwr b2 rxpwr, b4..3 bias scale */
#define P01_DUR_MODULE            167  /* bits3..0 PwrUp, bits7..4 PwrDn */
#define P01_CDB_SUPPORT           163  /* vendor use in this design */
#define P01_CHECKSUM              255  /* sum 130..254 */

/* ---------------- Page 02h (thresholds, RO) ---------------- */
#define P02_TEMP                  128  /* HA LA HW LW, 2 bytes each */
#define P02_VCC                   136
#define P02_AUX1                  144
#define P02_AUX2                  152
#define P02_AUX3                  160
#define P02_TXPWR                 176
#define P02_TXBIAS                184
#define P02_RXPWR                 192
#define P02_CHECKSUM              255  /* sum 128..254 */

/* ---------------- Page 03h user EEPROM (RW, persisted) ---------------- */

/* ---------------- Page 10h (lane control, bank 0) ---------------- */
#define P10_DP_DEINIT             128
#define P10_TX_POL_FLIP           129
#define P10_TX_DISABLE            130
#define P10_TX_SQ_DISABLE         131
#define P10_APPLY_DPINIT          143
#define P10_APPLY_IMMEDIATE       144
#define P10_DPCONFIG              145  /* 8 bytes: AppSel<<4 | DPID<<1 | Explicit */
#define P10_MASK_BASE             213  /* masks for Page 11h flags 134..152 */

/* ---------------- Page 11h (lane status, bank 0) ---------------- */
#define P11_DP_STATE              128  /* 4 bytes, nibble per lane */
#define P11_OUT_STATUS_RX         132
#define P11_OUT_STATUS_TX         133
#define P11_FLAG_DP_CHANGED       134
#define P11_FLAG_TX_FAULT         135
#define P11_FLAG_TX_LOS           136
#define P11_FLAG_TX_LOL           137
#define P11_FLAG_TX_ADAPT         138
#define P11_FLAG_TXPWR_HA         139
#define P11_FLAG_TXPWR_LA         140
#define P11_FLAG_TXPWR_HW         141
#define P11_FLAG_TXPWR_LW         142
#define P11_FLAG_BIAS_HA          143
#define P11_FLAG_BIAS_LA          144
#define P11_FLAG_BIAS_HW          145
#define P11_FLAG_BIAS_LW          146
#define P11_FLAG_RX_LOS           147
#define P11_FLAG_RX_LOL           148
#define P11_FLAG_RXPWR_HA         149
#define P11_FLAG_RXPWR_LA         150
#define P11_FLAG_RXPWR_HW         151
#define P11_FLAG_RXPWR_LW         152
#define P11_FLAG_FIRST            P11_FLAG_DP_CHANGED
#define P11_FLAG_LAST             P11_FLAG_RXPWR_LW
#define P11_MON_TXPWR             154  /* 8 x U16, 0.1 uW */
#define P11_MON_BIAS              170  /* 8 x U16, 2 uA */
#define P11_MON_RXPWR             186  /* 8 x U16, 0.1 uW */
#define P11_CFG_STATUS            202  /* 4 bytes, nibble per lane */
#define P11_ACTIVE_DPCONFIG       206  /* 8 bytes */

/* ---------------- Page 9Fh (CDB, LPL) ---------------- */
#define P9F_CMD                   128  /* U16 */
#define P9F_EPL_LEN               130
#define P9F_LPL_LEN               132
#define P9F_CHKCODE               133
#define P9F_RPL_LEN               134
#define P9F_RPL_CHK               135
#define P9F_PAYLOAD               136  /* 120 bytes */
#define P9F_PAYLOAD_MAX           120

/* ---------------- Vendor pages (this design) ---------------- */
#define PAGE_VENDOR_DIAG         0xB0  /* RO firmware diagnostics */
#define PAGE_VENDOR_CAL          0xB1  /* factory calibration, password */

#define PB0_UPTIME_S              128  /* U32 */
#define PB0_RESET_COUNT           132  /* U16 */
#define PB0_RESET_REASON          134
#define PB0_BOOT_BANK             135
#define PB0_FAULTLOG_COUNT        136  /* U16 */
#define PB0_I2C_BAD_PAGE          138  /* U16 */
#define PB0_I2C_RO_WRITE          140  /* U16 */
#define PB0_DSP_RECOVERIES        142  /* U16 */
#define PB0_PREFEC_BER            144  /* 4 x float32 BE */
#define PB0_SNR                   160  /* 4 x U16, 0.01 dB */
#define PB0_FEC_CORR              168  /* 4 x U32 (since last read window) */
#define PB0_FEC_UNCORR            184  /* 4 x U32 cumulative */
#define PB0_TEC_SETPOINT          200  /* S16 1/256 degC */
#define PB0_APC_STATE             202  /* bit per lane: converged */
#define PB0_DSP_TEMP              204  /* S16 0.1 degC */
#define PB0_LAST_FAULT            206  /* U16 fault code */

#define PB1_RAW_TEMP              128  /* U16 raw ADC */
#define PB1_RAW_VCC               130
#define PB1_RAW_RSSI              132  /* 4 x U16 */
#define PB1_RAW_MPD               140  /* 4 x U16 */
#define PB1_COEF                  160  /* calibration block, see calib.h */
#define PB1_COMMAND               255  /* 0xA5 commit, 0x5A reload, 0xC3 defaults */

/* Supported pages bitmap helper */
#define OMT_N_HOST_LANES 8
#define OMT_N_MEDIA_LANES 4

#endif /* OMT_CMIS_MAP_H */
