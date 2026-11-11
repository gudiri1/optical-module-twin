# SPDX-License-Identifier: Apache-2.0
"""Command line front end: `python -m omt <command>`.

Every command works on the built-in simulated module (`--seed N`); the
inspection commands also accept dump files, and `--i2c-bus N` talks to a
real module through Linux i2c-dev.
"""
from __future__ import annotations

import argparse
import json
import sys
import time

from . import cmis, image, lint, vcd


def _bus(a):
    if getattr(a, "i2c_bus", None) is not None:
        from .bus import LinuxI2cBus
        return LinuxI2cBus(a.i2c_bus)
    from .sim import SimModule
    m = SimModule(seed=a.seed)
    return m


def _host(a, up=True):
    from .host import CmisHost
    bus = _bus(a)
    h = CmisHost(bus)
    if up and hasattr(bus, "run"):
        h.bring_up(appsel=getattr(a, "appsel", 1))
        bus.run(2000)
    return bus, h


def _image(a) -> cmis.CmisImage:
    if getattr(a, "file", None):
        return cmis.CmisImage.load(a.file, getattr(a, "page", None))
    _, h = _host(a)
    return h.snapshot()


def cmd_info(a):
    d = cmis.decode(_image(a))
    if a.json:
        print(json.dumps(d, indent=1, default=str))
        return 0
    lo, p0 = d.get("lower", {}), d.get("page00h", {})
    print(f"{lo.get('identifier')}  CMIS {lo.get('cmis_rev')}  state {lo.get('module_state')}"
          f"  fw {lo.get('fw_active')}")
    if p0:
        print(f"{p0['vendor_name']} {p0['vendor_pn']} rev {p0['vendor_rev']} SN {p0['vendor_sn']} "
              f"date {p0['date_code']}  class {p0['power_class']} ({p0['max_power_w']} W) {p0['connector']}")
    if lo:
        print(f"temp {lo['temperature_c']:.2f} C  vcc {lo['vcc_v']:.4f} V  laser {lo['laser_temp_c']:.2f} C"
              f"  tec {lo['tec_drive_pct']:+.0f} %  IntL {'asserted' if lo['intl_asserted'] else 'idle'}")
        for ap in lo["applications"]:
            print(f"  AppSel {ap['appsel']}: {ap['host_if']} -> {ap['media_if']} "
                  f"({ap['host_lanes']}/{ap['media_lanes']} lanes, assign 0x{ap['host_lane_assign']:02X})")
    p11 = d.get("page11h")
    if p11:
        print("  DP state:", " ".join(s.replace("DP", "") for s in p11["dp_state"]))
        for ln in p11["lanes"]:
            print(f"  L{ln['lane']}  tx {ln['tx_power_dbm']:6.2f} dBm  bias {ln['tx_bias_ma']:6.2f} mA"
                  f"  rx {ln['rx_power_dbm']:6.2f} dBm  {' '.join(ln['flags'])}")
    b0 = d.get("pageB0h")
    if b0:
        print("  pre-FEC BER " + " ".join(f"{x:.1e}" for x in b0["prefec_ber"])
              + f"  resets {b0['reset_count']} ({b0['reset_reason']})  last event {b0['last_event']}")
    return 0


def cmd_dump(a):
    _, h = _host(a, up=not a.lowpwr)
    img = h.snapshot(pages=(0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0xB0))
    with open(a.out, "w") as fh:
        fh.write(img.to_json())
    print(f"wrote {a.out}")
    return 0


def cmd_lint(a):
    f = lint.lint(_image(a))
    for x in f:
        print(x)
    s = lint.summary(f)
    print(f"{s['error']} errors, {s['warning']} warnings, {s['info']} info")
    return 1 if s["error"] else 0


def cmd_mon(a):
    m, h = _host(a)
    print("   t[s]  temp[C]  vcc[V] laser[C]  " + "  ".join(f"tx{i}[dBm] rx{i}[dBm]" for i in range(1, 5)))
    for i in range(a.seconds):
        lo = cmis.decode_lower(h.lower())
        p11 = cmis.decode_page11(h.upper(0x11))
        print(f"{i:7d}  {lo['temperature_c']:7.2f}  {lo['vcc_v']:6.4f} {lo['laser_temp_c']:8.2f}  "
              + "  ".join(f"{ln['tx_power_dbm']:9.2f} {ln['rx_power_dbm']:9.2f}" for ln in p11["lanes"]))
        m.wait_ms(1000)
    return 0


