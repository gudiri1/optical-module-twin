# SPDX-License-Identifier: Apache-2.0
"""Host-side CMIS driver: what a switch/router transceiver manager does.

Bring-up, flag servicing, lane control and CDB firmware management, written
against an abstract bus (simulator or :class:`omt.bus.LinuxI2cBus`).
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from . import cmis
from .sim import I2cNack

CDB_BUSY = 0x80
CDB_FAIL = 0x40


class CmisError(RuntimeError):
    pass


@dataclass
class BringUpResult:
    t_mgmt_ms: int
    t_pwrup_ms: int
    t_dpinit_ms: int
    config_status: list[str]
    dp_state: list[str]


class CmisHost:
    def __init__(self, bus, chunk: int = 32, retries: int = 20):
        self.bus = bus
        self.chunk = chunk
        self.retries = retries
        self._page: tuple[int, int] | None = None
        self.nacks = 0

    # ------------------------------------------------------------ access
    def _retry(self, fn, *a):
        for _ in range(self.retries):
            try:
                return fn(*a)
            except I2cNack:
                self.nacks += 1
                self._page = None  # module may have reset: page select lost
                self.bus.wait_ms(5)
        raise CmisError("module not responding (NACK)")

    def write(self, off: int, data: bytes | list[int] | int) -> None:
        if isinstance(data, int):
            data = bytes([data])
        self._retry(self.bus.write, off, bytes(data))

    def read(self, off: int, n: int) -> bytes:
        out = b""
        while n > 0:
            k = min(n, self.chunk)
            out += self._retry(self.bus.read, off, k)
            off += k
            n -= k
        return out

    def select(self, page: int, bank: int = 0, force: bool = False) -> None:
        if force or self._page != (page, bank):
            self.write(126, bytes([bank, page]))
            self._page = (page, bank)

    def lower(self) -> bytes:
        return self.read(0, 128)

    def upper(self, page: int, bank: int = 0) -> bytes:
        self.select(page, bank)
        return self.read(128, 128)

    def snapshot(self, pages=(0x00, 0x01, 0x02, 0x10, 0x11, 0xB0)) -> cmis.CmisImage:
        img = cmis.CmisImage(self.lower(), {})
        for p in pages:
            img.pages[p] = self.upper(p)
        return img

    # ------------------------------------------------------------- state
    def module_state(self) -> int:
        return (self.read(3, 1)[0] >> 1) & 7

    def wait(self, cond, timeout_ms: int, poll_ms: int = 10) -> int:
        """Poll cond() until true; returns elapsed ms or raises."""
        t = 0
        while not cond():
            if t >= timeout_ms:
                raise CmisError(f"timeout after {timeout_ms} ms")
            self.bus.wait_ms(poll_ms)
            t += poll_ms
        return t

    def dp_states(self) -> list[int]:
        b = self.upper(0x11)
        return [(b[(lane // 2)] >> (4 * (lane & 1))) & 0xF for lane in range(8)]

    def read_flags(self) -> dict:
        """Read (and thereby clear) all latched flags."""
        lo = self.read(8, 4)
        self.select(0x11)
        lane = self.read(134, 19)
        out = {"module": lo[0], "temp_vcc": lo[1], "aux12": lo[2], "aux3": lo[3]}
        for i, (off, name) in enumerate(cmis.P11_FLAGS.items()):
            out[name] = lane[i]
        return out

    # ---------------------------------------------------------- bring-up
    def bring_up(self, appsel: int = 1, lanes: int = 0xFF, timeout_ms: int = 10000) -> BringUpResult:
        """Typical host sequence: LowPwr -> Ready -> configure -> activate."""
        t_mgmt = self.wait(lambda: self._probe(), 2000, 5)
        self.read_flags()
        self.write(26, 0x00)  # clear LowPwrRequestSW and LowPwrAllowRequestHW
        self.select(0x10)
        self.write(128, 0xFF)  # hold all data paths in DPDeinit while configuring
        t_pwr = self.wait(lambda: self.module_state() == 3, timeout_ms)
        self.select(0x10)
        cfg = bytes([(appsel << 4)] * 8)
        self.write(145, cfg)
        self.write(143, lanes)  # ApplyDPInit (staged set 0)
        self.wait(lambda: all(s != 0xC for s in self._cfg_status()), 1000)
        status = self._cfg_status()
        if any(s != 1 for s, i in zip(status, range(8)) if lanes & (1 << i)):
            raise CmisError(f"configuration rejected: {[cmis.CFG_STATUS.get(s) for s in status]}")
        self.select(0x10)
        self.write(128, (~lanes) & 0xFF)  # release DPDeinit
        t_dp = self.wait(lambda: all(s == 4 for s, i in zip(self.dp_states(), range(8))
                                     if lanes & (1 << i)), timeout_ms)
        return BringUpResult(t_mgmt, t_pwr, t_dp, [cmis.CFG_STATUS.get(s, hex(s)) for s in status],
                             [cmis.DP_STATES.get(s, hex(s)) for s in self.dp_states()])

    def _probe(self) -> bool:
        try:
            self.bus.read(0, 1)
            return True
        except I2cNack:
            self._page = None
            return False

    def _cfg_status(self) -> list[int]:
        b = self.upper(0x11)[202 - 128: 206 - 128]
        return [(b[lane // 2] >> (4 * (lane & 1))) & 0xF for lane in range(8)]

    def low_power(self, on: bool) -> None:
        self.write(26, 0x10 if on else 0x00)

    def tx_disable(self, mask: int) -> None:
        self.select(0x10)
        self.write(130, mask & 0xFF)

    def deinit(self, mask: int = 0xFF) -> None:
        self.select(0x10)
        self.write(128, mask & 0xFF)

    def password(self, pw: int) -> None:
        self.write(122, struct.pack(">I", pw))

    # --------------------------------------------------------------- CDB
    @staticmethod
    def chkcode(hdr: bytes, lpl: bytes) -> int:
        return (~(sum(hdr[0:5]) + sum(lpl))) & 0xFF

    def cdb(self, cmd: int, lpl: bytes = b"", timeout_ms: int = 2000) -> tuple[int, bytes]:
        if len(lpl) > 120:
            raise ValueError("LPL payload > 120 bytes")
        self.select(0x9F)
        if lpl:
            self.write(136, lpl)
        hdr = struct.pack(">HHBBBB", cmd, 0, len(lpl), 0, 0, 0)
        hdr = hdr[:5] + bytes([self.chkcode(hdr, lpl)]) + b"\x00\x00"
        self.write(130, hdr[2:])   # lengths + check code first ...
        self.read(8, 1)            # clear a stale CDB-complete flag
        self.write(128, hdr[:2])   # ... CMDID last triggers execution
        self.wait(lambda: not (self.read(37, 1)[0] & CDB_BUSY), timeout_ms, 1)
        status = self.read(37, 1)[0]
        self.select(0x9F, force=True)
        rlen = self.read(134, 1)[0]
        rpl = self.read(136, rlen) if rlen else b""
        return status, rpl

    def fw_info(self) -> dict:
        st, r = self.cdb(0x0100)
        if st != 1:
            raise CmisError(f"Get FW info failed 0x{st:02X}")
        info = {"status": r[0]}
        for b in range(2):
            d = r[2 + 36 * b: 38 + 36 * b]
            info["AB"[b]] = {"version": f"{d[0]}.{d[1]}", "build": (d[2] << 8) | d[3],
                             "tag": d[4:20].rstrip(b"\0").decode(errors="replace"),
                             "running": bool(r[0] & (1 << (4 * b))),
                             "committed": bool(r[0] & (2 << (4 * b))),
                             "valid": not (r[0] & (4 << (4 * b)))}
        return info

    def firmware_update(self, image: bytes, run: bool = True, commit: bool = True,
                        block: int = 116, progress=None) -> dict:
        """Start -> write blocks -> complete -> run -> (confirm) -> commit."""
        log = {}
        st, _ = self.cdb(0x0101, struct.pack(">II", len(image), 0), 5000)
        if st != 1:
            raise CmisError(f"start download failed 0x{st:02X}")
        for off in range(0, len(image), block):
            st, _ = self.cdb(0x0103, struct.pack(">I", off) + image[off:off + block])
            if st != 1:
                raise CmisError(f"write block @{off} failed 0x{st:02X}")
            if progress:
                progress(off + block, len(image))
        st, _ = self.cdb(0x0107)
        log["complete"] = st
        if st != 1:
            raise CmisError(f"complete download failed 0x{st:02X} (image rejected)")
        if not run:
            return log
        st, _ = self.cdb(0x0109, b"\x00\x00")
        log["run"] = st
        if st != 1:
            raise CmisError(f"run image failed 0x{st:02X}")
        self._page = None
        self.bus.wait_ms(100)             # module resets into the new image
        self.wait(lambda: self._probe(), 2000, 5)
        self.bus.wait_ms(2500)            # firmware confirms after 2 s healthy
        log["after_run"] = self.fw_info()
        if commit:
            st, _ = self.cdb(0x010A)
            log["commit"] = st
            if st != 1:
                raise CmisError(f"commit failed 0x{st:02X}")
        log["final"] = self.fw_info()
        return log
