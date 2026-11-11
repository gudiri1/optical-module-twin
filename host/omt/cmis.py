# SPDX-License-Identifier: Apache-2.0
"""CMIS memory-map decoding (host side).

Works on any source of CMIS bytes: the simulator, a live module behind an
I2C adapter, or a dump file (``ethtool -m ... hex on`` text, raw binary or
the JSON dump written by ``omt dump``).

Offsets follow the layout used by the firmware (modeled on CMIS 5.x). Fields
in vendor pages B0h/B1h are specific to this project.
"""
from __future__ import annotations

import json
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

LOWER = -1

IDENTIFIERS = {
    0x18: "QSFP-DD", 0x19: "OSFP", 0x1A: "SFP-DD", 0x1B: "DSFP", 0x1E: "QSFP+ (CMIS)",
}
MODULE_STATES = {1: "LowPwr", 2: "PwrUp", 3: "Ready", 4: "PwrDn", 5: "Fault"}
DP_STATES = {1: "Deactivated", 2: "DPInit", 3: "DPDeinit", 4: "DPActivated",
             5: "DPTxTurnOn", 6: "DPTxTurnOff", 7: "DPInitialized"}
CFG_STATUS = {0: "Undefined", 1: "Success", 2: "Rejected", 3: "RejectedInvalidAppSel",
              4: "RejectedInvalidDataPath", 6: "RejectedLanesInUse",
              7: "RejectedPartialDataPath", 0xC: "InProgress"}
MEDIA_TYPES = {0: "Undefined", 1: "MMF", 2: "SMF", 3: "Passive copper", 4: "Active cable",
               5: "BASE-T"}
# subset of SFF-8024 codes used by this module (verify against SFF-8024 for others)
HOST_IF = {0x0D: "100GAUI-2 C2M", 0x11: "400GAUI-8 C2M"}
SMF_MEDIA_IF = {0x14: "100GBASE-DR", 0x1C: "400GBASE-DR4"}
CONNECTORS = {0x07: "LC", 0x0C: "MPO 1x12", 0x0D: "MPO 2x16", 0x23: "No separable connector"}
FAULT_CAUSES = {0: "none", 1: "DSP", 2: "laser over-temperature", 3: "module over-temperature",
                4: "Vcc", 5: "laser driver"}
EVENTS = {
    0x01: "BOOT", 0x02: "WDT_RESET", 0x03: "ROLLBACK", 0x04: "SW_RESET", 0x05: "IMAGE_INVALID",
    0x06: "BOOT_CONFIRMED", 0x10: "DSP_NO_RESPONSE", 0x11: "DSP_INIT_TIMEOUT", 0x12: "DSP_HANG",
    0x13: "DSP_RECOVERED", 0x14: "MDIO_ERROR", 0x20: "LASER_EOL", 0x21: "LASER_DRV_FAULT",
    0x22: "EYE_SAFETY", 0x30: "TEC_SATURATED", 0x31: "LASER_OVERTEMP", 0x40: "MODULE_OVERTEMP",
    0x41: "VCC_UNDERVOLT", 0x50: "NVM_CRC", 0x51: "CAL_DEFAULTS", 0x52: "CAL_COMMITTED",
    0x60: "FW_DOWNLOADED", 0x61: "FW_COMMITTED",
}
FLAG_NAMES4 = ("high_alarm", "low_alarm", "high_warning", "low_warning")

# Page 11h flag bytes
P11_FLAGS = {
    134: "dp_state_changed", 135: "tx_fault", 136: "tx_los", 137: "tx_cdr_lol",
    138: "tx_adapt_eq_fault", 139: "tx_power_high_alarm", 140: "tx_power_low_alarm",
    141: "tx_power_high_warning", 142: "tx_power_low_warning", 143: "tx_bias_high_alarm",
    144: "tx_bias_low_alarm", 145: "tx_bias_high_warning", 146: "tx_bias_low_warning",
    147: "rx_los", 148: "rx_cdr_lol", 149: "rx_power_high_alarm", 150: "rx_power_low_alarm",
    151: "rx_power_high_warning", 152: "rx_power_low_warning",
}


# Page 13h/14h diagnostics (CMIS 5.x as modeled by the firmware)
PATTERNS = {0: "PRBS31Q", 1: "PRBS31", 2: "PRBS23Q", 3: "PRBS23", 4: "PRBS15Q", 5: "PRBS15",
            6: "PRBS13Q", 7: "PRBS13", 8: "PRBS9Q", 9: "PRBS9", 10: "PRBS7Q", 11: "PRBS7",
            12: "SSPRQ", 14: "Custom", 15: "User"}
