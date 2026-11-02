# SPDX-License-Identifier: Apache-2.0
"""Real-hardware bus: Linux i2c-dev (USB-I2C adapters, host boards, BMCs).

Same interface as :class:`omt.sim.SimModule`, so the host driver, lint, DVT
and manufacturing scripts can run against a physical module. Not exercised
in CI (no hardware); keep it small and obvious.
"""
from __future__ import annotations

import ctypes as C
import fcntl
import os
import time

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001


class _Msg(C.Structure):
    _fields_ = [("addr", C.c_uint16), ("flags", C.c_uint16), ("len", C.c_uint16),
                ("buf", C.POINTER(C.c_uint8))]


class _RdWr(C.Structure):
    _fields_ = [("msgs", C.POINTER(_Msg)), ("nmsgs", C.c_uint32)]


class LinuxI2cBus:
    def __init__(self, bus: int, addr: int = 0x50, max_write: int = 8):
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
        self.addr = addr
        self.max_write = max_write  # CMIS hosts commonly limit block writes to 8 bytes

    def _xfer(self, msgs):
        arr = (_Msg * len(msgs))(*msgs)
        fcntl.ioctl(self.fd, I2C_RDWR, _RdWr(arr, len(msgs)))

    def write(self, offset: int, data: bytes) -> None:
        for i in range(0, len(data), self.max_write):
            chunk = bytes([offset + i]) + bytes(data[i:i + self.max_write])
            b = (C.c_uint8 * len(chunk)).from_buffer_copy(chunk)
            self._xfer([_Msg(self.addr, 0, len(chunk), b)])

    def read(self, offset: int, n: int) -> bytes:
        o = (C.c_uint8 * 1)(offset)
        r = (C.c_uint8 * n)()
        self._xfer([_Msg(self.addr, 0, 1, o), _Msg(self.addr, I2C_M_RD, n, r)])
        return bytes(r)

    def wait_ms(self, ms: int) -> None:
        time.sleep(ms / 1000.0)