def cmd_console(a):
    from .sim import SimModule
    m = SimModule(seed=a.seed)
    if not a.lowpwr:
        from .host import CmisHost
        CmisHost(m).bring_up()
    if a.command:
        print(m.console(" ".join(a.command)), end="")
        return 0
    print("UART console of the simulated module ('quit' to exit, '!set name value' injects)")
    while True:
        try:
            line = input("omt> ")
        except EOFError:
            break
        if line.strip() in ("quit", "exit"):
            break
        if line.startswith("!set "):
            _, k, v = line.split()
            m.set(k, float(v))
            continue
        if line.startswith("!run "):
            m.run(int(line.split()[1]))
            continue
        print(m.console(line).replace("omt> ", ""), end="")
    return 0


def cmd_mkimage(a):
    major, minor = (int(x) for x in a.version.split("."))
    img = image.build(major, minor, a.build, image.FLAG_CRASH_ON_BOOT if a.crash_on_boot else 0, a.tag)
    with open(a.out, "wb") as fh:
        fh.write(img)
    print(f"wrote {a.out}: {image.parse(img)}")
    return 0


def cmd_fwupdate(a):
    m, h = _host(a, up=False)
    img = open(a.image, "rb").read()
    t0 = time.time()
    log = h.firmware_update(img, commit=not a.no_commit,
                            progress=lambda d, n: print(f"\r  {min(d, n)}/{n} bytes", end=""))
    print()
    print(json.dumps(log["final"], indent=1))
    print(f"done in {time.time() - t0:.2f} s wall")
    return 0


def cmd_trace(a):
    m, h = _host(a, up=False)
    m.trace_take()
    h.bring_up()
    recs = m.trace_take()
    print(vcd.listing(recs, a.limit))
    if a.vcd:
        n = vcd.to_vcd(recs, a.vcd)
        print(f"wrote {a.vcd} ({len(recs)} transactions, {n} edges) - open in PulseView/GTKWave")
    return 0


def cmd_inject(a):
    m, h = _host(a)
    h.read_flags()
    for kv in a.set:
        k, v = kv.split("=")
        m.set(k, float(v))
    m.run(a.ms)
    print(json.dumps({k: v for k, v in h.read_flags().items() if v}, indent=1))
    print(m.console("state"), m.console(f"faults {a.faults}"), sep="", end="")
    return 0


def cmd_dvt(a):
    from . import dvt
    res = dvt.run_all(quick=a.quick)
    bad = 0
    for r in res:
        print(("PASS " if r.passed else "FAIL ") + r.name)
        for label, ok, det in r.checks:
            if not ok:
                print(f"     x {label} {det}")
        bad += not r.passed
    print(f"{len(res) - bad}/{len(res)} procedures passed")
    return 1 if bad else 0


def cmd_mfg(a):
    from . import mfg
    lot = mfg.run_lot(n=a.units, seed0=a.seed)
    print(f"{'parameter':14s} {'limit':>6s} {'Cpk pre':>8s} {'Cpk post':>9s} {'worst pre':>10s} {'worst post':>11s}")
    for k, s in lot["stats"].items():
        print(f"{k:14s} {s['limit']:6.2f} {s['cpk_pre']:8.2f} {s['cpk_post']:9.2f} "
              f"{s['worst_pre']:10.3f} {s['worst_post']:11.3f}")
    print(f"yield {lot['yield'] * 100:.1f} % of {a.units} units")
    if a.csv:
        mfg.write_csv(lot, a.csv)
        print(f"wrote {a.csv}")
    return 0


def cmd_report(a):
    from . import report
    path = report.generate(a.out, quick=a.quick, units=a.units, role_map=a.role_map)
    print(f"wrote {path}")
    return 0


def _sim_stimuli(m, a) -> None:
    """Twin-only stimuli (ignored on a real module, where the lab sets them)."""
    if not hasattr(m, "set"):
        return
    if getattr(a, "loss", None) is not None:
        m.set("host_loss_db", a.loss)
    if getattr(a, "host_prbs", None):
        m.set("host_prbs", cmis.PATTERN_CODES[a.host_prbs])
    if getattr(a, "far_prbs", None):
        m.set("far_prbs", cmis.PATTERN_CODES[a.far_prbs])
    if getattr(a, "fiber_loopback", None) is not None:
        m.set("loopback", 1)
        m.set("loop_atten_db", a.fiber_loopback)
    for kv in getattr(a, "set", None) or []:
        k, v = kv.split("=")
        m.set(k, float(v))
    m.run(300)


