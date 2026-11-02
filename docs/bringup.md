# Bring-up and debug guide

## Power-up sequence (ModulePwrUp)

| Step | Time | Action | Failure → |
|---|---|---|---|
| 0 | t0 | Laser driver rail on, SPI probe (ID 0x4C44); DSP rail on with reset held; TEC enabled | ModuleFault cause 5 (laser driver) |
| 1 | +20 ms | Release DSP reset | |
| 2 | +50 ms | MDIO probe 1.0002/1.0003 = 0x7F44/0x5350, start DSP init (mode 1x400G or 4x100G) | ModuleFault cause 1 (DSP) |
| 3 | ≤ +550 ms | DSP ready (30.0001 bit 0) | ModuleFault cause 1 |
| 3 | ≤ 4 s | TEC within ±1 °C of 50 °C, then ModuleReady (continues with a warning after 4 s) | Aux1 alarm later |

## Laser driver (SPI, 3-byte frames `[R/W|addr][hi][lo]`)

| Reg | Name | Notes |
|---|---|---|
| 0x00 | ID | 0x4C44 |
| 0x01 | CTRL | b0-3 laser enable, b4-7 output enable, b15 soft reset |
| 0x02 | STATUS | b0-3 channel fault, clear on read |
| 0x10+n | BIAS_SET | 12 bit, 120 mA full scale |
| 0x14+n | BIAS_MON | 12 bit |
| 0x18+n | MPD_MON | 12 bit, back-facet monitor photodiode |

## DSP (MDIO Clause 45, device 30)

| Reg | Name | Notes |
|---|---|---|
| 30.0000 | GCTRL | b15 reset, b0 start, b1 mode |
| 30.0001 | GSTAT | b0 ready, b15 fault |
| 30.0002 | SNAPSHOT | write 1 to latch BER/FEC counters |
| 30.0003 | HEARTBEAT | must change between 100 ms polls; 3 stalls = hang |
| 30.0010+n | LCTRL | b0 TX, b1 RX |
| 30.0020+n | LSTAT | b0 host CDR lock, b1 media CDR lock, b2 FFE converged |
| 30.0030+n | SNR | 0.01 dB |
| 30.0040+4n | ERR/BITS | 32-bit error count, 32-bit bit count in 2^16 units |
| 30.0060+n / 0068+n | FEC | corrected / uncorrectable RS(544,514) codewords |
| 30.0070 | DIE_TEMP | 0.1 °C |

## Console (UART, `python -m omt console`)

```
help ver state mon lanes dsp faults [n] log tasks tec cal
mdio r <dev> <reg> | mdio w <dev> <reg> <val> | ld r <reg> | reset
```

## Debug flow that worked during development

1. `omt inject <fault> --ms N` reproduces a field symptom on the twin.
2. Host view first: latched flags (read them before lower memory, they clear on read), module state, fault cause (byte 41), vendor page B0h (reset reason, DSP recoveries, I2C error counters).
3. Module view: `faults` (non-volatile, survives resets), `log` (RAM, last 64 events), `dsp`, `tec`, `mdio r`, `ld r`.
4. Bus view: `omt trace --vcd bus.vcd` and open in PulseView with the I2C decoder; NACKs mark resets and boot windows.
5. Turn the reproduction into a test (see `tests/integration/test_faults.py`) before fixing.

Example: F-002 (bias alarm never fired) was found by step 1 with
`aging.1=3`: flags showed TxFault without any bias alarm; `ld r 0x11` showed
the bias pinned at 105 mA, below the 110 mA alarm threshold.