PATTERN_CODES = {v: k for k, v in PATTERNS.items()}
LOOPBACKS = {"media-output": 180, "media-input": 181, "host-output": 182, "host-input": 183}
P14_SEL = {1: "BER", 6: "SNR"}


def f16(v: int) -> float:
    """CMIS F16: exponent bits 15..11, mantissa bits 10..0, value m * 10^(e-24)."""
    return (v & 0x7FF) * 10.0 ** ((v >> 11) - 24)


def _nib(b: bytes, off: int, lane: int) -> int:
    return (b[off + lane // 2] >> (4 * (lane & 1))) & 0xF


def u16(b: bytes, off: int) -> int:
    return (b[off] << 8) | b[off + 1]


def s16(b: bytes, off: int) -> int:
    v = u16(b, off)
    return v - 0x10000 if v & 0x8000 else v


def uw_to_dbm(uw: float) -> float:
    import math
    return 10.0 * math.log10(uw / 1000.0) if uw > 0 else float("-inf")


def ascii_field(b: bytes) -> str:
    return b.decode("ascii", errors="replace").rstrip(" \x00")


@dataclass
class CmisImage:
    """Lower memory + upper pages (each 128 bytes, indexed 128..255)."""

    lower: bytes = b""
    pages: dict[int, bytes] = field(default_factory=dict)

    def page(self, p: int) -> bytes | None:
        return self.pages.get(p)

    def byte(self, p: int, off: int) -> int:
        if off < 128:
            return self.lower[off]
        pg = self.pages[p]
        return pg[off - 128]

    def up(self, p: int) -> bytes:
        """Upper page as a 256-byte buffer so offsets can be used directly."""
        pg = self.pages[p]
        return bytes(128) + pg

    # ------------------------------------------------------------- I/O
    def to_json(self) -> str:
        return json.dumps({"lower": self.lower.hex(),
                           "pages": {f"{k:02X}": v.hex() for k, v in sorted(self.pages.items())}},
                          indent=1)

    @classmethod
    def from_json(cls, text: str) -> "CmisImage":
        d = json.loads(text)
        return cls(bytes.fromhex(d["lower"]),
                   {int(k, 16): bytes.fromhex(v) for k, v in d["pages"].items()})

    @classmethod
    def load(cls, path: str | Path, page: int | None = None) -> "CmisImage":
        """Load JSON, raw binary (256 bytes = lower + page 00h, or 128*n), or
        an ``ethtool -m <if> [page N] hex on`` text dump."""
        p = Path(path)
        data = p.read_bytes()
        if data.lstrip().startswith(b"{"):
            return cls.from_json(data.decode())
        if all(32 <= c < 127 or c in (9, 10, 13) for c in data[:512]):
            return cls.from_hexdump(data.decode(), page)
        return cls.from_raw(data)

    @classmethod
    def from_raw(cls, data: bytes) -> "CmisImage":
        if len(data) < 256 or len(data) % 128:
            raise ValueError("raw dump must be lower memory + whole 128-byte pages")
        img = cls(data[:128], {0x00: data[128:256]})
        # linear layout used by ethtool for paged CMIS: 00h, 01h, 02h, then 10h, 11h
        order = [0x01, 0x02, 0x10, 0x11]
        for i, pg in enumerate(order):
            start = 256 + 128 * i
            if len(data) >= start + 128:
                img.pages[pg] = data[start:start + 128]
        return img

    @classmethod
    def from_hexdump(cls, text: str, page: int | None = None) -> "CmisImage":
        lin: dict[int, int] = {}
        for line in text.splitlines():
            m = re.match(r"\s*0x([0-9a-fA-F]+)\s*:\s*((?:[0-9a-fA-F]{2}\s*)+)", line)
            if not m:
                continue
            base = int(m.group(1), 16)
            for i, h in enumerate(m.group(2).split()):
                lin[base + i] = int(h, 16)
        if not lin:
            raise ValueError("no hex lines found")
        top = max(lin) + 1
        buf = bytes(lin.get(i, 0) for i in range(top))
        if page is not None and min(lin) >= 128:
            # single upper page dump ("ethtool -m eth0 page 0x11 hex on")
            return cls(b"", {page: buf[128:256].ljust(128, b"\0")})
        if top <= 256:
            return cls(buf[:128].ljust(128, b"\0"), {0x00: buf[128:256].ljust(128, b"\0")})
        return cls.from_raw(buf[: top - top % 128])

    def merge(self, other: "CmisImage") -> "CmisImage":
        pages = dict(self.pages)
        pages.update(other.pages)
        return CmisImage(self.lower or other.lower, pages)


# --------------------------------------------------------------------- decoders

def decode_lower(lo: bytes) -> dict:
    st = lo[3]
    apps = []
    for i in range(8):
        d = lo[86 + 4 * i: 90 + 4 * i]
        if d[0] == 0xFF:
            break
        apps.append({
            "appsel": i + 1,
            "host_if": HOST_IF.get(d[0], f"0x{d[0]:02X}"), "host_if_code": d[0],
            "media_if": SMF_MEDIA_IF.get(d[1], f"0x{d[1]:02X}"), "media_if_code": d[1],
            "host_lanes": d[2] >> 4, "media_lanes": d[2] & 0xF, "host_lane_assign": d[3],
        })
    flags9 = lo[9]
    return {
        "identifier": IDENTIFIERS.get(lo[0], f"0x{lo[0]:02X}"), "identifier_code": lo[0],
        "cmis_rev": f"{lo[1] >> 4}.{lo[1] & 0xF}",
        "flat_memory": bool(lo[2] & 0x80),
        "module_state": MODULE_STATES.get((st >> 1) & 7, f"reserved({(st >> 1) & 7})"),
        "module_state_code": (st >> 1) & 7,
        "intl_asserted": not (st & 1),
        "flags": {
            "state_changed": bool(lo[8] & 0x01), "cdb1_complete": bool(lo[8] & 0x40),
            "temp": {n: bool(flags9 & (1 << i)) for i, n in enumerate(FLAG_NAMES4)},
            "vcc": {n: bool(flags9 & (1 << (i + 4))) for i, n in enumerate(FLAG_NAMES4)},
            "aux1": {n: bool(lo[10] & (1 << i)) for i, n in enumerate(FLAG_NAMES4)},
            "aux2": {n: bool(lo[10] & (1 << (i + 4))) for i, n in enumerate(FLAG_NAMES4)},
        },
        "temperature_c": s16(lo, 14) / 256.0,
        "vcc_v": u16(lo, 16) / 10000.0,
        "laser_temp_c": s16(lo, 18) / 256.0,
        "tec_drive_pct": s16(lo, 20) / 327.67,
        "low_power_sw": bool(lo[26] & 0x10), "low_power_allow_hw": bool(lo[26] & 0x40),
        "cdb_status": lo[37],
        "fw_active": f"{lo[39]}.{lo[40]}",
        "fault_cause": FAULT_CAUSES.get(lo[41], f"0x{lo[41]:02X}"),
        "media_type": MEDIA_TYPES.get(lo[85], f"0x{lo[85]:02X}"), "media_type_code": lo[85],
        "applications": apps,
        "page_select": lo[127], "bank_select": lo[126],
    }


def power_class(byte200: int, byte201: int) -> tuple[int, float]:
    cls = ((byte200 >> 5) & 7) + 1
    return cls, byte201 * 0.25


def decode_page00(p: bytes) -> dict:
    b = bytes(128) + p
    cls, maxp = power_class(b[200], b[201])
    return {
        "identifier_code": b[128],
        "vendor_name": ascii_field(b[129:145]), "vendor_oui": b[145:148].hex(":"),
        "vendor_pn": ascii_field(b[148:164]), "vendor_rev": ascii_field(b[164:166]),
        "vendor_sn": ascii_field(b[166:182]), "date_code": ascii_field(b[182:190]),
        "clei": ascii_field(b[190:200]),
        "power_class": cls, "max_power_w": maxp,
        "connector": CONNECTORS.get(b[203], f"0x{b[203]:02X}"),
        "checksum_ok": (sum(b[128:222]) & 0xFF) == b[222],
    }


DURATION = ["<1 ms", "1-5 ms", "5-10 ms", "10-50 ms", "50-100 ms", "100-500 ms", "0.5-1 s",
            "1-5 s", "5-10 s", "10-60 s", "1-5 min", "5-10 min", "10-50 min", "50-100 min",
            ">=100 min", "reserved"]
DURATION_MAX_S = [0.001, 0.005, 0.010, 0.050, 0.100, 0.5, 1, 5, 10, 60, 300, 600, 3000, 6000,
                  float("inf"), float("inf")]


def decode_page01(p: bytes) -> dict:
    b = bytes(128) + p
    return {
        "fw_inactive": f"{b[128]}.{b[129]}", "hw_rev": f"{b[130]}.{b[131]}",
        "wavelength_nm": u16(b, 138) * 0.05, "wavelength_tol_nm": u16(b, 140) * 0.005,
        "max_dp_init": DURATION[b[144] & 0xF], "max_dp_deinit": DURATION[b[144] >> 4],
        "max_dp_init_s": DURATION_MAX_S[b[144] & 0xF],
        "max_module_pwrup": DURATION[b[167] & 0xF], "max_module_pwrdn": DURATION[b[167] >> 4],
        "max_module_pwrup_s": DURATION_MAX_S[b[167] & 0xF],
        "monitors": {"temp": bool(b[159] & 1), "vcc": bool(b[159] & 2),
                     "aux1": bool(b[159] & 4), "aux2": bool(b[159] & 8),
                     "tx_bias": bool(b[160] & 1), "tx_power": bool(b[160] & 2),
                     "rx_power": bool(b[160] & 4)},
        "bias_scale": 1 << ((b[160] >> 3) & 3),
        "checksum_ok": (sum(b[130:255]) & 0xFF) == b[255],
    }


def _thr(b: bytes, off: int, signed: bool, scale: float) -> dict:
    f = s16 if signed else u16
    return {n: f(b, off + 2 * i) * scale for i, n in enumerate(FLAG_NAMES4)}


def decode_page02(p: bytes) -> dict:
    b = bytes(128) + p
    return {
        "temp_c": _thr(b, 128, True, 1 / 256), "vcc_v": _thr(b, 136, False, 1e-4),
        "aux1_c": _thr(b, 144, True, 1 / 256), "aux2_pct": _thr(b, 152, True, 1 / 327.67),
        "tx_power_uw": _thr(b, 176, False, 0.1), "tx_bias_ma": _thr(b, 184, False, 0.002),
        "rx_power_uw": _thr(b, 192, False, 0.1),
        "checksum_ok": (sum(b[128:255]) & 0xFF) == b[255],
    }


def _si(b: bytes, base: int) -> list[dict]:
    """Signal-integrity control set (Page 10h staged at 153, Page 11h active at 214)."""
    return [{"host_lane": h + 1, "adaptive_eq": bool(b[base] >> h & 1),
             "fixed_eq_db": _nib(b, base + 3, h), "tx_cdr": bool(b[base + 7] >> h & 1),
             "rx_cdr": bool(b[base + 8] >> h & 1), "rx_pre": _nib(b, base + 9, h),
             "rx_post": _nib(b, base + 13, h), "rx_amplitude": _nib(b, base + 17, h)} for h in range(8)]


def decode_page10(p: bytes) -> dict:
    b = bytes(128) + p
    return {
        "dp_deinit": b[128], "tx_disable": b[130],
        "staged_dpconfig": [{"appsel": b[145 + i] >> 4, "dpid": (b[145 + i] >> 1) & 7}
                            for i in range(8)],
        "staged_si": _si(b, 153),
    }


def decode_page11(p: bytes, media_lanes: int = 4) -> dict:
    b = bytes(128) + p
    dp = []
    for lane in range(8):
        v = (b[128 + lane // 2] >> (4 * (lane & 1))) & 0xF
        dp.append(DP_STATES.get(v, f"reserved({v})"))
    cfg = []
    for lane in range(8):
        v = (b[202 + lane // 2] >> (4 * (lane & 1))) & 0xF
        cfg.append(CFG_STATUS.get(v, f"0x{v:X}"))
    lanes = []
    for i in range(media_lanes):
        tx = u16(b, 154 + 2 * i) * 0.1
        rx = u16(b, 186 + 2 * i) * 0.1
        lanes.append({
            "lane": i + 1, "tx_power_uw": tx, "tx_power_dbm": uw_to_dbm(tx),
            "tx_bias_ma": u16(b, 170 + 2 * i) * 0.002,
            "rx_power_uw": rx, "rx_power_dbm": uw_to_dbm(rx),
            "flags": [n for off, n in P11_FLAGS.items() if off not in (136, 137, 138) and b[off] & (1 << i)],
        })
    return {
        "dp_state": dp, "config_status": cfg,
        "output_status_tx": b[133], "output_status_rx": b[132],
        "active_appsel": [b[206 + i] >> 4 for i in range(8)],
        "active_si": _si(b, 214),
        "host_lanes": [{"host_lane": h + 1,
                        "flags": [n for off, n in P11_FLAGS.items() if off in (136, 137, 138) and b[off] >> h & 1]}
                       for h in range(8)],
        "lanes": lanes,
        "flag_bytes": {n: b[off] for off, n in P11_FLAGS.items()},
    }


def decode_pageB0(p: bytes) -> dict:
    b = bytes(128) + p
    return {
        "uptime_s": struct.unpack(">I", b[128:132])[0],
        "reset_count": u16(b, 132), "reset_reason": ["power-on", "watchdog", "software", "host pin"][b[134]]
        if b[134] < 4 else b[134],
        "boot_bank": "AB"[b[135] & 1], "fault_log_entries": u16(b, 136),
        "i2c_bad_page": u16(b, 138), "i2c_ro_write": u16(b, 140), "dsp_recoveries": u16(b, 142),
        "prefec_ber": list(struct.unpack(">4f", b[144:160])),
        "snr_db": [u16(b, 160 + 2 * i) / 100 for i in range(4)],
        "fec_corrected": list(struct.unpack(">4I", b[168:184])),
        "fec_uncorrectable": list(struct.unpack(">4I", b[184:200])),
        "tec_setpoint_c": s16(b, 200) / 256, "apc_converged": b[202],
        "dsp_temp_c": s16(b, 204) / 10, "last_event": EVENTS.get(u16(b, 206), hex(u16(b, 206))),
        "host_snr_db": [u16(b, 208 + 2 * h) / 100 for h in range(8)],
        "host_eq_db": [b[224 + h] for h in range(8)],
    }


def _pats(v: int) -> list[str]:
    return [PATTERNS[i] for i in range(16) if v >> i & 1 and i in PATTERNS]


def decode_page13(p: bytes) -> dict:
    b = bytes(128) + p

    def side(en_off, pat_off, n):
        return [{"lane": i + 1, "enabled": bool(b[en_off] >> i & 1),
                 "pattern": PATTERNS.get(_nib(b, pat_off, i), "reserved")} for i in range(n)]
    lb = b[128]
    return {
        "loopback_caps": [n for n, bit in (("media-output", 0), ("media-input", 1), ("host-output", 2),
                                           ("host-input", 3)) if lb >> bit & 1],
        "host_generator_patterns": _pats(u16(b, 132)), "media_generator_patterns": _pats(u16(b, 134)),
        "host_checker_patterns": _pats(u16(b, 136)), "media_checker_patterns": _pats(u16(b, 138)),
        "host_generator": side(144, 148, 8), "media_generator": side(152, 156, 4),
        "host_checker": side(160, 164, 8), "media_checker": side(168, 172, 4),
        "loopbacks": {n: b[off] for n, off in LOOPBACKS.items()},
    }


def decode_page14(p: bytes) -> dict:
    b = bytes(128) + p
    sel = b[128]
    conv = f16 if sel == 1 else (lambda v: v / 256) if sel == 6 else (lambda v: v)
    return {
        "selector": P14_SEL.get(sel, sel),
        "host_checker_lol": b[136], "media_checker_lol": b[137],
        "host": [conv(u16(b, 192 + 2 * h)) for h in range(8)],
        "media": [conv(u16(b, 208 + 2 * m)) for m in range(4)],
    }


CAL_LAYOUT = (  # (name, offset inside B1h coefficient block, count)
    ("temp_offset_c", 0, 1), ("vcc_gain", 4, 1), ("rssi_slope", 8, 4), ("rssi_offset", 24, 4),
    ("mpd_slope", 40, 4), ("laser_temp_offset_c", 56, 1), ("tx_target_dbm", 60, 1),
)


def decode_pageB1(p: bytes) -> dict:
    b = bytes(128) + p
    out = {"raw_temp": u16(b, 128), "raw_vcc": u16(b, 130),
           "raw_rssi": [u16(b, 132 + 2 * i) for i in range(4)],
           "raw_mpd": [u16(b, 140 + 2 * i) for i in range(4)]}
    for name, off, n in CAL_LAYOUT:
        vals = struct.unpack(f">{n}f", b[160 + off: 160 + off + 4 * n])
        out[name] = vals[0] if n == 1 else list(vals)
    out["serial"] = ascii_field(b[224:240])
    out["date_code"] = ascii_field(b[240:248])
    out["command_result"] = b[255]
    return out


def decode(img: CmisImage) -> dict:
    out: dict = {}
    if img.lower:
        out["lower"] = decode_lower(img.lower)
    dec = {0x00: decode_page00, 0x01: decode_page01, 0x02: decode_page02, 0x10: decode_page10,
           0x11: decode_page11, 0x13: decode_page13, 0x14: decode_page14,
           0xB0: decode_pageB0, 0xB1: decode_pageB1}
    for pg, fn in dec.items():
        if pg in img.pages:
            out[f"page{pg:02X}h"] = fn(img.pages[pg])
    return out