def cmd_serdes(a):
    m, h = _host(a)
    _sim_stimuli(m, a)
    if a.fixed is not None or a.adaptive:
        st = h.set_host_eq(adaptive=a.adaptive, fixed_db=a.fixed or 0)
        print("ApplyImmediate:", ", ".join(sorted(set(st))))
        m.wait_ms(300)
    print("host lane  Eq mode   CTLE dB  SNR dB  flags")
    for x in h.host_lanes(flags=True):
        print(f"{x['host_lane']:9d}  {'adaptive' if x['adaptive'] else 'fixed':8s} {x['eq_db']:8d} "
              f"{x['snr_db']:7.2f}  {' '.join(x['flags']) or '-'}")
    return 0


def cmd_prbs(a):
    m, h = _host(a)
    _sim_stimuli(m, a)
    r = h.prbs_measure(a.side, a.seconds, a.pattern, loopback=a.loopback)
    where = f" with {a.loopback} loopback" if a.loopback else ""
    print(f"{a.side} side {a.pattern} checker, {a.seconds:g} s{where}")
    for i, b in enumerate(r["ber"]):
        print(f"  lane {i + 1}: " + ("LOL (no pattern lock)" if b is None else f"BER {b:.2e}"))
    return 1 if r["lol"] else 0


def cmd_bench(a):
    from .instruments import Bench
    if a.config:
        b = Bench.from_yaml(a.config)
    else:
        m, h = _host(a)
        b = Bench.simulated(m)
    for role, inst in b.instruments().items():
        errs = inst.errors()
        print(f"{role:7s} {inst.idn():32s} {'errors: ' + '; '.join(errs) if errs else 'ok'}")
    if a.sweep:
        b.pm.set_wavelength(1311.0)
        print("\n case C   module C  laser C   supply W   TX1 dBm")
        for tc in a.sweep:
            b.forcer.set_temp(tc)
            b.wait_s(a.soak)
            print(f"{tc:7.1f} {b.dmm.temperature(1):9.2f} {b.dmm.temperature(2):8.2f} "
                  f"{b.supply.power_w():9.2f} {b.pm.read_dbm(1):9.2f}")
    if a.log:
        b.write_log(a.log)
        print(f"\n{len(b.log)} SCPI transactions -> {a.log}")
    return 0


def cmd_qual(a):
    from . import qual
    ok = True
    for r in qual.run_all(quick=a.quick):
        ok &= r.passed
        print(f"{'PASS' if r.passed else 'FAIL'}  {r.name}")
        for label, good, detail in r.checks:
            print(f"   {'ok ' if good else 'BAD'} {label}" + (f"  ({detail})" if detail else ""))
        if "life_note" in r.data:
            print(f"   note: {r.data['life_note']}")
    return 0 if ok else 1


