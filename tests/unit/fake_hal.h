/* SPDX-License-Identifier: Apache-2.0 */
#ifndef FAKE_HAL_H
#define FAKE_HAL_H

#include "omt/fw.h"

struct fake_hw {
	struct omt_hal hal;
	uint32_t now;
	uint8_t flash[OMT_NVM_SIZE];
	uint16_t adc[OMT_ADC_COUNT];
	int pin[OMT_PIN_COUNT];
	uint32_t erases, kicks;
	int irq_depth;
	int reset_req;
	char uart[4096];
	size_t uart_len;
};

void fake_hal_init(struct fake_hw *f);
void fake_run(struct fake_hw *f, struct omt_fw *fw, uint32_t ms);
void host_write(struct omt_fw *fw, uint8_t off, const uint8_t *d, size_t n);
void host_write1(struct omt_fw *fw, uint8_t off, uint8_t v);
void host_read(struct omt_fw *fw, uint8_t off, uint8_t *d, size_t n);
uint8_t host_read1(struct omt_fw *fw, uint8_t off);

#endif
