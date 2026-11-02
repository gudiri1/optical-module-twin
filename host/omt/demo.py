# SPDX-License-Identifier: Apache-2.0
"""Scripted 10-minute walkthrough (`python -m omt demo`).

Tells one story end to end: bring-up -> a field failure -> root cause from
the module's own diagnostics -> a firmware update that goes wrong -> safe
rollback -> a lint of the final state.
"""
from __future__ import annotations

import time

from . import cmis, image, lint
from .host import CmisHost
from .sim import SimModule


def _c(m, cmd):
    return m.console(cmd).replace("omt> ", "")


def _h(title):
    print("\n" + "=" * 78 + f"\n {title}\n" + "=" * 78)


def run(fast: bool = False):
    pause = (lambda: None) if fast else (lambda: time.sleep(0.6))
    m = SimModule(seed=2026)
    h = CmisHost(m)

    _h("1  Bring-up: host takes the module from LowPwr to 400GBASE-DR4")
    r = h.bring_up()
    print(f"ModulePwrUp {r.t_pwrup_ms} ms (TEC settling), DPInit {r.t_dpinit_ms} ms, lanes {r.dp_state[0]}")
    m.run(3000)
    print(_c(m, "mon"), end="")
    pause()

    _h("2  Field failure: laser on lane 2 degrades (aging), host sees IntL")
    h.read_flags()
    m.set("aging.1", 1.2)
    m.run(2000)
    print("IntL pin:", "asserted" if m.get("intl") == 0 else "idle")
    print("flags:", {k: hex(v) for k, v in h.read_flags().items() if v})
    print("APC compensates - bias climbs:", f"{m.get('bias_ma.1'):.1f} mA", "launch", f"{m.get('tx_dbm.1'):.2f} dBm")
    pause()
    m.set("aging.1", 3.0)
    m.run(3000)
    print("aging continues ->", {k: hex(v) for k, v in h.read_flags().items() if v})
    pause()

    _h("3  Root cause from the module's own diagnostics (UART + fault log)")
    print(_c(m, "faults 5"), end="")
    print(_c(m, "lanes"), end="")
    print("-> LASER_EOL on lane index 1: bias pinned at the APC limit, power > 3 dB low;")
    print("   firmware shut the lane down (eye safety), other lanes unaffected:",
          [f"{m.get(f'tx_dbm.{i}'):.2f}" for i in range(4)])
    pause()

    _h("4  DSP firmware hang: detected by heartbeat, recovered by reset")
    h.read_flags()
    m.set("dsp_hang", 1)
    m.run(2000)
    print(_c(m, "dsp"), end="")
    print("events:", _c(m, "faults 3").count("code=0x1"), "DSP events in log; module state",
          cmis.decode_lower(h.lower())["module_state"])
    pause()

    _h("5  Firmware update with an image that hangs at boot -> automatic rollback")
    bad = image.build(1, 9, 99, image.FLAG_CRASH_ON_BOOT, "broken-rc")
    try:
        h.firmware_update(bad, commit=False)
    except Exception as e:
        print("host:", e)
    m.run(3000)
    info = h.fw_info()
    print("running:", "A" if info["A"]["running"] else "B", "| active version",
          cmis.decode_lower(h.lower())["fw_active"], "| watchdog resets", int(m.get("wdt_resets")))
    print(_c(m, "faults 4"), end="")
    pause()

    _h("6  Good update 1.1, run, commit; then lint the module")
    log = h.firmware_update(image.build(1, 1, 11, 0, "rel-1.1"))
    print("final:", {k: v["version"] for k, v in log["final"].items() if k in "AB"},
          "running", "B" if log["final"]["B"]["running"] else "A")
    h.bring_up()
    m.run(3000)
    for f in lint.lint(h.snapshot()):
        print(" ", f)
    print("\nDone. Full evidence: `python -m omt report` -> build/report.html")
