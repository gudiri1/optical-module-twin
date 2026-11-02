/*
 * Optical Module Twin - simulation API (C ABI, used by Python via ctypes).
 *
 * One handle = one complete module: controller firmware + hardware twin
 * (lasers, photodiodes, TEC, DSP, power, flash, watchdog) in virtual time.
 * Everything is deterministic for a given seed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OMT_SIM_H
#define OMT_SIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct omt_sim omt_sim;

#define OMT_SIM_GOLDEN_CAL  0x1u  /* write ideal factory calibration    */
#define OMT_SIM_BLANK_FLASH 0x2u  /* no firmware image programmed       */

omt_sim *omt_sim_create(uint32_t seed, uint32_t flags);
void omt_sim_destroy(omt_sim *s);

/* advance virtual time */
void omt_sim_run(omt_sim *s, uint32_t ms);
uint64_t omt_sim_time_ms(const omt_sim *s);

/* host management interface (TWI address 0x50). 0 = ACK, -1 = NACK */
int omt_sim_i2c_write(omt_sim *s, const uint8_t *data, size_t n); /* data[0] = offset */
int omt_sim_i2c_read(omt_sim *s, uint8_t offset, uint8_t *out, size_t n);

/* named stimuli / fault injection, returns -1 for an unknown name */
int omt_sim_set(omt_sim *s, const char *name, double value);
/* named ground truth from the twin (what a lab instrument would measure) */
double omt_sim_get(omt_sim *s, const char *name);

/* UART console: send one line, run 20 ms, return output length */
size_t omt_sim_console(omt_sim *s, const char *line, char *out, size_t cap);

/* I2C transaction trace: records of [u32 t_us][u8 kind][u8 len][len bytes]
 * kind: 'W' write (offset + data), 'R' read (offset + data), 'N' NACK.
 * Returns bytes copied and clears the trace. */
size_t omt_sim_trace_take(omt_sim *s, uint8_t *buf, size_t cap);

/* raw NVM access for white-box tests and failure analysis */
size_t omt_sim_flash_read(omt_sim *s, uint32_t addr, uint8_t *buf, size_t n);

/* build a firmware image with the firmware's own builder */
size_t omt_sim_build_image(uint8_t *buf, size_t cap, uint8_t major, uint8_t minor,
			   uint16_t build, uint32_t flags, const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* OMT_SIM_H */