def cmd_demo(a):
    from . import demo
    demo.run(fast=a.fast)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="omt", description="Optical Module Twin host tools")
    p.add_argument("--seed", type=int, default=1, help="simulated unit (default 1)")
    p.add_argument("--i2c-bus", type=int, help="use /dev/i2c-N instead of the simulator")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="decode a module (simulated, live or dump file)")
    s.add_argument("file", nargs="?")
    s.add_argument("--page", type=lambda x: int(x, 0))
    s.add_argument("--json", action="store_true")
    s.add_argument("--appsel", type=int, default=1)
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser("dump", help="write a JSON memory dump")
    s.add_argument("out")
    s.add_argument("--lowpwr", action="store_true")
    s.set_defaults(fn=cmd_dump)
    s = sub.add_parser("lint", help="CMIS consistency checks (exit 1 on errors)")
    s.add_argument("file", nargs="?")
    s.add_argument("--page", type=lambda x: int(x, 0))
    s.set_defaults(fn=cmd_lint)
    s = sub.add_parser("mon", help="print DDM once per second")
    s.add_argument("--seconds", type=int, default=5)
    s.set_defaults(fn=cmd_mon)
    s = sub.add_parser("console", help="UART debug console of the simulated module")
    s.add_argument("command", nargs="*")
    s.add_argument("--lowpwr", action="store_true")
    s.set_defaults(fn=cmd_console)
    s = sub.add_parser("mkimage", help="build a firmware image")
    s.add_argument("--version", default="1.1")
    s.add_argument("--build", type=int, default=0)
    s.add_argument("--tag", default="")
    s.add_argument("--crash-on-boot", action="store_true", help="test image that hangs at boot")
    s.add_argument("-o", "--out", default="omt-fw.bin")
    s.set_defaults(fn=cmd_mkimage)
    s = sub.add_parser("fwupdate", help="CDB firmware update")
    s.add_argument("image")
    s.add_argument("--no-commit", action="store_true")
    s.set_defaults(fn=cmd_fwupdate)
    s = sub.add_parser("trace", help="host bus trace of a bring-up (listing + VCD)")
    s.add_argument("--vcd")
    s.add_argument("--limit", type=int, default=40)
    s.set_defaults(fn=cmd_trace)
    s = sub.add_parser("inject", help="inject faults: omt inject aging.1=3 --ms 3000")
    s.add_argument("set", nargs="+")
    s.add_argument("--ms", type=int, default=3000)
    s.add_argument("--faults", type=int, default=8)
    s.set_defaults(fn=cmd_inject)
    s = sub.add_parser("dvt", help="run the DVT procedures")
    s.add_argument("--quick", action="store_true")
    s.set_defaults(fn=cmd_dvt)
    s = sub.add_parser("mfg", help="calibrate a simulated lot and report Cpk")
    s.add_argument("--units", type=int, default=40)
    s.add_argument("--csv")
    s.set_defaults(fn=cmd_mfg)
    s = sub.add_parser("report", help="full HTML verification report")
    s.add_argument("--out", default="build/report.html")
    s.add_argument("--quick", action="store_true")
    s.add_argument("--units", type=int, default=40)
    s.add_argument("--role-map", metavar="YAML", help="append a requirement-to-evidence table from a YAML file")
    s.set_defaults(fn=cmd_report)
    pats = sorted(cmis.PATTERN_CODES, key=cmis.PATTERN_CODES.get)
    s = sub.add_parser("serdes", help="host-side lanes: CTLE, SNR, flags; set Tx input Eq")
    s.add_argument("--adaptive", action="store_true", help="adaptive Tx input Eq (ApplyImmediate)")
    s.add_argument("--fixed", type=int, metavar="DB", help="fixed Tx input Eq 0..12 dB (ApplyImmediate)")
    s.add_argument("--loss", type=float, help="twin: host channel loss at Nyquist [dB]")
    s.add_argument("--set", nargs="*", metavar="NAME=V", help="twin: further stimuli, e.g. host_los.3=1")
    s.set_defaults(fn=cmd_serdes)
    s = sub.add_parser("prbs", help="PRBS generator/checker BER, optional loopback (Page 13h/14h)")
    s.add_argument("--side", choices=("host", "media"), default="media")
    s.add_argument("--pattern", choices=pats, default="PRBS31Q")
    s.add_argument("--seconds", type=float, default=3.0)
    s.add_argument("--loopback", choices=sorted(cmis.LOOPBACKS))
    s.add_argument("--loss", type=float, help="twin: host channel loss at Nyquist [dB]")
    s.add_argument("--host-prbs", choices=pats, help="twin: pattern the switch ASIC sends")
    s.add_argument("--far-prbs", choices=pats, help="twin: pattern arriving from the far end")
    s.add_argument("--fiber-loopback", type=float, metavar="DB", help="twin: fiber loopback with attenuation")
    s.add_argument("--set", nargs="*", metavar="NAME=V", help="twin: further stimuli, e.g. rx_dbm.2=-9")
    s.set_defaults(fn=cmd_prbs)
    s = sub.add_parser("qual", help="qualification: HTOL readpoints, temperature and power cycling, NVM wear")
    s.add_argument("--quick", action="store_true")
    s.set_defaults(fn=cmd_qual)
    s = sub.add_parser("bench", help="lab instruments over SCPI: identify, optional temperature sweep")
    s.add_argument("--config", metavar="YAML", help="real bench (see docs/lab/bench-example.yaml)")
    s.add_argument("--sweep", type=float, nargs="*", metavar="C", help="case temperatures to step through")
    s.add_argument("--soak", type=float, default=60, help="soak time per step [s]")
    s.add_argument("--log", metavar="CSV", help="write the SCPI transaction log")
    s.set_defaults(fn=cmd_bench)
    s = sub.add_parser("demo", help="scripted 6-step walkthrough")
    s.add_argument("--fast", action="store_true")
    s.set_defaults(fn=cmd_demo)

    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
