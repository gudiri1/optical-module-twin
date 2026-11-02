# SPDX-License-Identifier: Apache-2.0
"""Manufacturing test station: factory calibration and lot statistics.

Station instruments map to the twin's ground truth:
  reference thermocouple   -> module_temp / laser_temp
  calibrated DMM           -> vcc
  calibrated optical source + VOA -> rx_dbm / rx_mw.<l>
  reference power meter    -> tx_mw.<l>

Flow per unit: power up -> unlock (factory password) -> sample raw ADC
values from vendor page B1h -> compute coefficients -> write + commit with
CRC -> verify DDM accuracy -> record. A lot run reports Cpk per parameter.
"""
from __future__ import annotations

import csv
import statistics
import struct
from dataclasses import dataclass

from . import cmis
from .host import CmisHost
from .sim import SimModule

FACTORY_PASSWORD = 0x4F4D5401
# Station reference uncertainty (1 sigma) - what makes Cpk realistic
REF_SIGMA = {"temp_c": 0.25, "vcc_rel": 0.0008, "optical_db": 0.12}


class Station:
    """Reference instruments with random error (seeded per unit for reproducibility)."""

    def __init__(self, m: SimModule, seed: int):
        import random
        self.m = m
        self.rng = random.Random(seed * 7919)

    def temp(self, name="module_temp"):
        return self.m.get(name) + self.rng.gauss(0, REF_SIGMA["temp_c"])

    def vcc(self):
        return self.m.get("vcc") * (1 + self.rng.gauss(0, REF_SIGMA["vcc_rel"]))

    def optical_mw(self, name):
        return self.m.get(name) * 10 ** (self.rng.gauss(0, REF_SIGMA["optical_db"]) / 10)
SPEC = {  # symmetric limits on (DDM - reference)
    "temp_err_c": 3.0, "vcc_err_pct": 3.0, "rx_err_db": 1.0, "tx_err_db": 1.0, "tx_abs_db": 1.0,
}


def _avg_raw(m: SimModule, h: CmisHost, off: int, n: int = 16) -> float:
    acc = 0
    for _ in range(n):
        m.run(20)
        acc += cmis.u16(h.upper(0xB1), off - 128)
    return acc / n


def measure(m: SimModule, h: CmisHost) -> dict:
    h.read_flags()
    out = {}
    lo = cmis.decode_lower(h.lower())
    out["temp_err_c"] = lo["temperature_c"] - m.get("module_temp")
    out["vcc_err_pct"] = (lo["vcc_v"] - m.get("vcc")) / m.get("vcc") * 100
    rx_err, tx_err, tx_abs = [], [], []
    for level in (-8.0, -3.0, 2.0):
        m.set("rx_dbm", level)
        m.run(400)
        lanes = cmis.decode_page11(h.upper(0x11))["lanes"]
        for i, ln in enumerate(lanes):
            rx_err.append(ln["rx_power_dbm"] - m.get(f"rx_dbm.{i}"))
    m.set("rx_dbm", -3.0)
    lanes = cmis.decode_page11(h.upper(0x11))["lanes"]
    for i, ln in enumerate(lanes):
        tx_err.append(ln["tx_power_dbm"] - m.get(f"tx_dbm.{i}"))
        tx_abs.append(m.get(f"tx_dbm.{i}") - 1.0)
    out["rx_err_db"] = max(rx_err, key=abs)
    out["tx_err_db"] = max(tx_err, key=abs)
    out["tx_abs_db"] = max(tx_abs, key=abs)
    return out


@dataclass
class UnitRecord:
    seed: int
    serial: str
    pre: dict
    post: dict
    coeffs: dict
    passed: bool
    cal_result: int


