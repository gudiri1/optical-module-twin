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
| 30.0100+h | HCTRL | host lane h (0..7): b0 adaptive CTLE, b7..4 fixed peaking dB |
| 30.0110+h | HSTAT | b0 signal detect, b1 host CDR lock, b2 adaptation done |
| 30.0120+h / 0130+h | HSNR / HEQ | host input SNR 0.01 dB / CTLE in use dB |
| 30.0200..0206 | PRBS / loopback | generator and checker enables (host 8 bits, media 4 bits), loopback masks |
| 30.0208+h / 0210+m | PATTERN | CMIS pattern code per lane |
| 30.0218 | CHK_LOCK | b7..0 host checkers, b11..8 media checkers |
| 30.021A | CHK_SNAP | b0 latch checker counters, b1 discard |
| 30.0220+4h / 0240+4m | CHK counters | errors and bits (2^16 units) like 30.0040 |

Host lane h feeds media lane h/2 in both applications.

## Power-up on the oscilloscope

`dvt.power_sequencing_scope()` (part of `omt dvt`) arms the scope on the
laser-driver rail (CH1, rising, 1.65 V, 100 ms/div), releases LowPwr over TWI
and measures:

| Probe | Net | Expect (twin, 1 ms resolution) |
|---|---|---|
| CH1 | laser driver 3.3 V rail | trigger, t = 0 |
| CH2 | DSP core 0.75 V rail | in regulation at +2 ms, monotonic |
| CH3 | DSP_RSTN | one rising edge, ≥ 20 ms after CH2 starts (19 ms measured = 20 ms minus one sample) |
| CH4 | IntL | falls at ModuleReady, about +770 ms |

The capture exports to VCD (`vcd.write_waveforms`) for GTKWave/PulseView. The
first version of this check measured 9 ms of reset hold: finding F-009.

## Host lanes and PRBS

```
python -m omt serdes --loss 14                     # CTLE, SNR, Tx flags per host lane
python -m omt serdes --fixed 4                     # fixed Tx input Eq via ApplyImmediate
python -m omt prbs --side host --host-prbs PRBS31Q # module checker on the host lanes
python -m omt prbs --side media --loopback media-output     # DSP only, no optics
python -m omt prbs --side media --fiber-loopback 6          # optics in the loop
```

Isolation rule used by `dvt.loopback_isolation()`: errors with fiber
loopback but none with media-output loopback point at the optical path
(connector, laser, PD); errors from the host but none with host-output
loopback point at the host channel, not the module SerDes.

## Console (UART, `python -m omt console`)

```
help ver state mon lanes dsp serdes faults [n] log tasks tec cal
mdio r <dev> <reg> | mdio w <dev> <reg> <val> | ld r <reg> | reset
```

## Source-level debugging

**On the twin (any PC).** The firmware runs inside `libomt_sim`, built with
`-g`, so gdb debugs it like target code. A session from development (MinGW
gdb, Windows), stopping in the DSP hang recovery:

```
$ gdb -q -batch -x dbg.cmd --args python repro.py     # repro.py: bring up, set dsp_hang=1, run 2 s
(gdb) set breakpoint pending on
(gdb) break dsp_recover if fw->dsp_rec_step == 4
(gdb) run
Thread 1 hit Breakpoint 1, dsp_recover (fw=0xec4d70) at firmware/src/dsp.c:359
#1  dsp_task (fw=0xec4d70) at firmware/src/dsp.c:410
#2  omt_fw_tick (fw=0xec4d70) at firmware/src/fw.c:243
#3  omt_sim::run (this=0xec4370, ms=2000) at sim/src/twin.cpp:675
(gdb) print fw->dsp_recoveries_in_row
$2 = 1
(gdb) print fw->host[0]
$5 = {snr_db = 25, eq_db = 8, sig = true, lock = true, adapt_done = true, adapt_fault = false}
```

Virtual time stops while the debugger holds the process, so breakpoints do
not trip the watchdog, unlike on hardware.

**On a board (SWD/JTAG).** `west build -b <board> ports/zephyr`, then
`west debug` (OpenOCD or J-Link runner from the board definition) or
`west debugserver` plus `arm-none-eabi-gdb build/zephyr/zephyr.elf` and
`target remote :3333`. Useful starting points:

| Symptom | Break / watch |
|---|---|
| module goes to Fault | `break msm_enter_fault`, then `bt` and `print cause` |
| host sees a wrong byte | `watch fw.m.lower[<offset>]` (hardware watchpoint) |
| watchdog resets | the watchdog keeps running while halted: disable it in the debug build (`CONFIG_WDT_DISABLE_AT_BOOT`) or let OpenOCD freeze it in debug mode (`DBGMCU`) |
| timing (sequence, ISR load) | toggle a spare GPIO in the code path and put it on the scope next to the rails |

## Debug flow that worked during development

1. `omt inject <fault> --ms N` reproduces a field symptom on the twin.
2. Host view first: latched flags (read them before lower memory, they clear on read), module state, fault cause (byte 41), vendor page B0h (reset reason, DSP recoveries, I2C error counters).
3. Module view: `faults` (non-volatile, survives resets), `log` (RAM, last 64 events), `dsp`, `tec`, `mdio r`, `ld r`.
4. Bus view: `omt trace --vcd bus.vcd` and open in PulseView with the I2C decoder; NACKs mark resets and boot windows.
5. Turn the reproduction into a test (see `tests/integration/test_faults.py`) before fixing.

Example: F-002 (bias alarm never fired) was found by step 1 with
`aging.1=3`: flags showed TxFault without any bias alarm; `ld r 0x11` showed
the bias pinned at 105 mA, below the 110 mA alarm threshold.
