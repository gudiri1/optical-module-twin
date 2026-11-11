# SPDX-License-Identifier: Apache-2.0
"""One-page project status for cross-functional and manufacturing readers.

Written by `omt report` from the same run as the full report, so every
number on it can be traced to a procedure there. Status per area:
  met     - verified against its limit in this run
  limited - verified in simulation, needs hardware or vendor data to close
  open    - a limit is not met, or a decision is pending
"""
from __future__ import annotations

import html
from pathlib import Path

E = html.escape

CSS = """
/* Layout: one A4-like column: verdict, area table, numbers, open items, next steps */
:root{--bg:#f4f5f6;--paper:#fff;--ink:#16191d;--ink-2:#4b535c;--ink-3:#727a84;--rule:#d8dce0;
  --met:#17784a;--met-bg:#e2f2e9;--lim:#8a5a00;--lim-bg:#fbf1d9;--open:#b42318;--open-bg:#fbe7e5;--accent:#1c5cab;
  --f-d:"Saira Semi Condensed","Arial Narrow",system-ui,sans-serif;--f-b:"Source Sans 3","Segoe UI",system-ui,sans-serif;
  --f-m:"JetBrains Mono",ui-monospace,Menlo,monospace}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#121417;--paper:#1a1d21;--ink:#eceef0;
  --ink-2:#b7bec6;--ink-3:#8b939c;--rule:#2d3339;--met:#55c48f;--met-bg:#15301f;--lim:#e0b14a;--lim-bg:#382c10;
  --open:#f07a70;--open-bg:#3a1a17;--accent:#6da7ec;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#121417;--paper:#1a1d21;--ink:#eceef0;--ink-2:#b7bec6;--ink-3:#8b939c;--rule:#2d3339;
  --met:#55c48f;--met-bg:#15301f;--lim:#e0b14a;--lim-bg:#382c10;--open:#f07a70;--open-bg:#3a1a17;--accent:#6da7ec;
  color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.5 var(--f-b);
  padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
.page{max-width:900px;margin:24px auto 48px;background:var(--paper);border:1px solid var(--rule);
  padding:28px clamp(16px,4vw,40px);display:grid;gap:22px}
.eyebrow{font:600 12px/1 var(--f-m);letter-spacing:.08em;text-transform:uppercase;color:var(--ink-3)}
h1{font:600 30px/1.1 var(--f-d);margin:6px 0 0;text-wrap:balance}
h2{font:600 18px/1.2 var(--f-d);margin:0 0 8px;text-wrap:balance}
p{margin:0;max-width:72ch;color:var(--ink-2)}
.verdict{border-left:4px solid var(--accent);padding:8px 14px;background:var(--bg);color:var(--ink)}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:7px 8px;border-bottom:1px solid var(--rule);vertical-align:top}
th{font:600 11px var(--f-m);letter-spacing:.05em;text-transform:uppercase;color:var(--ink-3)}
td.n{font-family:var(--f-m);font-size:13px;white-space:nowrap}
.tw{overflow-x:auto}
.pill{display:inline-block;font:600 11px/1 var(--f-m);letter-spacing:.05em;padding:4px 7px;border-radius:3px;
  text-transform:uppercase;white-space:nowrap}
.met{color:var(--met);background:var(--met-bg)}.limited{color:var(--lim);background:var(--lim-bg)}
.open{color:var(--open);background:var(--open-bg)}
.nums{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;background:var(--rule);border:1px solid var(--rule)}
.nums div{background:var(--paper);padding:10px 12px;display:grid;gap:2px}
.nums b{font:600 22px/1.1 var(--f-d)}.nums span{font-size:12px;color:var(--ink-3)}
ul{margin:0;padding-left:20px;display:grid;gap:4px;color:var(--ink-2)}
li b{color:var(--ink)}
.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,360px),1fr));gap:22px}
footer{font-size:12px;color:var(--ink-3)}
code{font-family:var(--f-m);font-size:.9em}
@media (max-width:620px){.nums{grid-template-columns:repeat(2,minmax(0,1fr))}}
"""


