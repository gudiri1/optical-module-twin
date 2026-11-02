/*
 * Optical Module Twin - module controller firmware, public interface.
 *
 * One `struct omt_fw` holds the complete firmware state, so several
 * simulated modules can run in one process (Monte-Carlo manufacturing
 * runs) and the state is trivially reset on a watchdog/software reset.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OMT_FW_H
#define OMT_FW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omt/cmis_map.h"
#include "omt/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

struct omt_fw;

#define OMT_FW_VERSION_MAJOR 1
#define OMT_FW_VERSION_MINOR 0

/* ------------------------------------------------------------------ */
/* Fault / event codes (fault log, console, vendor page B0h)           */
/* ------------------------------------------------------------------ */
enum omt_event {
	EV_NONE              = 0x00,
	EV_BOOT              = 0x01,
	EV_WDT_RESET         = 0x02,
	EV_ROLLBACK          = 0x03,
	EV_SW_RESET          = 0x04,
	EV_IMAGE_INVALID     = 0x05,
	EV_BOOT_CONFIRMED    = 0x06,
	EV_DSP_NO_RESPONSE   = 0x10,
	EV_DSP_INIT_TIMEOUT  = 0x11,
	EV_DSP_HANG          = 0x12,
	EV_DSP_RECOVERED     = 0x13,
	EV_MDIO_ERROR        = 0x14,
	EV_LASER_EOL         = 0x20,
	EV_LASER_DRV_FAULT   = 0x21,
	EV_EYE_SAFETY        = 0x22,
	EV_TEC_SATURATED     = 0x30,
	EV_LASER_OVERTEMP    = 0x31,
	EV_MODULE_OVERTEMP   = 0x40,
	EV_VCC_UNDERVOLT     = 0x41,
	EV_NVM_CRC           = 0x50,
	EV_CAL_DEFAULTS      = 0x51,
	EV_CAL_COMMITTED     = 0x52,
	EV_FW_DOWNLOADED     = 0x60,
	EV_FW_COMMITTED      = 0x61,
};

/* Module fault cause (lower memory byte 41, vendor use) */
enum omt_fault_cause {
	FC_NONE = 0, FC_DSP = 1, FC_LASER_OVERTEMP = 2, FC_MODULE_OVERTEMP = 3,
	FC_VCC = 4, FC_LASER_DRIVER = 5,
};

/* ------------------------------------------------------------------ */
/* Persistent records                                                  */
/* ------------------------------------------------------------------ */
#define OMT_CAL_MAGIC 0x4C41434Fu /* "OCAL" */
struct omt_calib {
	uint32_t magic;
	uint16_t version;
	uint16_t len;
	float temp_offset_c;          /* added after conversion            */
	float vcc_gain;               /* multiplies converted Vcc           */
	float rssi_slope[4];          /* uW per ADC count                   */
	float rssi_offset[4];         /* ADC counts (dark level)            */
	float mpd_slope[4];           /* uW per MPD count                   */
	float laser_temp_offset_c;
	float tx_target_dbm;          /* APC target launch power            */
	char serial[16];              /* factory identity                   */
	char date_code[8];
	uint32_t crc;                 /* CRC32 over all preceding bytes     */
};

/* Factory password unlocking vendor page B1h (CMIS password entry 122..125) */
#define OMT_FACTORY_PASSWORD 0x4F4D5401u

#define OMT_BOOT_MAGIC 0x544F4F42u /* "BOOT" */
struct omt_boot_rec {
	uint32_t magic;
	uint32_t seq;                 /* highest seq wins (log-structured)  */
	uint8_t committed_bank;       /* 0 = A, 1 = B                       */
	uint8_t trial_bank;           /* 0xFF = none                        */
	uint8_t trial_attempts;
	uint8_t last_reset_reason;
	uint16_t reset_count;
	uint16_t reserved;
	uint32_t crc;
};

