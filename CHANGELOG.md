# Changelog

## Unreleased
* Host-side SerDes: 8 host lanes with adaptive or fixed Tx input CTLE through the CMIS staged/active
  control sets (Page 10h/11h SI bytes, ApplyImmediate); Tx LOS, Tx CDR LOL and Tx adaptive Eq fault
  per host lane (F-008)
* CMIS diagnostics Pages 13h/14h: PRBS generators and checkers on host and media side, four loopbacks,
  BER as F16 or SNR, latched checker LOL; console `serdes`; CLI `serdes`, `prbs`
* Power-up sequence times the DSP reset hold from the rails, not from the state change (F-009)
* User EEPROM as an append-only log over two sectors: wear down by ~60x, power-fail safe, old format
  still read (F-010)
* SCPI instrument layer (supply, thermal forcer, source + VOA, power meter, DMM, BERT, oscilloscope)
  over VISA, TCP or simulation; environmental DVT procedures run through it; CLI `bench`
* New DVT procedures: host channel sweep, loopback fault isolation, power sequencing on the oscilloscope
* Qualification: HTOL readpoints with life extrapolation, temperature cycling, power cycling with NVM
  wear projection; CLI `qual`; docs/qualification.md (EVT/DVT/PVT)
* Report sections for host interface, bench bring-up and qualification; one-page status page
* docs/hardware.md (nets, ADC scaling, first power-on checklist, footprint); `tools/footprint.py`
* Build and run natively on Windows (MinGW): the host `ctypes` binding
  discovers the platform-correct `libomt_sim` name, and the Windows DLL is
  statically linked (libgcc/libstdc++/winpthread) so it loads without the
  toolchain runtime on `PATH`. Verified with `pytest` and `ctest`.

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
