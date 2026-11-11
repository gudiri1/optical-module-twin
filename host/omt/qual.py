# SPDX-License-Identifier: Apache-2.0
"""Qualification campaign on the twin: accelerated laser aging (HTOL-style
readpoints with an end-of-life prediction), temperature cycling, and power
cycling with flash-wear projection.

What the twin can and cannot qualify is listed in docs/qualification.md.
The aging law and its constants are modeling assumptions, stated in
:class:`AgingModel`; the firmware behavior under them (APC compensation,
bias alarms, EOL shutdown, NVM wear) is what these procedures verify.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass

import numpy as np

from . import cmis
from .dvt import LIM, Result, fresh, fresh_bench

K_B_EV = 8.617e-5
HOURS_PER_YEAR = 8760.0

QLIM = {
    "bias_drift_pct": 50.0,      # laser failure criterion: operating current +50 % (GR-468 style; verify)
    "tx_drift_db": 0.5,          # APC must hold launch power through aging
    "warn_lead_years": 1.0,      # bias warning must come at least this long before EOL shutdown
    "prediction_err": 0.20,      # extrapolation vs. the twin's actual crossing
    "nor_endurance": 100_000,    # erase cycles per sector (typical NOR datasheet)
    "power_cycles_per_day": 100,
    "service_years": 20,
}


def arrhenius_af(ea_ev: float, t_use_c: float, t_stress_c: float) -> float:
    return math.exp(ea_ev / K_B_EV * (1 / (t_use_c + 273.15) - 1 / (t_stress_c + 273.15)))


@dataclass
class AgingModel:
    """Laser degradation d(t) = k * (t / life_ref)^n in the twin's units
    (0 = new; threshold +150 %, slope -40 % at d = 1). Assumptions: 0.7 eV,
    laser held at 50 C by the TEC in use, 85 C laser in the stress test,
    square-root law, +/-15 % unit-to-unit spread of k."""
    ea_ev: float = 0.7
    t_use_c: float = 50.0
    t_stress_c: float = 85.0
    life_ref_years: float = 40.0
    exponent: float = 0.5
    unit_spread: float = 0.15

    @property
    def af(self) -> float:
        return arrhenius_af(self.ea_ev, self.t_use_c, self.t_stress_c)

    def degradation(self, years: float, k: float) -> float:
        return k * (max(years, 0.0) / self.life_ref_years) ** self.exponent


# ------------------------------------------------------------------ HTOL

def _lane_bias(h) -> tuple[list[float], list[float]]:
    p11 = cmis.decode_page11(h.upper(0x11))
    return [ln["tx_bias_ma"] for ln in p11["lanes"]], [ln["tx_power_dbm"] for ln in p11["lanes"]]


def htol(seed=71, hours=(0, 168, 500, 1000, 2000), model: AgingModel | None = None,
         settle_ms=3000) -> Result:
    """Readpoints after accelerated stress; extrapolate bias to the warning
    threshold and compare with where the twin's laser actually gets there."""
    model = model or AgingModel()
    m, h = fresh(seed)
    h.bring_up()
    m.run(2000)
    rng = random.Random(seed)
    k = [max(0.5, 1 + model.unit_spread * rng.gauss(0, 1)) for _ in range(4)]
    r = Result(f"HTOL readpoints ({model.t_stress_c:.0f} C laser, Ea {model.ea_ev} eV, AF {model.af:.1f})", True)
    rows = []
    for hr in hours:
        yrs = hr * model.af / HOURS_PER_YEAR
        for lane in range(4):
            m.set(f"aging.{lane}", model.degradation(yrs, k[lane]))
        m.run(settle_ms)
        h.read_flags()
        bias, txp = _lane_bias(h)
        rows.append({"hours": hr, "use_years": yrs, "bias_ma": bias, "tx_dbm": txp,
                     "tx_true_dbm": [m.get(f"tx_dbm.{i}") for i in range(4)]})
    r.data.update(rows=rows, k=k, af=model.af)
    b0, b1 = rows[0]["bias_ma"], rows[-1]["bias_ma"]
    drift = [100 * (b1[i] / b0[i] - 1) for i in range(4)]
    r.data["bias_drift_pct"] = drift
    r.check(f"bias increase after {hours[-1]} h <= {QLIM['bias_drift_pct']:.0f} %",
            max(drift) <= QLIM["bias_drift_pct"], " / ".join(f"{d:+.1f} %" for d in drift))
    tx = max(abs(rows[-1]["tx_true_dbm"][i] - rows[0]["tx_true_dbm"][i]) for i in range(4))
    r.check("APC holds launch power through aging", tx <= QLIM["tx_drift_db"], f"max {tx:.2f} dB")

    # extrapolate: bias is smooth in sqrt(t) -> quadratic fit, solve for the warning level
    warn = 90.0
    pred = []
    for i in range(4):
        x = np.array([math.sqrt(row["use_years"]) for row in rows])
        y = np.array([row["bias_ma"][i] for row in rows])
        c = np.polyfit(x, y, 2)
        roots = [z.real for z in np.roots([c[0], c[1], c[2] - warn]) if abs(z.imag) < 1e-9 and z.real > x[-1]]
        pred.append(min(roots) ** 2 if roots else float("inf"))
    # ground truth: age the twin until the firmware raises the bias warning
    truth = []
    for i in range(4):
        lo, hi = rows[-1]["use_years"], 200.0
        for _ in range(14):                      # bisection on the warning flag
            mid = math.sqrt(lo * hi)
            m.set(f"aging.{i}", model.degradation(mid, k[i]))
            m.run(settle_ms)
            h.read_flags()
            m.run(200)
            warned = bool(h.read_flags()["tx_bias_high_warning"] >> i & 1) or _lane_bias(h)[0][i] >= warn
            lo, hi = (lo, mid) if warned else (mid, hi)
        truth.append(hi)
        m.set(f"aging.{i}", model.degradation(rows[-1]["use_years"], k[i]))
    r.data.update(pred_years=pred, true_years=truth)
    err = max(abs(p - t) / t for p, t in zip(pred, truth))
    r.check(f"extrapolated life within {QLIM['prediction_err'] * 100:.0f} % of the twin's actual crossing",
            err <= QLIM["prediction_err"],
            " / ".join(f"{p:.1f} vs {t:.1f} y" for p, t in zip(pred, truth)))
    # end of life: the APC clamp (105 mA) can no longer reach target and the
    # firmware shuts the lane down. Fresh unit per probe: the shutdown latches.
    eol = []
    for i in range(4):
        lo, hi = truth[i], 400.0
        for _ in range(12):
            mid = math.sqrt(lo * hi)
            u, uh = fresh(seed)
            uh.bring_up()
            u.set(f"aging.{i}", model.degradation(mid, k[i]))
            u.run(settle_ms)
            dead = u.get(f"bias_ma.{i}") == 0
            u.close()
            lo, hi = (lo, mid) if dead else (mid, hi)
        eol.append(hi)
    lead = [e - t for e, t in zip(eol, truth)]
    r.data.update(eol_years=eol, warn_lead_years=lead)
    r.check(f"bias warning at least {QLIM['warn_lead_years']:.0f} year before EOL shutdown (every lane)",
            min(lead) >= QLIM["warn_lead_years"],
            " / ".join(f"warn {t:.1f} y, EOL {e:.1f} y" for t, e in zip(truth, eol)))
    r.data["life_note"] = (f"model life to bias warning {min(pred):.1f}..{max(pred):.1f} years at "
                           f"{model.t_use_c:.0f} C laser (assumed aging law, see AgingModel)")
    return r