/* Firmware image (stored in bank A/B), little endian */
#define OMT_IMG_MAGIC 0x49544D4Fu /* "OMTI" */
#define OMT_IMG_FLAG_CRASH_ON_BOOT 0x00000001u /* test hook */
struct omt_img_hdr {
	uint32_t magic;
	uint8_t ver_major;
	uint8_t ver_minor;
	uint16_t build;
	uint32_t payload_len;
	uint32_t payload_crc;
	uint32_t flags;
	char tag[16];
	uint32_t hdr_crc;             /* CRC32 over preceding 36 bytes      */
};

/* Tunables carried in the image payload */
struct omt_tunables {
	float apc_ki;                 /* bias mA per (uW error) per step    */
	float tec_kp;
	float tec_ki;
	float tec_setpoint_c;
	float los_assert_dbm;
	float los_deassert_dbm;
};

/* ------------------------------------------------------------------ */
/* Runtime state                                                       */
/* ------------------------------------------------------------------ */
#define OMT_LOG_LEN 64
struct omt_log_entry {
	uint32_t t_ms;
	uint16_t code;
	uint16_t arg;
};

struct omt_threshold {
	int32_t ha, la, hw, lw;       /* CMIS-encoded units                 */
	int32_t hyst;                 /* hysteresis in the same units       */
};

struct omt_tx_lane {
	float bias_cmd_ma;
	float bias_meas_ma;
	float tx_uw;                  /* calibrated launch power            */
	float target_uw;
	uint16_t mpd_raw;
	bool enabled;                 /* laser driver channel on            */
	bool squelched;               /* output squelched (DP not active)   */
	bool converged;
	bool armed;                   /* alarms armed after first convergence */
	bool fault;
	uint16_t sat_ms;              /* time at bias limit below target    */
	uint16_t conv_ms;
};

struct omt_rx_lane {
	float rx_uw;
	uint16_t rssi_raw;
	bool los;
	bool lol;                     /* media-side CDR loss of lock        */
	bool host_lol;                /* host-side CDR loss of lock         */
	float snr_db;
	float prefec_ber;
	uint32_t fec_corr;
	uint32_t fec_uncorr;
};

struct omt_dp {
	uint8_t host_mask;            /* host lanes in this data path       */
	uint8_t media_mask;           /* media (optical) lanes              */
	uint8_t appsel;
	uint8_t state;                /* enum cmis_dp_state                 */
	uint32_t t_enter;
	bool used;
};

struct omt_cmis {
	uint8_t lower[128];
	uint8_t p00[128], p01[128], p02[128], p03[128];
	uint8_t p10[128], p11[128], p9f[128], pb0[128], pb1[128];
	uint8_t addr;                 /* I2C byte address pointer           */
	bool cal_unlocked;
	uint32_t dirty;
	uint16_t bad_page;
	uint16_t ro_write;
	uint8_t cond_module[4];       /* current (unlatched) conditions of bytes 8..11 */
	uint8_t cond_lane[P11_FLAG_LAST - P11_FLAG_FIRST + 1];
	uint32_t user_eeprom_dirty_ms;
};

/* dirty bits: side effects of host writes, consumed by tasks */
#define DIRTY_MODCTRL      0x0001u
#define DIRTY_LANECTRL     0x0002u
#define DIRTY_APPLY_DPINIT 0x0004u
#define DIRTY_CDB          0x0008u
#define DIRTY_CAL_CMD      0x0010u
#define DIRTY_USER_EEPROM  0x0020u
#define DIRTY_PASSWORD     0x0040u

struct omt_cdb {
	uint16_t cmd;
	bool busy;
	uint32_t busy_until;
	/* firmware download session */
	bool dl_active;
	bool dl_complete;
	uint8_t dl_bank;
	uint32_t dl_size;
	uint32_t dl_written;
	uint8_t result;
	uint8_t rpl[120];
	uint8_t rpl_len;
};

struct omt_task {
	const char *name;
	uint16_t period_ms;
	uint32_t last;
	void (*fn)(struct omt_fw *fw);
	uint32_t runs;
	uint32_t max_us;
};

struct omt_fw {
	const struct omt_hal *hal;
	struct omt_cmis m;
	struct omt_calib cal;
	struct omt_tunables tun;
	struct omt_boot_rec boot;
	struct omt_cdb cdb;

