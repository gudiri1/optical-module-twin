# Optical Module Twin

Module-side controller firmware for a 400G QSFP-DD (400GBASE-DR4) optical
transceiver, running against a hardware digital twin, with the host tooling,
verification, manufacturing and analysis around it.

```
 host (switch/router)                 the module
┌────────────────────┐  TWI 0x50   ┌──────────────────────────────────────────────────────┐
│ omt.host.CmisHost  │◄──CMIS 5.x─►│ controller firmware (C11, firmware/)                 │
│  bring-up, flags,  │  IntL,LPMode│  CMIS memory map · MSM/DPSM · DDM + alarms · CDB     │
│  CDB fw update     │  ResetL     │  APC · TEC · DSP supervision · fault log · watchdog  │
│ omt lint / dvt /   │             │──────────── HAL (struct omt_hal) ────────────────────│
│ mfg / report       │             │ digital twin (C++17, sim/) or Zephyr board port      │
└────────────────────┘             │  4 lasers+MPD (SPI) · RX PD/TIA (ADC) · TEC (DAC)    │
                                   │  PAM4 DSP (MDIO Cl.45) · NOR flash · WDT · pins      │
                                   └──────────────────────────────────────────────────────┘
```

The host-side counterpart in Zephyr is
[zephyrproject-rtos/zephyr#123842](https://github.com/zephyrproject-rtos/zephyr/pull/123842)
(SFF-8472/CMIS decoding and drivers). This repository is independent of it and
does not modify it; `tests/interop` replays its access pattern against the
module and (opt-in) cross-checks its decoder field by field.

## Quick start

```sh
cmake -S . -B build -G Ninja && cmake --build build   # firmware lib, twin, C unit tests
ctest --test-dir build                                  # C unit tests
pip install -r requirements.txt                         # pytest, pyyaml, numpy
pytest                                                  # ~60 tests, a few seconds
cd host && python -m omt demo                           # 6-step walkthrough
python -m omt report --out ../build/report.html         # full evidence page
```

`make all` does build + unit tests + pytest + report. `make asan` repeats the C
tests under AddressSanitizer/UBSan.

## What is in here

| Path | Content |
|---|---|
| `firmware/` | Controller firmware, target independent C11, every peripheral behind `include/omt/hal.h` |
| `sim/` | Hardware twin in C++17 with a C ABI (`omt_sim.h`), 1 ms virtual time, deterministic per seed |
| `host/omt/` | Python: ctypes binding, Linux i2c-dev bus, CMIS host driver, decoder, lint, VCD trace, DVT, manufacturing, DSP lab, report, CLI |
| `ports/zephyr/` | Zephyr application: scheduler thread, I2C target callbacks, flash map, shell bridge; builds and runs on `native_sim` |
| `tests/` | C unit tests (`unit/`), pytest integration, fault matrix, CDB, DVT, manufacturing, interop |
| `requirements/` | Requirements with IDs (traced to tests) and the findings log |
| `docs/` | Architecture, ADRs, bring-up guide, memory map, findings, verification report snapshot |
| `tools/` | Release script, conformance vector generator, Zephyr PR interop harness |

## Conformance vectors for host stacks

```sh
python3 tools/gen_vectors.py build/vectors          # 7 module states -> cmis_vectors.h / .json
west build -b native_sim/native/64 tests/zephyr/cmis_xcheck -t run   # Zephyr lib/cmis vs. twin
```
Each vector is a byte image of Lower Memory and Pages 00h/01h/02h/11h captured
from the running firmware (nominal, fiber cut, laser end of life, hot/low Vcc,
ModuleFault, LowPwr, 4x100G breakout) plus the values an independent decoder
reads from it. The Zephyr CMIS library with the follow-up PR decodes all seven identically.

## Command line

```
python -m omt info [dump.json|ethtool.txt]   decode a module or a dump
python -m omt lint [file]                    CMIS consistency rules L001..L020
python -m omt mon --seconds 10               live DDM
python -m omt console [cmd]                  firmware UART console (interactive without cmd)
python -m omt inject aging.1=3 --ms 3000     fault injection, shows flags + fault log
python -m omt trace --vcd bus.vcd            bring-up on the host bus, open in PulseView
python -m omt mkimage --version 1.1 -o fw.bin && python -m omt fwupdate fw.bin
python -m omt dvt [--quick]                  design verification procedures
python -m omt mfg --units 40 --csv lot.csv   calibration station, Cpk
python -m omt report                         HTML report with charts and traceability
```
`--i2c-bus N` runs `info`, `dump`, `lint` and `mon` against a real module via
`/dev/i2c-N` (not exercised in CI).

## Results of the current build

* 155/155 DVT checks in 22 procedures; 12 injected fault scenarios handled
* RX sensitivity −7.8 dBm average power at pre-FEC BER 2.4e-4 (limit −6 dBm)
* ModulePwrUp 780 ms (advertised ≤ 5 s), DPInit 140 ms (≤ 1 s), ≤ 7.9 W at 75 °C case (class 8, 10 W)
* Calibration: Cpk ≥ 1.5 on all DDM parameters after calibration (0.4–1.7 before), 100 % yield on 40 units
* 7 firmware findings (F-001…F-007) and 4 host-side notes (P-1…P-4), see `docs/findings.md`
* Zephyr: fixes for P-1/P-3 and a follow-up for P-2 tested with twister (33/33 on native_sim), to be posted on #123842

## Status

| Part | State |
|---|---|
| Module firmware, twin, host tools, tests | v1.0.0, all suites green (`make test`) |
| Zephyr host side: [zephyr#123842](https://github.com/zephyrproject-rtos/zephyr/pull/123842) | in review (Architecture WG); review fixes and a Page 02h/11h follow-up prepared |
| Conformance vectors | 7 module states; Zephyr `lib/cmis` (with the follow-up) decodes all identically |

## Reproducibility

Every number in `docs/verification-report.html` is produced by the code in
this repository: `cd host && python -m omt report`. Runs are deterministic
per seed.

## Honest limits

This is a simulation. CMIS byte offsets follow CMIS 5.x as modeled in
`firmware/include/omt/cmis_map.h` and must be checked against the OIF text
before product use; vendor pages B0h/B1h are this design's own. The twin's
physics is first-order (documented constants in `sim/src/twin.cpp`), and the
DSP model is about 0.2 dB optimistic against the equalized link (F-007). The
Zephyr port is build- and run-tested on `native_sim`, where only flash exists.

## License

Apache-2.0, see `LICENSE` and `NOTICE`. Contributions: `CONTRIBUTING.md`.
