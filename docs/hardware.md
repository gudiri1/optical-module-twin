# Hardware reference for bring-up

What the firmware assumes about the board, written the way a bring-up
engineer reads a schematic: block by block, net by net, with the value to
expect on the bench. The twin implements exactly this board
(`sim/src/twin.cpp`); a real board port must match it or change `hal.h`.

## Block diagram

```
          QSFP-DD host connector                    module PCB
 ┌───────────────────────────────┐   ┌─────────────────────────────────────────────────────────┐
 │ SCL/SDA (TWI 0x50, 400 kHz) ──┼──►│ MCU I2C target ──► omt_i2c_write/read (ISR)              │
 │ LPMode/InitMode ──────────────┼──►│ MCU GPIO in   OMT_PIN_LPMODE                             │
 │ ResetL ───────────────────────┼──►│ MCU GPIO in   OMT_PIN_RESETL                             │
 │ IntL ◄────────────────────────┼───│ MCU GPIO out  OMT_PIN_INTL (open drain, active low)      │
 │ Vcc 3.3 V ────────────────────┼──►│ ─┬─► load switch (TX_PWR)  ─► laser driver 3.3 V         │
 │                               │   │  └─► DSP core regulator (DSP_PWR) ─► 0.75 V              │
 │ 8 × 53.125 Gb/s PAM4 (Tx in) ─┼──►│ DSP host SerDes (CTLE, CDR) ── PAM4 DSP ── 4 × 106 Gb/s  │
 │ 8 × 53.125 Gb/s PAM4 (Rx out)◄┼───│                                          │      ▲       │
 └───────────────────────────────┘   │ MCU SPI ──► quad laser driver ──► 4 × DML/EML + MPD      │
                                     │ MCU MDIO ─► DSP (PRTAD 0, Clause 45)        │ MPO-12   │
                                     │ MCU ADC ◄── temp sensor, Vcc/2, 4 × RSSI,   ▼          │
                                     │             laser thermistor, TEC current  fiber       │
                                     │ MCU DAC ──► TEC driver (TEC_EN) ─► Peltier under lasers  │
                                     │ MCU QSPI/internal ─► 64 KiB NOR (cal, log, A/B images)   │
                                     └─────────────────────────────────────────────────────────┘
```

## Controller nets (`firmware/include/omt/hal.h`)

| Net | Dir | Active | Expected on the bench |
|---|---|---|---|
| `LPMODE` | in | high = low power requested | follows the host; ignored unless byte 26 bit 6 (LowPwrAllowRequestHW) is set |
| `RESETL` | in | low = reset | high in operation; low holds the MCU (twin: firmware stopped, rails off) |
| `INTL` | out | low = interrupt | high after the host has read all latched flags; low on any unmasked flag |
| `DSP_PWR` | out | high = rail on | high in PwrUp/Ready; DSP core 0.75 V within 2 ms |
| `DSP_RSTN` | out | low = DSP in reset | rises ≥ 20 ms after `DSP_PWR` (finding F-009) |
| `TX_PWR` | out | high = laser driver rail | rises together with `DSP_PWR` at the start of ModulePwrUp |
| `TEC_EN` | out | high = TEC driver on | high in PwrUp/Ready; low in LowPwr and Fault |

Interfaces: laser driver on SPI (3-byte frames, ID register 0x00 = 0x4C44),
DSP on MDIO Clause 45 (1.0002/1.0003 = 0x7F44/0x5350), NOR flash 64 KiB with
4 KiB sectors, watchdog 300 ms, UART console.

## ADC scaling (16 bit, 2.5 V reference)

| Channel | Signal | Transfer | At 25 °C / nominal |
|---|---|---|---|
| 0 `TEMP` | module temperature sensor | 0.5 V + 10 mV/°C | 0.75 V → 19661 counts |
| 1 `VCC` | Vcc through 1:2 divider | Vcc / 2 | 1.65 V → 43253 counts |
| 2–5 `RSSI0..3` | RX photodiode via TIA | dark ≈ 200 counts + ≈ 19 counts/µW (unit spread ±8 %, calibrated) | −3 dBm ≈ 9.7k counts |
| 6 `LASER_TEMP` | thermistor on the TEC plate | 0.5 V + 10 mV/°C | 50 °C → 1.0 V |
| 7 `TEC_I` | TEC current sense | 1.25 V + 0.5 V/A | 0 A → 1.25 V |

DAC 0 drives the TEC: 0x8000 = no current, 0xFFFF = full cooling.

## First power-on checklist

| Step | Measure | Pass |
|---|---|---|
| 1 | Vcc at the connector, module in LowPwr | 3.135–3.465 V, < 1.5 W |
| 2 | TWI: read byte 0 | 0x18 (QSFP-DD); NACK for ~20 ms after reset is normal (boot) |
| 3 | IntL after reset | low (ModuleStateChanged latched), high after reading byte 8 |
| 4 | `python -m omt console ver` / `faults` | firmware version, fault log readable |
| 5 | Release LowPwr (byte 26 = 0x00), scope on TX rail, DSP rail, DSP_RSTN, IntL | sequence as in `docs/bringup.md`; `omt dvt` runs this as *Power-up sequencing on the oscilloscope* |
| 6 | `omt console ld r 0` / `mdio r 1 2` | 0x4C44 / 0x7F44 |
| 7 | `omt serdes`, `omt prbs --side host --loopback host-output` | all host lanes locked; loopback BER 0 |
| 8 | Fiber loopback, `omt prbs --side media` | BER below 2.4e-4 at the attenuation of the link budget |

## Firmware footprint (Cortex-M4F)

`python tools/footprint.py` cross-compiles the unchanged firmware with
arm-none-eabi-gcc 10.3 at `-Os` for a Cortex-M4F, links it with newlib-nano
(float printf for the console) and a stub board, and reads the linker map:

| | Flash | RAM |
|---|---|---|
| Whole image | 40.6 KiB | 4.0 KiB |
| Firmware modules (14 files) | ≈ 20 KiB | 3.5 KiB (`struct omt_fw`, one instance) |
| C library (newlib-nano, libm, libgcc) | ≈ 21 KiB, mostly float `printf` | 0.5 KiB |

Largest single stack frame: 288 B (`console_task`); the tool reports frames,
not a call-graph worst case. Removing float formatting from the console would
roughly halve the library share. A 128 KiB flash / 16 KiB RAM part leaves room
for the two 16 KiB image banks only if they live in external NOR, as in this
design.