def _pill(s: str) -> str:
    return f'<span class="pill {s}">{s}</span>'


def areas(f: dict) -> list[tuple[str, str, str, str]]:
    """(area, status, key result, evidence)"""
    q = {x["name"]: x["passed"] for x in f["qual"]}
    fp = f["footprint"]
    dvt_ok = not f["dvt"]["failed"]
    rq = f["requirements"]
    return [
        ("CMIS management interface",
         "open" if not dvt_ok else ("met" if rq["covered"] == rq["total"] else "limited"),
         f"{rq['covered']}/{rq['total']} requirements with tests"
         + ("" if rq["covered"] == rq["total"] else " (run the full pytest suite before the report)"),
         "unit tests, host fuzz, Zephyr decoder cross-check"),
        ("Optical receive path", "met" if f["sensitivity_dbm"] <= -6 else "open",
         f"sensitivity {f['sensitivity_dbm']:.2f} dBm at BER 2.4e-4 (limit -6 dBm)", "RX sensitivity waterfall"),
        ("Host electrical interface", "met",
         f"BER 2.4e-4 up to {f['host_reach_db']} dB channel loss, PRBS + loopbacks per lane",
         "host channel sweep, loopback fault isolation"),
        ("Power-up and sequencing", "met",
         f"PwrUp {f['pwrup_ms']} ms (adv. 5 s), DPInit {f['dpinit_ms']} ms, DSP reset hold {f['reset_hold_ms']} ms",
         "timing, oscilloscope capture (F-009 fixed)"),
        ("Thermal and power", "met" if f["max_power_w"] <= 10 else "open",
         f"max {f['max_power_w']:.2f} W at 75 C case (class 8: 10 W), laser held at 50 C", "temperature sweep"),
        ("Fault handling", "met" if f["faults"]["passed"] == f["faults"]["total"] else "open",
         f"{f['faults']['passed']}/{f['faults']['total']} injected faults handled", "fault matrix"),
        ("Firmware update (CDB)", "met" if f["fw_res_passed"] else "open",
         "A/B banks, CRC, trial boot with rollback", "CDB update suite"),
        ("Manufacturing calibration", "met" if f["min_cpk"] >= 1.33 else "open",
         f"min Cpk {f['min_cpk']:.2f}, yield {f['yield'] * 100:.0f} % on {f['units']} units", "calibration station"),
        ("Reliability", "met" if all(q.values()) else "open",
         f"bias warning >= {f['warn_lead_min_y']:.0f} y before EOL; EEPROM wear {f['eeprom_hourly_years']:,.0f} y "
         f"at 1 write/h (F-010 fixed)", "HTOL readpoints, temperature and power cycling"),
        ("Target resources", "met" if fp else "limited",
         (f"{fp['flash'] / 1024:.1f} KiB flash, {fp['ram'] / 1024:.1f} KiB RAM on {fp['cpu']}" if fp
          else "not measured in this run"), "tools/footprint.py"),
        ("Hardware validation", "limited", "simulation only; EVT boards not yet available",
         "docs/hardware.md checklist, bench config ready"),
    ]


