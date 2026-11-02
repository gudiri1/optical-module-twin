# SPDX-License-Identifier: Apache-2.0
"""Design verification test (DVT) procedures.

Each procedure drives the module like a lab would (environmental chamber,
programmable supply, variable optical attenuator, reference power meter =
the twin's ground truth) and returns measured data plus pass/fail against
explicit limits. Used by pytest (tests/dvt) and by the HTML report.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

from . import cmis, image
from .host import CmisHost
from .sim import SimModule

# --- acceptance limits (internal DVT spec; SFF/CMIS DDM accuracy is looser) ---
LIM = {
    "temp_acc_c": 3.0,          # DDM temperature vs reference
    "vcc_acc_pct": 3.0,
    "rx_acc_db": 1.0,
    "tx_acc_db": 1.0,
    "tx_stability_db": 0.5,     # APC: launch power over temperature
    "laser_temp_c": 1.0,        # TEC regulation
    "max_power_w": 10.0,        # advertised power class
    "ber_nominal": 1e-6,        # pre-FEC BER at -3 dBm
    "sensitivity_dbm": -6.0,    # avg RX power at KP4 threshold must be below this
}


@dataclass
class Result:
    name: str
    passed: bool
    checks: list[tuple[str, bool, str]] = field(default_factory=list)
    data: dict = field(default_factory=dict)

    def check(self, label: str, ok: bool, detail: str = ""):
        self.checks.append((label, bool(ok), detail))
        if not ok:
            self.passed = False


def fresh(seed=1, **kw) -> tuple[SimModule, CmisHost]:
    m = SimModule(seed=seed, **kw)
    return m, CmisHost(m)


def db(a, b):
    return 10 * math.log10(max(a, 1e-9) / max(b, 1e-9))


# ---------------------------------------------------------------- timing

def power_up_timing(seed=1) -> Result:
    m, h = fresh(seed)
    r = Result("Power-up and data-path timing", True)
    adv = cmis.decode_page01(h.upper(0x01))
    bu = h.bring_up()
    r.data = {"pwrup_ms": bu.t_pwrup_ms, "dpinit_ms": bu.t_dpinit_ms,
              "adv_pwrup_s": adv["max_module_pwrup_s"], "adv_dpinit_s": adv["max_dp_init_s"]}
    r.check("ModulePwrUp within advertised max", bu.t_pwrup_ms / 1000 <= adv["max_module_pwrup_s"],
            f"{bu.t_pwrup_ms} ms <= {adv['max_module_pwrup']}")
    r.check("DPInit within advertised max", bu.t_dpinit_ms / 1000 <= adv["max_dp_init_s"],
            f"{bu.t_dpinit_ms} ms <= {adv['max_dp_init']}")
    r.check("all host lanes DPActivated", all(s == "DPActivated" for s in bu.dp_state))
    return r


# ---------------------------------------------------------------- environment

def temp_sweep(seed=1, temps=(-5, 0, 15, 25, 40, 55, 70, 75), settle_s=100) -> Result:
    m, h = fresh(seed)
    h.bring_up()
    r = Result("Temperature sweep -5..75 degC case", True)
    rows = []
    ref_tx = None
    for tc in temps:
        h.read_flags()                 # clear, then capture what this step latches
        m.set("case_temp", tc)
        m.run(settle_s * 1000)
        fl = h.read_flags()            # COR: flags before the lower-memory read
        lo = cmis.decode_lower(h.lower())
        p11 = cmis.decode_page11(h.upper(0x11))
        b0 = cmis.decode_pageB0(h.upper(0xB0))
        row = {"case_c": tc, "ddm_temp": lo["temperature_c"], "true_temp": m.get("module_temp"),
               "ddm_vcc": lo["vcc_v"], "laser_c": m.get("laser_temp"), "tec_pct": lo["tec_drive_pct"],
               "power_w": m.get("power_w"), "ber": max(b0["prefec_ber"])}
        for i in range(4):
            row[f"tx_ddm_dbm{i}"] = p11["lanes"][i]["tx_power_dbm"]
            row[f"tx_true_dbm{i}"] = m.get(f"tx_dbm.{i}")
            row[f"rx_ddm_dbm{i}"] = p11["lanes"][i]["rx_power_dbm"]
            row[f"rx_true_dbm{i}"] = m.get(f"rx_dbm.{i}")
            row[f"bias_ma{i}"] = p11["lanes"][i]["tx_bias_ma"]
        row["temp_flags"] = fl["temp_vcc"] & 0x0F
        rows.append(row)
        if tc == 25:
            ref_tx = [row[f"tx_true_dbm{i}"] for i in range(4)]
    r.data["rows"] = rows
    for row in rows:
        tc = row["case_c"]
        r.check(f"{tc:>3} C DDM temperature", abs(row["ddm_temp"] - row["true_temp"]) <= LIM["temp_acc_c"],
                f"{row['ddm_temp']:.2f} vs {row['true_temp']:.2f}")
        tx_err = max(abs(row[f"tx_ddm_dbm{i}"] - row[f"tx_true_dbm{i}"]) for i in range(4))
        rx_err = max(abs(row[f"rx_ddm_dbm{i}"] - row[f"rx_true_dbm{i}"]) for i in range(4))
        r.check(f"{tc:>3} C TX DDM accuracy", tx_err <= LIM["tx_acc_db"], f"max err {tx_err:.2f} dB")
        r.check(f"{tc:>3} C RX DDM accuracy", rx_err <= LIM["rx_acc_db"], f"max err {rx_err:.2f} dB")
        if ref_tx:
            drift = max(abs(row[f"tx_true_dbm{i}"] - ref_tx[i]) for i in range(4))
            r.check(f"{tc:>3} C launch power stability (APC)", drift <= LIM["tx_stability_db"],
                    f"{drift:.2f} dB vs 25 C")
        r.check(f"{tc:>3} C TEC regulation", abs(row["laser_c"] - 50.0) <= LIM["laser_temp_c"],
                f"laser {row['laser_c']:.2f} C")
        r.check(f"{tc:>3} C power <= class limit", row["power_w"] <= LIM["max_power_w"],
                f"{row['power_w']:.2f} W")
        r.check(f"{tc:>3} C pre-FEC BER", row["ber"] < LIM["ber_nominal"], f"{row['ber']:.1e}")
        expect_hw = row["ddm_temp"] > 75.0
        got_hw = bool(row["temp_flags"] & 0x4)
        r.check(f"{tc:>3} C temp high warning only above 75 C", expect_hw == got_hw or (
            abs(row["ddm_temp"] - 75) < 1.0), f"DDM {row['ddm_temp']:.1f} C flag={got_hw}")
    return r


def vcc_margin(seed=1, levels=(2.95, 3.10, 3.15, 3.30, 3.45, 3.50, 3.70)) -> Result:
    m, h = fresh(seed)
    h.bring_up()
    r = Result("Supply voltage margining", True)
    rows = []
    for v in levels:
        m.set("vcc", v)
        m.run(1000)
        h.read_flags()
        m.set("vcc", 3.3)
        m.run(500)
        m.set("vcc", v)
        m.run(1000)
        lo = cmis.decode_lower(h.lower())   # this read clears bytes 8..11 (COR) ...
        fl = sum(1 << i for i, k in enumerate(cmis.FLAG_NAMES4) if lo["flags"]["vcc"][k])  # ... use its copy
        rows.append({"vcc": v, "ddm": lo["vcc_v"], "flags": fl, "state": lo["module_state"]})
        err = abs(lo["vcc_v"] - v) / v * 100
        r.check(f"{v:.3f} V DDM accuracy", err <= LIM["vcc_acc_pct"], f"{lo['vcc_v']:.4f} V ({err:.2f} %)")
        exp = (0x1 if v > 3.63 else 0) | (0x2 if v < 2.97 else 0) | (0x4 if v > 3.465 else 0) | (
            0x8 if v < 3.135 else 0)
        near = any(abs(v - t) / t < 0.005 for t in (3.63, 2.97, 3.465, 3.135))
        if not near:  # within DDM accuracy of a threshold either answer is correct
            r.check(f"{v:.3f} V alarm/warning flags", fl == exp, f"flags 0x{fl:X}, expected 0x{exp:X}")
        r.check(f"{v:.3f} V module stays Ready", lo["module_state"] == "Ready")
    r.data["rows"] = rows
    return r


# ---------------------------------------------------------------- receiver

def rx_sensitivity(seed=1, start=-12.0, stop=-3.0, step=0.5, dwell_ms=3000) -> Result:
    m, h = fresh(seed)
    h.bring_up()
    r = Result("Receiver sensitivity (pre-FEC BER waterfall)", True)
    rows = []
    p = start
    while p <= stop + 1e-9:
        m.set("rx_dbm", p)
        m.run(dwell_ms)
        b0 = cmis.decode_pageB0(h.upper(0xB0))
        unc = b0["fec_uncorrectable"][0]
        rows.append({"rx_dbm": p, "ber_meas": b0["prefec_ber"][0], "ber_true": m.get("ber.0"),
                     "snr_db": b0["snr_db"][0], "fec_uncorr": unc,
                     "fec_uncorr_dwell": unc - (rows[-1]["fec_uncorr"] if rows else 0),
                     "ddm_rx_dbm": cmis.decode_page11(h.upper(0x11))["lanes"][0]["rx_power_dbm"]})
        p += step
    r.data["rows"] = rows
    # interpolate sensitivity at KP4 threshold in log-BER domain
    sens = None
    for a, b in zip(rows, rows[1:]):
        if a["ber_meas"] >= 2.4e-4 > b["ber_meas"] > 0:
            la, lb = math.log10(a["ber_meas"]), math.log10(b["ber_meas"])
            sens = a["rx_dbm"] + (math.log10(2.4e-4) - la) / (lb - la) * (b["rx_dbm"] - a["rx_dbm"])
            break
    r.data["sensitivity_dbm"] = sens
    r.check("sensitivity found", sens is not None)
    if sens is not None:
        r.check("sensitivity @ BER 2.4e-4 meets limit", sens <= LIM["sensitivity_dbm"],
                f"{sens:.2f} dBm <= {LIM['sensitivity_dbm']} dBm")
    worst = max(abs(math.log10(max(x["ber_meas"], 1e-12)) - math.log10(max(x["ber_true"], 1e-12)))
                for x in rows if x["ber_true"] > 1e-7)
    r.check("DSP BER counters agree with link truth", worst < 0.3, f"max |log10 err| {worst:.2f}")
    return r


# ---------------------------------------------------------------- faults

def _events(m: SimModule) -> list[str]:
    txt = m.console("faults 20")
    out = []
    for line in txt.splitlines():
        if "code=0x" in line:
            code = int(line.split("code=0x")[1].split()[0], 16)
            out.append(cmis.EVENTS.get(code, hex(code)))
    return out


def _scenario(name, seed, setup, inject, run_ms, expect) -> Result:
    m, h = fresh(seed)
    h.bring_up()
    m.run(3000)
    if setup:
        setup(m, h)
    h.read_flags()
    inject(m, h)
    m.run(run_ms)
    intl = int(m.get("intl"))          # sample IntL before the host services flags
    flags = {k: v for k, v in h.read_flags().items() if v}  # COR: read flags before lower memory
    lo = cmis.decode_lower(h.lower())
    obs = {"state": lo["module_state"], "fault_cause": lo["fault_cause"], "intl_pin": intl,
           "flags": flags, "events": _events(m),
           "b0": cmis.decode_pageB0(h.upper(0xB0))}
    r = Result(name, True, data={"observed": {k: v for k, v in obs.items() if k != "b0"}})
    for label, fn in expect:
        try:
            ok = fn(obs, m, h)
        except Exception as e:  # pragma: no cover - reported as failure
            ok = False
            label += f" ({e})"
        r.check(label, ok)
    return r


def fault_matrix(seed=11) -> list[Result]:
    S = []
    S.append(_scenario(
        "Laser end-of-life on lane 2", seed, None,
        lambda m, h: m.set("aging.1", 3.0), 3000,
        [("TxFault flag lane 2", lambda o, m, h: o["flags"].get("tx_fault", 0) & 0x2),
         ("bias high alarm lane 2", lambda o, m, h: o["flags"].get("tx_bias_high_alarm", 0) & 0x2),
         ("TX power low alarm lane 2", lambda o, m, h: o["flags"].get("tx_power_low_alarm", 0) & 0x2),
         ("LASER_EOL logged", lambda o, m, h: "LASER_EOL" in o["events"]),
         ("lane 2 laser shut down", lambda o, m, h: m.get("bias_ma.1") == 0),
         ("other lanes unaffected", lambda o, m, h: all(m.get(f"tx_dbm.{i}") > 0.5 for i in (0, 2, 3))),
         ("module remains Ready", lambda o, m, h: o["state"] == "Ready")]))
    S.append(_scenario(
        "Laser driver over-current on lane 1", seed, None,
        lambda m, h: m.set("ld_fault.0", 1), 500,
        [("TxFault flag lane 1", lambda o, m, h: o["flags"].get("tx_fault", 0) & 0x1),
         ("LASER_DRV_FAULT logged", lambda o, m, h: "LASER_DRV_FAULT" in o["events"]),
         ("IntL asserted", lambda o, m, h: o["intl_pin"] == 0)]))
    S.append(_scenario(
        "TEC open circuit at 25 C", seed, None,
        lambda m, h: m.set("tec_open", 1), 15000,
        [("laser temperature (Aux1) low warning/alarm", lambda o, m, h: o["flags"].get("aux12", 0) & 0xA),
         ("TEC_SATURATED logged", lambda o, m, h: "TEC_SATURATED" in o["events"]),
         ("APC compensates: launch power held", lambda o, m, h: abs(m.get("tx_dbm.0") - 1.0) < 0.5),
         ("module remains Ready", lambda o, m, h: o["state"] == "Ready")]))
    S.append(_scenario(
        "TEC open circuit at 70 C case (thermal runaway)", seed,
        lambda m, h: (m.set("case_temp", 70), m.run(60000)),
        lambda m, h: m.set("tec_open", 1), 20000,
        [("ModuleFault", lambda o, m, h: o["state"] == "Fault"),
         ("fault cause laser over-temperature", lambda o, m, h: o["fault_cause"] == "laser over-temperature"),
         ("lasers off", lambda o, m, h: all(m.get(f"bias_ma.{i}") == 0 for i in range(4))),
         ("LASER_OVERTEMP logged", lambda o, m, h: "LASER_OVERTEMP" in o["events"])]))
    S.append(_scenario(
        "DSP firmware hang (transient)", seed, None,
        lambda m, h: m.set("dsp_hang", 1), 3000,
        [("DSP_HANG detected", lambda o, m, h: "DSP_HANG" in o["events"]),
         ("DSP_RECOVERED", lambda o, m, h: "DSP_RECOVERED" in o["events"]),
         ("link re-locked", lambda o, m, h: all(m.get(f"media_lock.{i}") for i in range(4))),
         ("module remains Ready", lambda o, m, h: o["state"] == "Ready"),
         ("RX LOL was flagged to host", lambda o, m, h: o["flags"].get("rx_cdr_lol", 0) != 0)]))
    S.append(_scenario(
        "DSP permanently hung", seed, None,
        lambda m, h: m.set("dsp_hang_sticky", 1), 6000,
        [("ModuleFault", lambda o, m, h: o["state"] == "Fault"),
         ("fault cause DSP", lambda o, m, h: o["fault_cause"] == "DSP")]))
    S.append(_scenario(
        "Marginal MDIO bus (20 % transaction errors)", seed, None,
        lambda m, h: m.set("mdio_fail_p", 0.2), 5000,
        [("retries hide errors: module Ready", lambda o, m, h: o["state"] == "Ready"),
         ("no spurious DSP hang", lambda o, m, h: "DSP_HANG" not in o["events"]),
         ("MDIO errors visible in fault log", lambda o, m, h: "MDIO_ERROR" in o["events"])]))
    S.append(_scenario(
        "Controller firmware hang -> watchdog", seed, None,
        lambda m, h: m.set("fw_hang", 1), 1000,
        [("watchdog reset happened", lambda o, m, h: m.get("wdt_resets") == 1),
         ("reset reason reported (B0h)", lambda o, m, h: o["b0"]["reset_reason"] == "watchdog"),
         ("WDT_RESET logged", lambda o, m, h: "WDT_RESET" in o["events"]),
         ("module back in LowPwr, lasers off", lambda o, m, h: o["state"] == "LowPwr"
          and m.get("bias_ma.0") == 0),
         ("host can bring it up again", lambda o, m, h: h.bring_up().t_dpinit_ms > 0)]))
    S.append(_scenario(
        "Module over-temperature (90 C case)", seed, None,
        lambda m, h: m.set("case_temp", 90), 120000,
        [("ModuleFault", lambda o, m, h: o["state"] == "Fault"),
         ("cause module over-temperature", lambda o, m, h: o["fault_cause"] == "module over-temperature")]))
    S.append(_scenario(
        "Supply brown-out (2.8 V)", seed, None,
        lambda m, h: m.set("vcc", 2.8), 1000,
        [("ModuleFault", lambda o, m, h: o["state"] == "Fault"),
         ("cause Vcc", lambda o, m, h: o["fault_cause"] == "Vcc"),
         ("Vcc low alarm latched", lambda o, m, h: o["flags"].get("temp_vcc", 0) & 0x20)]))
    S.append(_scenario(
        "Fiber cut (RX LOS all lanes)", seed, None,
        lambda m, h: m.set("rx_dbm", -40), 1000,
        [("RX LOS flags", lambda o, m, h: o["flags"].get("rx_los", 0) == 0x0F),
         ("RX CDR LOL flags", lambda o, m, h: o["flags"].get("rx_cdr_lol", 0) == 0x0F),
         ("RX power low alarm", lambda o, m, h: o["flags"].get("rx_power_low_alarm", 0) == 0x0F),
         ("IntL asserted", lambda o, m, h: o["intl_pin"] == 0)]))
    S.append(_scenario(
        "Host electrical LOS on lane 3", seed, None,
        lambda m, h: m.set("host_los.2", 1), 500,
        [("TX CDR LOL lane 3 only", lambda o, m, h: o["flags"].get("tx_cdr_lol", 0) == 0x4)]))
    return S


# ---------------------------------------------------------------- firmware mgmt

def fw_update_suite(seed=21) -> list[Result]:
    out = []
    # 1 good update with commit
    m, h = fresh(seed)
    r = Result("CDB firmware update 1.0 -> 1.1, run, commit", True)
    log = h.firmware_update(image.build(1, 1, 11, 0, "rel-1.1"))
    r.check("download complete accepted", log["complete"] == 1)
    r.check("runs new image after reset", log["after_run"]["B"]["running"])
    r.check("committed", log["final"]["B"]["committed"])
    m.set("power_cycle", 1)
    m.run(100)
    r.check("survives power cycle", cmis.decode_lower(h.lower())["fw_active"] == "1.1")
    r.check("host link works on new image", h.bring_up().t_dpinit_ms > 0)
    out.append(r)

    # 2 corrupted image
    m, h = fresh(seed)
    r = Result("Corrupted image is rejected", True)
    img = bytearray(image.build(1, 2, 12, 0, "bad"))
    img[100] ^= 0xFF
    try:
        h.firmware_update(bytes(img))
        r.check("download rejected", False)
    except Exception as e:
        r.check("download rejected with image error", "0x47" in str(e), str(e))
    r.check("still running 1.0", cmis.decode_lower(h.lower())["fw_active"] == "1.0")
    out.append(r)

    # 3 image that crashes at boot -> automatic rollback
    m, h = fresh(seed)
    r = Result("Image crashing at boot rolls back", True)
    try:
        h.firmware_update(image.build(1, 3, 13, image.FLAG_CRASH_ON_BOOT, "crash"), commit=False)
    except Exception:
        pass
    m.run(3000)
    lo = cmis.decode_lower(h.lower())
    r.check("back on 1.0 after watchdog", lo["fw_active"] == "1.0", lo["fw_active"])
    r.check("watchdog fired", m.get("wdt_resets") >= 1)
    r.check("ROLLBACK logged", "ROLLBACK" in _events(m))
    out.append(r)

    # 4 run without commit -> power cycle returns to committed image
    m, h = fresh(seed)
    r = Result("Uncommitted image is not persistent", True)
    h.firmware_update(image.build(1, 4, 14, 0, "trial"), commit=False)
    r.check("running trial 1.4", cmis.decode_lower(h.lower())["fw_active"] == "1.4")
    m.set("power_cycle", 1)
    m.run(100)
    r.check("power cycle returns to 1.0", cmis.decode_lower(h.lower())["fw_active"] == "1.0")
    out.append(r)
    return out


# ---------------------------------------------------------------- robustness

def host_fuzz(seed=5, n=4000) -> Result:
    """Random host traffic, including writes to every page/offset."""
    rnd = random.Random(seed)
    m, h = fresh(seed)
    h.bring_up()
    r = Result(f"Host interface fuzz ({n} random transactions)", True)
    pages = [0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x9F, 0xB0, 0xB1, 0x42, 0xFF]
    nacks = 0
    for _ in range(n):
        try:
            op = rnd.random()
            if op < 0.15:
                m.write(127, bytes([rnd.choice(pages)]))
            elif op < 0.55:
                off = rnd.randrange(0, 256)
                ln = rnd.randrange(1, 9)
                if off <= 26 < off + ln:  # avoid permanent LowPwr/reset storms dominating
                    continue
                m.write(off, bytes(rnd.randrange(256) for _ in range(min(ln, 256 - off))))
            else:
                off = rnd.randrange(0, 256)
                m.read(off, rnd.randrange(1, 129))
        except Exception:
            nacks += 1
        m.run(rnd.randrange(0, 5))
    m.run(2000)
    h._page = None
    lo = h.lower()
    r.check("module still responds", len(lo) == 128)
    r.check("identifier intact", lo[0] == 0x18)
    p0, p1, p2 = cmis.decode_page00(h.upper(0)), cmis.decode_page01(h.upper(1)), cmis.decode_page02(h.upper(2))
    r.check("Page 00h/01h/02h checksums intact", p0["checksum_ok"] and p1["checksum_ok"] and p2["checksum_ok"])
    r.check("no ISR lock leak", m.get("irq_depth") == 0)
    r.check("no unexpected watchdog reset", m.get("wdt_resets") == 0)
    st = cmis.decode_lower(lo)["module_state"]
    r.check("module state valid", st in cmis.MODULE_STATES.values(), st)
    r.data = {"nacks": nacks, "b0": cmis.decode_pageB0(h.upper(0xB0))}
    return r


def breakout(seed=31) -> Result:
    m, h = fresh(seed)
    r = Result("4x100G breakout (AppSel 2) and per-lane control", True)
    bu = h.bring_up(appsel=2)
    r.check("4 data paths activated", all(s == "DPActivated" for s in bu.dp_state))
    h.tx_disable(0x2)  # media lane 2
    m.run(200)
    r.check("lane 2 output disabled", m.get("tx_dbm.1") < -20, f"{m.get('tx_dbm.1'):.1f} dBm")
    r.check("lanes 1,3,4 still on", all(m.get(f"tx_dbm.{i}") > 0.5 for i in (0, 2, 3)))
    h.deinit(0x0C)  # host lanes 3,4 = data path 2
    m.run(200)
    st = h.dp_states()
    r.check("DP2 deactivated, others active", st[2] == 1 and st[3] == 1 and st[0] == 4 and st[4] == 4,
            str(st))
    h.select(0x10)
    h.write(145, bytes([0x10] * 8))
    h.write(143, 0xFF)
    m.run(50)
    cfg = cmis.decode_page11(h.upper(0x11))["config_status"]
    r.check("switch to 400G while lanes active is rejected", cfg[0] == "RejectedLanesInUse", cfg[0])
    return r


def run_all(quick: bool = False) -> list[Result]:
    res = [power_up_timing()]
    res.append(temp_sweep(temps=(0, 25, 70) if quick else (-5, 0, 15, 25, 40, 55, 70, 75),
                          settle_s=60 if quick else 100))
    res.append(vcc_margin())
    res.append(rx_sensitivity(step=1.0 if quick else 0.5))
    res += fault_matrix()
    res += fw_update_suite()
    res.append(breakout())
    res.append(host_fuzz(n=1500 if quick else 4000))
    return res
