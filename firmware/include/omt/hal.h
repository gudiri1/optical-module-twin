/*
 * Hardware abstraction layer of the module controller.
 *
 * The firmware core never touches registers directly. A board port
 * (the C++ digital twin, a Zephyr port, or a real MCU BSP) fills this
 * table. Every call is synchronous; return values < 0 are errors.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OMT_HAL_H
#define OMT_HAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPIOs of the module controller */
enum omt_pin {
	OMT_PIN_LPMODE = 0,   /* in:  host LPMode (1 = low power requested)   */
	OMT_PIN_RESETL,       /* in:  host ResetL (0 = held in reset)          */
	OMT_PIN_INTL,         /* out: IntL to host, active low                 */
	OMT_PIN_DSP_PWR,      /* out: DSP core rail enable                     */
	OMT_PIN_DSP_RSTN,     /* out: DSP reset, active low                    */
	OMT_PIN_TX_PWR,       /* out: laser driver rail enable                 */
	OMT_PIN_TEC_EN,       /* out: TEC driver enable                        */
	OMT_PIN_COUNT
};

/* 16-bit ADC channels (2.5 V reference) */
enum omt_adc_ch {
	OMT_ADC_TEMP = 0,     /* module temperature sensor, 10 mV/degC + 0.5 V */
	OMT_ADC_VCC,          /* Vcc through 1/2 divider                       */
	OMT_ADC_RSSI0,        /* RX photodiode RSSI, lanes 0..3                */
	OMT_ADC_RSSI1,
	OMT_ADC_RSSI2,
	OMT_ADC_RSSI3,
	OMT_ADC_LASER_TEMP,   /* thermistor on TEC plate, 10 mV/degC + 0.5 V   */
	OMT_ADC_TEC_I,        /* TEC current sense, 1.25 V + 0.5 V/A           */
	OMT_ADC_COUNT
};

enum omt_dac_ch {
	OMT_DAC_TEC = 0,      /* TEC drive, 0x8000 = 0, 0xFFFF = full cool     */
};

enum omt_reset_cause {
	OMT_RST_POWER_ON = 0,
	OMT_RST_WATCHDOG = 1,
	OMT_RST_SOFTWARE = 2,
	OMT_RST_HOST_PIN = 3,
};

struct omt_hal {
	void *ctx;

	uint32_t (*millis)(void *ctx);
	uint8_t (*reset_cause)(void *ctx);

	/* Quad laser driver on SPI: 3-byte frames, see tx.c */
	int (*spi_xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);

	/* DSP on MDIO, IEEE 802.3 Clause 45 */
	int (*mdio_read)(void *ctx, uint8_t prtad, uint8_t devad, uint16_t reg, uint16_t *val);
	int (*mdio_write)(void *ctx, uint8_t prtad, uint8_t devad, uint16_t reg, uint16_t val);

	int (*adc_read)(void *ctx, uint8_t ch, uint16_t *raw);
	int (*dac_write)(void *ctx, uint8_t ch, uint16_t val);

	int (*gpio_get)(void *ctx, uint8_t pin);
	void (*gpio_set)(void *ctx, uint8_t pin, int level);

	/* NOR-flash semantics: erase -> 0xFF, program can only clear bits */
	int (*nvm_read)(void *ctx, uint32_t addr, void *buf, size_t len);
	int (*nvm_write)(void *ctx, uint32_t addr, const void *buf, size_t len);
	int (*nvm_erase)(void *ctx, uint32_t addr, size_t len);

	void (*wdt_kick)(void *ctx);
	void (*uart_tx)(void *ctx, const char *s, size_t n);
	void (*system_reset)(void *ctx, uint8_t cause);

	/* Interrupt lock for data shared with the I2C target ISR */
	void (*irq_lock)(void *ctx);
	void (*irq_unlock)(void *ctx);
};

/* NVM layout (64 KiB, 4 KiB sectors) */
#define OMT_NVM_SECTOR        0x1000u
#define OMT_NVM_CAL           0x0000u
#define OMT_NVM_USER_EEPROM   0x1000u
#define OMT_NVM_FAULTLOG_A    0x2000u
#define OMT_NVM_FAULTLOG_B    0x3000u
#define OMT_NVM_BOOT          0x4000u
#define OMT_NVM_IMAGE_A       0x8000u
#define OMT_NVM_IMAGE_B       0xC000u
#define OMT_NVM_IMAGE_SIZE    0x4000u
#define OMT_NVM_SIZE          0x10000u

#ifdef __cplusplus
}
#endif

#endif /* OMT_HAL_H */