def calibrate_unit(seed: int, date_code: str = "261031") -> UnitRecord:
    m = SimModule(seed=seed, golden_cal=False)
    h = CmisHost(m)
    h.bring_up()
    m.set("rx_dbm", -3.0)
    m.run(2000)
    pre = measure(m, h)

    h.password(FACTORY_PASSWORD)
    st = Station(m, seed)
    c: dict = {}
    raw_t = _avg_raw(m, h, 128)
    c["temp_offset_c"] = st.temp() - ((raw_t * 2.5 / 65535) - 0.5) / 0.01
    raw_v = _avg_raw(m, h, 130)
    c["vcc_gain"] = st.vcc() / (raw_v * 2.5 / 65535 * 2)
    rs1, rs2, ref1, ref2 = [], [], [], []
    m.set("rx_dbm", -10.0)
    m.run(200)
    for i in range(4):
        rs1.append(_avg_raw(m, h, 132 + 2 * i, 8))
        ref1.append(st.optical_mw(f"rx_mw.{i}") * 1000)
    m.set("rx_dbm", 0.0)
    m.run(200)
    for i in range(4):
        rs2.append(_avg_raw(m, h, 132 + 2 * i, 8))
        ref2.append(st.optical_mw(f"rx_mw.{i}") * 1000)
    c["rssi_slope"] = [(ref2[i] - ref1[i]) / (rs2[i] - rs1[i]) for i in range(4)]
    c["rssi_offset"] = [rs1[i] - ref1[i] / c["rssi_slope"][i] for i in range(4)]
    c["mpd_slope"] = []
    for i in range(4):
        raw = _avg_raw(m, h, 140 + 2 * i, 8)
        c["mpd_slope"].append(st.optical_mw(f"tx_mw.{i}") * 1000 / raw)
    aux1 = cmis.decode_lower(h.lower())["laser_temp_c"]
    c["laser_temp_offset_c"] = st.temp("laser_temp") - aux1
    c["tx_target_dbm"] = 1.0
    serial = f"OMT26{seed % 10_000_000:07d}"

    blk = bytearray(h.upper(0xB1)[32:])  # bytes 160..255 of page B1h
    blk[0:4] = struct.pack(">f", c["temp_offset_c"])
    blk[4:8] = struct.pack(">f", c["vcc_gain"])
    blk[8:24] = struct.pack(">4f", *c["rssi_slope"])
    blk[24:40] = struct.pack(">4f", *c["rssi_offset"])
    blk[40:56] = struct.pack(">4f", *c["mpd_slope"])
    blk[56:60] = struct.pack(">f", c["laser_temp_offset_c"])
    blk[60:64] = struct.pack(">f", c["tx_target_dbm"])
    blk[64:80] = serial.encode().ljust(16)
    blk[80:88] = date_code.encode().ljust(8)
    h.select(0xB1)
    h.write(160, bytes(blk[:95]))
    h.write(255, 0xA5)                       # commit to flash
    m.run(100)
    result = h.upper(0xB1)[127]
    h.write(122, b"\0\0\0\0")                # lock again
    m.run(1500)                              # APC re-targets with the new MPD slope
    post = measure(m, h)
    ok = result == 0x01 and all(abs(post[k]) <= SPEC[k] for k in SPEC)
    sn = cmis.decode_page00(h.upper(0))["vendor_sn"]
    m.close()
    return UnitRecord(seed, sn, pre, post, c, ok, result)


def cpk(values, lim):
    mu = statistics.fmean(values)
    sd = statistics.pstdev(values) or 1e-12
    return min(lim - mu, mu + lim) / (3 * sd), mu, sd


def run_lot(n: int = 40, seed0: int = 1000) -> dict:
    units = [calibrate_unit(seed0 + i) for i in range(n)]
    stats = {}
    for k, lim in SPEC.items():
        pre = [u.pre[k] for u in units]
        post = [u.post[k] for u in units]
        c_pre, mu_pre, sd_pre = cpk(pre, lim)
        c_post, mu_post, sd_post = cpk(post, lim)
        stats[k] = {"limit": lim, "pre": pre, "post": post, "cpk_pre": c_pre, "cpk_post": c_post,
                    "mean_post": mu_post, "sd_post": sd_post, "worst_pre": max(pre, key=abs),
                    "worst_post": max(post, key=abs)}
    return {"units": units, "stats": stats, "yield": sum(u.passed for u in units) / n}


def write_csv(lot: dict, path: str) -> None:
    keys = list(SPEC)
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["serial", "seed", "cal_result", "pass"] + [f"pre_{k}" for k in keys]
                   + [f"post_{k}" for k in keys] + ["temp_offset_c", "vcc_gain"]
                   + [f"rssi_slope_{i}" for i in range(4)] + [f"mpd_slope_{i}" for i in range(4)])
        for u in lot["units"]:
            w.writerow([u.serial, u.seed, f"0x{u.cal_result:02X}", u.passed]
                       + [f"{u.pre[k]:.4f}" for k in keys] + [f"{u.post[k]:.4f}" for k in keys]
                       + [f"{u.coeffs['temp_offset_c']:.4f}", f"{u.coeffs['vcc_gain']:.6f}"]
                       + [f"{x:.6f}" for x in u.coeffs["rssi_slope"]]
                       + [f"{x:.5f}" for x in u.coeffs["mpd_slope"]])