	/* module state machine */
	uint8_t mstate;
	uint8_t pwrup_step;
	uint32_t mstate_t;
	uint8_t fault_cause;
	bool lowpwr_req;

	/* data paths */
	struct omt_dp dp[OMT_N_MEDIA_LANES];
	uint8_t dsp_mode;             /* 0 = 1x400G, 1 = 4x100G             */

	/* optics */
	struct omt_tx_lane tx[OMT_N_MEDIA_LANES];
	struct omt_rx_lane rx[OMT_N_MEDIA_LANES];

	/* environment monitors (engineering units) */
	float temp_c;
	float vcc_v;
	float laser_temp_c;
	float tec_drive;              /* -1..+1, + = cooling                */
	float tec_integ;
	uint32_t tec_sat_ms;
	uint16_t brownout_ms;
	bool tec_enabled;
	bool tec_alarm_logged;
	uint16_t raw_temp, raw_vcc;
	bool mon_primed;

	/* DSP supervision */
	bool dsp_ready;
	uint16_t dsp_heartbeat;
	uint8_t dsp_hb_stall;
	uint8_t dsp_recoveries_in_row;
	uint16_t dsp_recoveries;
	uint16_t mdio_errors;
	int16_t dsp_temp_dc;
	uint32_t ber_window_t;
	uint8_t dsp_lane_on;          /* lanes enabled in the DSP           */
	uint8_t dsp_rec_step;         /* hang recovery state, 0 = idle      */
	uint32_t dsp_rec_t;
	uint32_t dsp_last_hang_t;

	/* boot / image */
	uint8_t running_bank;
	uint8_t running_major, running_minor;
	uint16_t running_build;
	bool boot_confirmed;
	bool hung;                    /* image test hook: never kick WDT    */
	uint32_t pending_reset_t;     /* deferred software reset, 0 = none  */

	/* fault log */
	uint32_t flog_seq;
	uint32_t flog_addr;           /* next free slot                     */
	uint16_t flog_count;
	uint16_t last_fault;

	/* diagnostics */
	struct omt_log_entry log[OMT_LOG_LEN];
	uint8_t log_head;
	uint32_t boot_ms;

	/* scheduler */
	struct omt_task tasks[12];
	uint8_t n_tasks;

	/* console */
	char con_buf[96];
	uint8_t con_len;
};

/* ---- lifecycle ---- */
void omt_fw_init(struct omt_fw *fw, const struct omt_hal *hal);
void omt_fw_tick(struct omt_fw *fw);   /* call every 1 ms (or faster) */

/* ---- host management interface (I2C target, address 0x50) ---- */
/* Write transaction: buf[0] = byte address, buf[1..] data. */
int omt_i2c_write(struct omt_fw *fw, const uint8_t *buf, size_t len);
/* Read transaction from the current address pointer. */
int omt_i2c_read(struct omt_fw *fw, uint8_t *buf, size_t len);

/* ---- UART debug console ---- */
void omt_console_rx(struct omt_fw *fw, const char *s, size_t n);

/* ---- internal API shared between modules ---- */
void omt_log(struct omt_fw *fw, uint16_t code, uint16_t arg);
void omt_event(struct omt_fw *fw, uint16_t code, uint16_t arg); /* log + NVM fault log */
uint32_t omt_now(const struct omt_fw *fw);
void omt_printf(struct omt_fw *fw, const char *fmt, ...);

/* cmis_mem.c */
void cmis_mem_init(struct omt_fw *fw);
uint8_t *cmis_page(struct omt_fw *fw, uint8_t page);
void cmis_set_u16(uint8_t *p, uint16_t v);
uint16_t cmis_get_u16(const uint8_t *p);
void cmis_set_u32(uint8_t *p, uint32_t v);
void cmis_set_f32(uint8_t *p, float v);
float cmis_get_f32(const uint8_t *p);
void cmis_update_checksums(struct omt_fw *fw);
void cmis_set_module_state(struct omt_fw *fw, uint8_t st);
void cmis_set_dp_state(struct omt_fw *fw, uint8_t host_lane, uint8_t st);
void cmis_latch_module(struct omt_fw *fw, uint8_t byte_off, uint8_t cond);
void cmis_latch_lane(struct omt_fw *fw, uint8_t flag_off, uint8_t cond);
void cmis_update_intl(struct omt_fw *fw);
uint8_t cmis_encode_power_class(uint32_t max_mw);

