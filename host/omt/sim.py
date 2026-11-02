# SPDX-License-Identifier: Apache-2.0
"""ctypes binding to the simulated module (libomt_sim.so).

``SimModule`` implements the same bus interface as a real I2C adapter, so
everything in :mod:`omt.host` runs unchanged against hardware.
"""
from __future__ import annotations

import ctypes as C
import os
import struct
from pathlib import Path

GOLDEN_CAL = 0x1
BLANK_FLASH = 0x2


class I2cNack(IOError):
    """Module did not acknowledge (reset, boot, or held in ResetL)."""


def _find_lib() -> Path:
    env = os.environ.get("OMT_SIM_LIB")
    if env:
        return Path(env)
    root = Path(__file__).resolve().parents[2]
    for cand in (root / "build" / "libomt_sim.so", root / "build-rel" / "libomt_sim.so"):
        if cand.exists():
            return cand
    raise FileNotFoundError("libomt_sim.so not found - run `cmake -B build && cmake --build build` "
                            "or set OMT_SIM_LIB")


_lib = None


def lib():
    global _lib
    if _lib is None:
        L = C.CDLL(str(_find_lib()))
        L.omt_sim_create.restype = C.c_void_p
        L.omt_sim_create.argtypes = [C.c_uint32, C.c_uint32]
        L.omt_sim_destroy.argtypes = [C.c_void_p]
        L.omt_sim_run.argtypes = [C.c_void_p, C.c_uint32]
        L.omt_sim_time_ms.restype = C.c_uint64
        L.omt_sim_time_ms.argtypes = [C.c_void_p]
        L.omt_sim_i2c_write.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
        L.omt_sim_i2c_read.argtypes = [C.c_void_p, C.c_uint8, C.c_char_p, C.c_size_t]
        L.omt_sim_set.argtypes = [C.c_void_p, C.c_char_p, C.c_double]
        L.omt_sim_get.restype = C.c_double
        L.omt_sim_get.argtypes = [C.c_void_p, C.c_char_p]
        L.omt_sim_console.restype = C.c_size_t
        L.omt_sim_console.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_size_t]
        L.omt_sim_trace_take.restype = C.c_size_t
        L.omt_sim_trace_take.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
        L.omt_sim_flash_read.restype = C.c_size_t
        L.omt_sim_flash_read.argtypes = [C.c_void_p, C.c_uint32, C.c_char_p, C.c_size_t]
        L.omt_sim_build_image.restype = C.c_size_t
        L.omt_sim_build_image.argtypes = [C.c_char_p, C.c_size_t, C.c_uint8, C.c_uint8,
                                          C.c_uint16, C.c_uint32, C.c_char_p]
        _lib = L
    return _lib


class SimModule:
    """One simulated module on a virtual host board."""

    def __init__(self, seed: int = 1, golden_cal: bool = True, blank_flash: bool = False,
                 boot: bool = True):
        flags = (GOLDEN_CAL if golden_cal else 0) | (BLANK_FLASH if blank_flash else 0)
        self._h = lib().omt_sim_create(seed, flags)
        self.seed = seed
        if boot:
            self.run(30)  # MCU boot

    def close(self):
        if self._h:
            lib().omt_sim_destroy(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # ---------------------------------------------------- bus interface
    def write(self, offset: int, data: bytes) -> None:
        buf = bytes([offset]) + bytes(data)
        if lib().omt_sim_i2c_write(self._h, buf, len(buf)) != 0:
            raise I2cNack(f"NACK writing 0x{offset:02X}")

    def read(self, offset: int, n: int) -> bytes:
        out = C.create_string_buffer(n)
        if lib().omt_sim_i2c_read(self._h, offset, out, n) != 0:
            raise I2cNack(f"NACK reading 0x{offset:02X}")
        return out.raw

    def wait_ms(self, ms: int) -> None:
        self.run(ms)

    # --------------------------------------------------------- the lab
    def run(self, ms: int) -> None:
        while ms > 0:
            step = min(ms, 1_000_000)
            lib().omt_sim_run(self._h, step)
            ms -= step

    @property
    def time_ms(self) -> int:
        return lib().omt_sim_time_ms(self._h)

    def set(self, name: str, value: float) -> None:
        if lib().omt_sim_set(self._h, name.encode(), float(value)) != 0:
            raise KeyError(name)

    def get(self, name: str) -> float:
        return lib().omt_sim_get(self._h, name.encode())

    def console(self, line: str) -> str:
        out = C.create_string_buffer(16384)
        n = lib().omt_sim_console(self._h, line.encode(), out, len(out))
        return out.raw[:n].decode(errors="replace")

    def trace_take(self) -> list[tuple[int, str, int, bytes]]:
        """Return [(t_us, kind, offset, data)] and clear the trace."""
        recs = []
        while True:
            buf = C.create_string_buffer(1 << 20)
            n = lib().omt_sim_trace_take(self._h, buf, len(buf))
            raw = buf.raw[:n]
            i = 0
            while i + 6 <= len(raw):
                t_us, kind, ln = struct.unpack_from("<IcB", raw, i)
                body = raw[i + 6: i + 6 + ln]
                recs.append((t_us, kind.decode(), body[0], bytes(body[1:])))
                i += 6 + ln
            if n < len(buf):
                return recs

    def flash_read(self, addr: int, n: int) -> bytes:
        out = C.create_string_buffer(n)
        got = lib().omt_sim_flash_read(self._h, addr, out, n)
        return out.raw[:got]


def build_image(major: int, minor: int, build: int = 0, flags: int = 0, tag: str = "") -> bytes:
    """Firmware image produced by the firmware's own C builder (reference)."""
    out = C.create_string_buffer(4096)
    n = lib().omt_sim_build_image(out, len(out), major, minor, build, flags, tag.encode())
    return out.raw[:n]
