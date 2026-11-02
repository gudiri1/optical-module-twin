# CMIS memory map as implemented

Modeled on CMIS 5.x; verify offsets against the OIF-CMIS text before product use.

## Lower memory

| Byte | Field | Access |
|---|---|---|
| 0 | Identifier 0x18 (QSFP-DD) | RO |
| 1 | CMIS revision 0x52 | RO |
| 2 | bit 7 flat memory = 0 | RO |
| 3 | bits 3-1 module state, bit 0 interrupt deasserted | RO |
| 8 | latched: b0 ModuleStateChanged, b6 CdbCmdComplete | COR |
| 9 | latched temperature (b0-3) and Vcc (b4-7) HA/LA/HW/LW | COR |
| 10 | latched Aux1 (laser temp) b0-3, Aux2 (TEC drive) b4-7 | COR |
| 14-15 | temperature S16 1/256 °C | RO |
| 16-17 | Vcc U16 100 µV | RO |
| 18-19 | Aux1 laser plate temperature S16 1/256 °C | RO |
| 20-21 | Aux2 TEC drive S16, 32767 = 100 % cooling | RO |
| 26 | b3 SoftwareReset, b4 LowPwrRequestSW, b6 LowPwrAllowRequestHW | RW |
| 31-34 | masks for bytes 8-11 | RW |
| 37 | CdbStatus1 (b7 busy, b6 fail, b5-0 result) | RO |
| 39-40 | active firmware version | RO |
| 41 | fault cause (this design) | RO |
| 85 | media type 0x02 SMF | RO |
| 86-93 | AppSel 1: 0x11/0x1C 8/4 lanes; AppSel 2: 0x0D/0x14 2/1 lanes, assign 0x55; 94 = 0xFF | RO |
| 118-125 | password change / entry (write-only) | WO |
| 126 / 127 | bank select / page select | RW |

## Upper pages

| Page | Content |
|---|---|
| 00h | vendor name/OUI/PN/rev/SN/date/CLEI, power class 8, max power 10 W, MPO-12, checksum at 222 |
| 01h | inactive FW version, HW rev, 1311 nm ± 6.5 nm, durations, supported monitors, checksum at 255 |
| 02h | thresholds HA/LA/HW/LW for temp, Vcc, Aux1, Aux2, TX power, TX bias, RX power, checksum at 255 |
| 03h | user EEPROM, persistent |
| 10h | DPDeinit 128, TX disable 130, ApplyDPInit 143, DPConfig 145-152, lane flag masks 213-231 |
| 11h | DP state 128-131, output status 132-133, latched lane flags 134-152 (COR), TX power / bias / RX power 154-201, config status 202-205, active config 206-213 |
| 9Fh | CDB LPL: CMD 128, EPL len 130, LPL len 132, check code 133, RPL len/check 134-135, payload 136-255 |
| B0h | vendor diagnostics: uptime, reset count/reason, boot bank, fault log size, I2C error counters, DSP recoveries, pre-FEC BER (float), SNR, FEC counters, TEC setpoint, APC state, DSP temperature, last event |
| B1h | factory calibration (needs password 0x4F4D5401): raw ADC at 128-147, coefficients 160-247, command 255 (A5 commit, 5A reload, C3 defaults) |