/* monitor.c */
void mon_task(struct omt_fw *fw);
void mon_prime_temp(struct omt_fw *fw);
uint8_t alarm_eval(const struct omt_threshold *t, int32_t v, uint8_t prev);
void mon_load_thresholds(struct omt_fw *fw);
uint16_t mon_uw_to_cmis(float uw);
int16_t mon_c_to_cmis(float c);

/* msm.c / dpsm.c */
void msm_task(struct omt_fw *fw);
void msm_enter_fault(struct omt_fw *fw, uint8_t cause, uint16_t ev, uint16_t arg);
void dpsm_task(struct omt_fw *fw);
void dpsm_reset(struct omt_fw *fw);
void dpsm_apply_config(struct omt_fw *fw, uint8_t lanes);

/* tx.c */
int tx_init(struct omt_fw *fw);
void tx_shutdown(struct omt_fw *fw);
void tx_lane_enable(struct omt_fw *fw, uint8_t lane, bool on);
void tx_lane_squelch(struct omt_fw *fw, uint8_t lane, bool sq);
void apc_task(struct omt_fw *fw);
int ld_read(struct omt_fw *fw, uint8_t reg, uint16_t *val);
int ld_write(struct omt_fw *fw, uint8_t reg, uint16_t val);

/* tec.c */
void tec_enable(struct omt_fw *fw, bool on);
void tec_task(struct omt_fw *fw);
bool tec_locked(const struct omt_fw *fw);

/* dsp.c */
void dsp_power_up(struct omt_fw *fw);
void dsp_release_reset(struct omt_fw *fw);
int dsp_start(struct omt_fw *fw);
bool dsp_poll_ready(struct omt_fw *fw);
void dsp_power_down(struct omt_fw *fw);
int dsp_set_mode(struct omt_fw *fw, uint8_t mode);
int dsp_lane_enable(struct omt_fw *fw, uint8_t media_lane, bool on);
bool dsp_lane_locked(struct omt_fw *fw, uint8_t media_lane, bool *host_lock);
void dsp_task(struct omt_fw *fw);

/* nvm.c */
uint32_t omt_crc32(uint32_t crc, const void *data, size_t len);
void nvm_load_calibration(struct omt_fw *fw);
int nvm_save_calibration(struct omt_fw *fw);
void nvm_default_calibration(struct omt_calib *c);
void nvm_load_user_eeprom(struct omt_fw *fw);
void nvm_save_user_eeprom(struct omt_fw *fw);
void flog_init(struct omt_fw *fw);
void flog_append(struct omt_fw *fw, uint16_t code, uint16_t arg);
int flog_read(struct omt_fw *fw, uint16_t idx, uint8_t out[16]);
int boot_load(struct omt_fw *fw);
int boot_store(struct omt_fw *fw);

/* image.c */
int img_validate(struct omt_fw *fw, uint8_t bank, struct omt_img_hdr *hdr);
void img_boot(struct omt_fw *fw);
void img_confirm_task(struct omt_fw *fw);
void img_default_tunables(struct omt_tunables *t);
void img_publish_versions(struct omt_fw *fw);
size_t omt_img_build(uint8_t *buf, size_t cap, uint8_t major, uint8_t minor, uint16_t build,
		     uint32_t flags, const char *tag, const struct omt_tunables *tun);

/* cdb.c */
void cdb_task(struct omt_fw *fw);
uint8_t cdb_chkcode(const uint8_t *p9f);

/* console.c */
void console_task(struct omt_fw *fw);

/* fw.c */
void housekeeping_task(struct omt_fw *fw);

#ifdef __cplusplus
}
#endif

#endif /* OMT_FW_H */