# ------------------------------------------------------------------ cycling

def temp_cycling(seed=72, cycles=20, lo=-5.0, hi=75.0, dwell_s=120.0) -> Result:
    """Functional temperature cycling: link up, no resets, no fault flags,
    TEC and APC in regulation at every extreme. (Mechanical fatigue of
    solder and fiber attach is outside the twin.)"""
    m, h, bench = fresh_bench(seed)
    h.bring_up()
    m.run(2000)
    h.read_flags()
    r = Result(f"Temperature cycling {lo:g}/{hi:g} C, {cycles} cycles", True)
    resets0 = m.get("resets")
    unc0 = sum(cmis.decode_pageB0(h.upper(0xB0))["fec_uncorrectable"])
    rows, bad_flags = [], {}
    watch = ("tx_fault", "tx_los", "tx_cdr_lol", "rx_los", "rx_cdr_lol", "tx_power_low_alarm",
             "tx_bias_high_alarm", "rx_power_low_alarm")
    for c in range(cycles):
        for t in (lo, hi):
            bench.forcer.set_temp(t)
            bench.wait_s(dwell_s)
            f = h.read_flags()
            for k in watch:
                if f[k]:
                    bad_flags[k] = bad_flags.get(k, 0) | f[k]
            lower = cmis.decode_lower(h.lower())
            rows.append({"cycle": c + 1, "case_c": t, "laser_c": bench.dmm.temperature(2),
                         "tx_dbm": [bench.pm.read_dbm(i + 1) for i in range(4)],
                         "state": lower["module_state"], "power_w": bench.supply.power_w()})
    r.data["rows"] = rows
    r.check("no controller resets", m.get("resets") == resets0, f"{int(m.get('resets') - resets0)} resets")
    r.check("module Ready and all data paths active throughout",
            all(x["state"] == "Ready" for x in rows) and all(s == 4 for s in h.dp_states()))
    r.check("no link or transmitter fault flags", not bad_flags,
            ", ".join(f"{k}=0x{v:02X}" for k, v in bad_flags.items()))
    unc = sum(cmis.decode_pageB0(h.upper(0xB0))["fec_uncorrectable"]) - unc0
    r.check("no uncorrectable FEC codewords", unc == 0, str(unc))
    r.check("TEC holds the laser at every extreme", all(abs(x["laser_c"] - 50) <= LIM["laser_temp_c"] for x in rows),
            f"{min(x['laser_c'] for x in rows):.2f}..{max(x['laser_c'] for x in rows):.2f} C")
    first = {x["case_c"]: x for x in rows[:2]}
    last = {x["case_c"]: x for x in rows[-2:]}
    drift = max(abs(last[t]["tx_dbm"][i] - first[t]["tx_dbm"][i]) for t in (lo, hi) for i in range(4))
    r.check("launch power repeatable cycle 1 vs last", drift <= 0.2, f"{drift:.3f} dB")
    return r