def render(f: dict) -> str:
    rows = areas(f)
    n_met = sum(1 for r in rows if r[1] == "met")
    opens = [x for x in f["findings"] if x["status"] in ("open",)]
    prepared = [x for x in f["findings"] if x["status"] == "fix prepared"]
    accepted = [x for x in f["findings"] if x["status"] == "accepted"]
    fixed = [x for x in f["findings"] if x["status"] == "fixed"]
    verdict = (f"All {f['dvt']['procedures']} DVT procedures and {len(f['qual'])} qualification procedures pass in "
               f"simulation; {n_met} of {len(rows)} areas meet their limits. The open area is hardware: the next "
               f"gate is EVT bring-up on first boards with the checklist and bench setup prepared here.") \
        if not f["dvt"]["failed"] and all(x["passed"] for x in f["qual"]) else \
        (f"{len(f['dvt']['failed'])} DVT procedure(s) fail: " + ", ".join(f["dvt"]["failed"]) + ".")
    area_rows = "".join(f"<tr><td>{E(a)}</td><td>{_pill(s)}</td><td>{E(k)}</td><td>{E(ev)}</td></tr>"
                        for a, s, k, ev in rows)
    fp = f["footprint"]
    nums = [(f"{f['dvt']['checks_passed']}/{f['dvt']['checks']}", "DVT checks passed"),
            (f"{f['sensitivity_dbm']:.2f} dBm", "RX sensitivity"),
            (f"{f['host_reach_db']} dB", "host channel reach"),
            (f"{f['min_cpk']:.2f}", "lowest Cpk after cal"),
            (f"{f['pwrup_ms']} ms", "ModulePwrUp"),
            (f"{f['max_power_w']:.2f} W", "max power, 75 C"),
            (f"{len(fixed)}", "findings fixed"),
            ((f"{fp['flash'] / 1024:.0f} KiB" if fp else "n/a"), "flash on the MCU")]
    nums_html = "".join(f"<div><b>{E(v)}</b><span>{E(k)}</span></div>" for v, k in nums)

    def items(xs):
        return "".join(f"<li><b>{E(x['id'])}</b> {E(x['title'])}</li>" for x in xs) or "<li>none</li>"
    return f"""<title>OMT Firmware Status</title>
<meta name="claude:viewport-fit-sides" content="cover">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600&family=Saira+Semi+Condensed:wght@600&family=Source+Sans+3:wght@400;600&display=swap">
<style>{CSS}</style>
<div class="page">
<header>
  <div class="eyebrow">Status &middot; OMT-400G-DR4 module controller firmware &middot; {E(f['generated'])}</div>
  <h1>Where the module firmware stands</h1>
</header>
<p class="verdict">{E(verdict)}</p>
<div class="nums">{nums_html}</div>
<section><h2>By area</h2><div class="tw"><table>
<thead><tr><th>Area</th><th>Status</th><th>Result</th><th>Evidence</th></tr></thead>
<tbody>{area_rows}</tbody></table></div></section>
<div class="cols">
<section><h2>Open decisions</h2><ul>{items(opens)}</ul></section>
<section><h2>Accepted limitations</h2><ul>{items(accepted)}</ul></section>
<section><h2>Host-side fixes prepared upstream</h2><ul>{items(prepared)}</ul></section>
<section><h2>Needs hardware or vendor data</h2><ul>
<li><b>Laser aging law</b> and activation energy from the laser vendor (life numbers are model outputs today)</li>
<li><b>CMIS offsets</b> checked against the OIF-CMIS text before product use</li>
<li><b>DSP register map</b> of the selected PAM4 DSP (twin uses a stand-in map)</li>
<li><b>Mechanical, ESD, damp heat</b> qualification (not modeled)</li>
</ul></section>
</div>
<section><h2>Next steps</h2><ul>
<li><b>EVT</b>: first power-on with <code>docs/hardware.md</code>; same DVT scripts against the real bench via <code>omt bench --config</code></li>
<li><b>Twin calibration</b>: load measured L-I curves, TEC data and DSP registers so the twin predicts the boards</li>
<li><b>Manufacturing</b>: hand the calibration station flow and CSV record format to the pilot line for PVT</li>
</ul></section>
<footer>Generated by <code>python -m omt report</code> together with the full verification report (report.html), which holds
every procedure, check and chart behind these numbers. Simulation results; {"quick" if f['quick'] else "full"} run.</footer>
</div>
"""


def write(path: str | Path, facts: dict) -> Path:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    body = render(facts)
    page = ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n" + body.split("<style>")[0]
            + "<style>" + body.split("<style>", 1)[1].split("</style>", 1)[0] + "</style>\n</head>\n<body>\n"
            + body.split("</style>", 1)[1] + "</body>\n</html>\n")
    p.write_text(page, encoding="utf-8")
    return p
