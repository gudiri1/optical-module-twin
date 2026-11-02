# Board bring-up

A hardware board provides, through devicetree:

| Firmware need | Devicetree | Zephyr API used in `src/hal_zephyr.c` |
|---|---|---|
| CMIS host interface (TWI 0x50) | `omt,host-i2c` chosen, I2C controller with target support | `i2c_target_register()` |
| Laser driver | node `omt_laser` on SPI | `spi_transceive_dt()` |
| DSP management | `omt_mdio` MDIO controller | `mdio_read_c45()` / `mdio_write_c45()` |
| Monitors | `zephyr,user` `io-channels` (8 channels in `enum omt_adc_ch` order) | `adc_read_dt()` |
| TEC drive | DAC `omt_dac` | `dac_write_value()` |
| LPMode, ResetL, IntL, rails | `zephyr,user` `*-gpios` | `gpio_pin_get_dt()` / `gpio_pin_set_dt()` |
| NVM (64 KiB) | `omt,nvm` chosen fixed partition | `flash_area_*()` |
| Watchdog | `watchdog0` alias | `wdt_feed()` |

On native_sim only the flash partition exists; the other services return
`-ENODEV`, so the firmware boots, initializes the CMIS memory map, writes its
fault log to the simulated flash and stays in ModuleLowPwr / ModuleFault
exactly as it would on a board whose laser driver does not answer.