def power_cycling(seed=73, cycles=1000, eeprom_bursts=150) -> Result:
    """Power-cycle endurance through the supply, then NVM wear per event and
    the projected flash life for boot records and user EEPROM writes."""
    m, h, bench = fresh_bench(seed)
    h.wait(lambda: h._probe(), 2000, 5)
    r = Result(f"Power-cycle endurance ({cycles} cycles) and NVM wear", True)
    sn = cmis.decode_page00(h.upper(0x00))["vendor_sn"]
    rc0 = cmis.decode_pageB0(h.upper(0xB0))["reset_count"]
    e0 = m.get("flash_erases")
    failures = 0
    for i in range(cycles):
        bench.supply.output(False)
        bench.wait_s(0.03)
        bench.supply.output(True)
        bench.wait_s(0.08)
        if (i + 1) % 100 == 0:
            try:
                h._page = None
                h.wait(lambda: h._probe(), 500, 5)
            except Exception:  # noqa: BLE001 - count, keep cycling
                failures += 1
    bench.wait_s(0.2)
    erases_boot = m.get("flash_erases") - e0
    b0 = cmis.decode_pageB0(h.upper(0xB0))
    r.check("module answers after every 100th cycle", failures == 0, f"{failures} misses")
    r.check("reset count advanced by exactly the number of cycles", b0["reset_count"] - rc0 == cycles,
            f"{b0['reset_count'] - rc0}")
    r.check("factory identity intact", cmis.decode_page00(h.upper(0x00))["vendor_sn"] == sn, sn)
    r.check("full bring-up after the campaign", h.bring_up().t_dpinit_ms > 0)

    # user EEPROM: one host write burst per coalescing window
    e1 = m.get("flash_erases")
    h.select(0x03)
    for b in range(eeprom_bursts):
        h.write(128, bytes([b & 0xFF]) * 8)
        m.run(700)
    erases_ee = m.get("flash_erases") - e1
    per_boot = erases_boot / cycles
    per_burst = erases_ee / eeprom_bursts / 2        # erases alternate between the two sectors
    boots_life = QLIM["nor_endurance"] / per_boot if per_boot else float("inf")
    years_boot = boots_life / QLIM["power_cycles_per_day"] / 365
    r.data.update(erases_per_boot=per_boot, erases_per_eeprom_burst=per_burst, boot_life_years=years_boot,
                  eeprom_bursts_to_wearout=QLIM["nor_endurance"] / per_burst if per_burst else float("inf"))
    r.check(f"boot-record wear: {QLIM['service_years']} y at {QLIM['power_cycles_per_day']} cycles/day",
            years_boot >= QLIM["service_years"], f"{per_boot:.4f} erases/boot -> {years_boot:,.0f} years")
    r.data["eeprom_profile"] = [
        {"writes": label, "per_year": n, "years": QLIM["nor_endurance"] / per_burst / n if per_burst else float("inf")}
        for label, n in (("1 per day", 365), ("1 per hour", 8760), ("1 per minute", 525_600))]
    hourly = r.data["eeprom_profile"][1]["years"]
    r.check(f"user EEPROM wear: >= {QLIM['service_years']} y at one host write per hour",
            hourly >= QLIM["service_years"], f"{per_burst:.4f} erases/sector/write -> {hourly:,.0f} years")
    return r


def run_all(quick: bool = False) -> list[Result]:
    return [htol(),
            temp_cycling(cycles=6 if quick else 20, dwell_s=90 if quick else 120),
            power_cycling(cycles=300 if quick else 1000, eeprom_bursts=120 if quick else 150)]
