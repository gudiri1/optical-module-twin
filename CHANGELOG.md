# Changelog

## 1.0.0 (project release, 2026-11)
* First public release under Apache-2.0: module firmware 1.0, hardware twin, host tools, DVT/manufacturing suites
* Conformance vectors (`tools/gen_vectors.py`) and Zephyr cross-check (`tests/zephyr/cmis_xcheck`)
* Zephyr upstream patch series in `upstream/`

Firmware image versions below.

## 1.0
* CMIS 5.x memory map with paging, clear-on-read flags, masks and IntL
* Module and data path state machines; AppSel 400GBASE-DR4 and 4x100GBASE-DR breakout
* DDM with factory calibration, hysteresis alarms, protection (over-temperature, brown-out, laser over-temperature)
* APC with end-of-life and eye-safety shutdown; TEC PI loop; DSP supervision with hang recovery
* CDB firmware management with A/B images, trial boot, rollback, commit
* Non-volatile fault log, boot record, user EEPROM with write coalescing
* Fixes from verification: F-001, F-002, F-003, F-006
