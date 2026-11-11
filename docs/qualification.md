# Qualification and product phases

How the verification in this repository maps onto a module's way from
first prototype to volume, and what the twin can and cannot qualify.

## Phases

| Phase | Question | What runs here | Exit criterion |
|---|---|---|---|
| **EVT** (engineering validation, first boards) | Does every block come up and talk? | `docs/hardware.md` first power-on checklist; `omt console ver/faults/ld r/mdio r`; `omt bench`; *Power-up sequencing on the oscilloscope*; *Fault isolation with PRBS and loopbacks*; `omt serdes`, `omt prbs` | all buses answer, sequence within spec, host lanes lock, loopback BER 0 |
| **DVT** (design verification) | Does the design meet its specification over corners? | `omt dvt`: temperature sweep, Vcc margining, RX sensitivity, host channel sweep, power-up timing, 12 fault scenarios, CDB update suite, breakout, host fuzz | all DVT checks pass; findings closed or accepted |
| **Qualification** (DVT units, reliability) | Will it keep meeting it for its service life? | `omt qual`: HTOL readpoints with life extrapolation, temperature cycling, power-cycle endurance and NVM wear | criteria in `QLIM` (host/omt/qual.py) |
| **PVT** (production validation, pilot line) | Can the factory build it repeatably? | `omt mfg`: calibration station, Cpk per DDM parameter, yield, CSV records; `omt lint` on every unit | Cpk ≥ 1.33 on all parameters, yield target |
| **Ramp / volume** | Does it stay in control? | Cpk and yield per lot, fault-log and B0h counters from returns (`omt info dump.json`, `omt lint`) | lot statistics inside control limits |

## Qualification matrix

Typical optical-transceiver reliability tests (Telcordia GR-468 style; the
exact list, durations and sample sizes come from the customer's
qualification plan) and how much of each the twin covers.

| Test | Twin coverage | Procedure | Needs hardware for |
|---|---|---|---|
| High-temperature operating life (laser aging) | **Firmware response**: APC compensation, bias warning before EOL, EOL shutdown, life extrapolation from readpoints | `qual.htol` | the actual degradation law and activation energy of the laser |
| Temperature cycling (operating) | **Functional**: link, flags, TEC, APC over cycles | `qual.temp_cycling` | solder, fiber attach and lens alignment fatigue |
| Power cycling | **Full** for firmware and NVM: boot, reset count, identity, wear | `qual.power_cycling` | inrush, hot-plug and connector wear |
| NVM endurance / data retention | **Endurance projection** per usage profile (finding F-010) | `qual.power_cycling` | retention at temperature (flash datasheet) |
| Damp heat, mechanical shock, vibration | none | – | all of it |
| ESD (HBM/CDM), EMC | none | – | all of it |
| Low-temperature / high-temperature storage | none (non-operating) | – | all of it |
| Firmware update robustness | **Full**: bad image, crash on boot, power loss before commit | `dvt.fw_update_suite` | flash timing on the real part |

## Assumptions in the aging model

`qual.AgingModel`: degradation `d = k·(t/40 y)^0.5`, activation energy
0.7 eV, laser held at 50 °C by the TEC in use and 85 °C in the stress test
(acceleration factor 11.7), unit-to-unit spread of `k` ±15 % (1σ). These
constants are placeholders for vendor laser data; with them the worst laser
of the seed-71 sample reaches the 90 mA bias warning after about 8 years.
The life numbers are therefore model outputs, reported but not used as pass
criteria. The pass criteria cover what the firmware is responsible for:
launch power held (APC), at least one year between bias warning and EOL
shutdown, and readpoint extrapolation within 20 % of the twin's actual
crossing.

## Criteria (`QLIM`)

| Criterion | Limit |
|---|---|
| Operating-current increase after the HTOL equivalent | ≤ 50 % |
| Launch power change through aging | ≤ 0.5 dB |
| Bias warning before EOL shutdown | ≥ 1 year |
| Life extrapolation error | ≤ 20 % |
| Boot-record wear | ≥ 20 years at 100 power cycles per day |
| User EEPROM wear | ≥ 20 years at one host write per hour |
