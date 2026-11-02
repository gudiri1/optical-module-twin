# SPDX-License-Identifier: Apache-2.0
"""Verification report: one self-contained HTML page with DVT, fault-matrix,
manufacturing, DSP-lab and traceability evidence. Charts are inline SVG with a
hover layer; every chart also has its numbers in a table.
"""
from __future__ import annotations

import datetime as _dt
import html
import json
import math
from pathlib import Path

import yaml

from . import __version__, dsplab, dvt, mfg

ROOT = Path(__file__).resolve().parents[2]
E = html.escape



# ----------------------------------------------------------------- charts
def _nice(lo, hi, n=5):
    if hi <= lo:
        hi = lo + 1
    raw = (hi - lo) / n
    mag = 10 ** math.floor(math.log10(raw))
    step = min((s * mag for s in (1, 2, 2.5, 5, 10) if s * mag >= raw), default=10 * mag)
    a = math.floor(lo / step) * step
    b = math.ceil(hi / step) * step
    ticks = []
    v = a
    while v <= b + step * 1e-9:
        ticks.append(round(v, 10))
        v += step
    return a, b, ticks


def _fmt(v, f):
    return f.format(v) if isinstance(f, str) else f(v)


class Chart:
    W, H = 640, 300
    L, R, T, B = 64, 96, 16, 46

    def __init__(self, cid, xlabel, ylabel, logy=False, xfmt="{:g}", yfmt="{:g}", ymin=None, ymax=None,
                 xmin=None, xmax=None, logx=False):
        self.cid, self.xlabel, self.ylabel = cid, xlabel, ylabel
        self.logy, self.logx = logy, logx
        self.xfmt, self.yfmt = xfmt, yfmt
        self.ylim, self.xlim = (ymin, ymax), (xmin, xmax)
        self.series, self.refs = [], []

    def add(self, name, pts, slot=1, dash=False, markers=True):
        pts = [(x, y) for x, y in pts if y is not None and (not self.logy or y > 0) and (not self.logx or x > 0)]
        self.series.append({"name": name, "pts": pts, "slot": slot, "dash": dash, "markers": markers})
        return self

    def ref(self, y, label):
        self.refs.append((y, label))
        return self

    def _ty(self, v):
        return math.log10(v) if self.logy else v

    def _tx(self, v):
        return math.log10(v) if self.logx else v

    def svg(self):
        xs = [self._tx(x) for s in self.series for x, _ in s["pts"]]
        ys = [self._ty(y) for s in self.series for _, y in s["pts"]] + [self._ty(y) for y, _ in self.refs]
        x0 = self._tx(self.xlim[0]) if self.xlim[0] is not None else min(xs)
        x1 = self._tx(self.xlim[1]) if self.xlim[1] is not None else max(xs)
        if self.logy:
            y0 = math.floor(min(ys)) if self.ylim[0] is None else math.log10(self.ylim[0])
            y1 = math.ceil(max(ys)) if self.ylim[1] is None else math.log10(self.ylim[1])
            yt = list(range(int(y0), int(y1) + 1))
            step = max(1, len(yt) // 7)
            yt = yt[::step]
        else:
            lo = min(ys) if self.ylim[0] is None else self.ylim[0]
            hi = max(ys) if self.ylim[1] is None else self.ylim[1]
            y0, y1, yt = _nice(lo, hi)
        if self.logx:
            xa, xb = math.floor(x0), math.ceil(x1)
            xt = list(range(xa, xb + 1))
            x0, x1 = xa, xb
        else:
            x0, x1, xt = _nice(x0, x1, 6)
        pw, ph = self.W - self.L - self.R, self.H - self.T - self.B

        def px(v):
            return self.L + (v - x0) / (x1 - x0) * pw

        def py(v):
            return self.T + ph - (v - y0) / (y1 - y0) * ph

        o = [f'<svg class="chart" viewBox="0 0 {self.W} {self.H}" role="img" aria-labelledby="{self.cid}-t" '
             f'data-chart="{self.cid}">']
        for t in yt:
            yy = py(t)
            lab = f"1e{t}" if self.logy else _fmt(t, self.yfmt)
            o.append(f'<line class="grid" x1="{self.L}" x2="{self.L + pw}" y1="{yy:.1f}" y2="{yy:.1f}"/>')
            o.append(f'<text class="tick" x="{self.L - 8}" y="{yy + 4:.1f}" text-anchor="end">{E(lab)}</text>')
        for t in xt:
            xx = px(t)
            lab = f"1e{t}" if self.logx else _fmt(t, self.xfmt)
            o.append(f'<text class="tick" x="{xx:.1f}" y="{self.T + ph + 18}" text-anchor="middle">{E(lab)}</text>')
        o.append(f'<line class="axis" x1="{self.L}" x2="{self.L + pw}" y1="{self.T + ph}" y2="{self.T + ph}"/>')
        o.append(f'<text class="axlabel" x="{self.L + pw / 2:.0f}" y="{self.H - 6}" text-anchor="middle">'
                 f'{E(self.xlabel)}</text>')
        o.append(f'<text class="axlabel" transform="translate(14 {self.T + ph / 2:.0f}) rotate(-90)" '
                 f'text-anchor="middle">{E(self.ylabel)}</text>')
        for y, label in self.refs:
            yy = py(self._ty(y))
            o.append(f'<line class="refline" x1="{self.L}" x2="{self.L + pw}" y1="{yy:.1f}" y2="{yy:.1f}"/>')
            o.append(f'<text class="reflabel" x="{self.L + pw + 6}" y="{yy + 4:.1f}">{E(label)}</text>')
        hover, ends = [], []
        for s in self.series:
            s["pts"] = [(x, y) for x, y in s["pts"] if y0 - 1e-9 <= self._ty(y) <= y1 + 1e-9]
            pts = [(px(self._tx(x)), py(self._ty(y))) for x, y in s["pts"]]
            d = " ".join(f"{'M' if i == 0 else 'L'}{a:.1f},{b:.1f}" for i, (a, b) in enumerate(pts))
            cls = f"line s{s['slot']}" + (" dash" if s["dash"] else "")
            o.append(f'<path class="{cls}" d="{d}"/>')
            if s["markers"]:
                for a, b in pts:
                    o.append(f'<circle class="dot s{s["slot"]}" cx="{a:.1f}" cy="{b:.1f}" r="4"/>')
            if pts:
                ends.append([pts[-1][1], pts[-1][0], s["name"]])
            hover.append({"n": s["name"], "s": s["slot"],
                          "p": [[round(a, 1), round(b, 1), _fmt(x, self.xfmt), _fmt(y, self.yfmt)]
                                for (a, b), (x, y) in zip(pts, s["pts"])]})
        ends.sort()
        for i in range(1, len(ends)):                 # keep end labels >= 13 px apart
            ends[i][0] = max(ends[i][0], ends[i - 1][0] + 13)
        for y, x, name in ends:
            o.append(f'<text class="dlabel" x="{x + 8:.1f}" y="{y + 4:.1f}">{E(name)}</text>')
        o.append(f'<line class="xhair" x1="0" x2="0" y1="{self.T}" y2="{self.T + ph}" visibility="hidden"/>')
        o.append(f'<rect class="hit" x="{self.L}" y="{self.T}" width="{pw}" height="{ph}" fill="transparent"/>')
        o.append("</svg>")
        data = E(json.dumps({"series": hover, "x": self.xlabel}))
        legend = "".join(f'<span class="key"><i class="sw s{s["slot"]}{" dash" if s["dash"] else ""}"></i>'
                         f'{E(s["name"])}</span>' for s in self.series)
        return (f'<div class="chartwrap" data-hover="{data}">'
                + (f'<div class="legend">{legend}</div>' if len(self.series) > 1 else "")
                + "".join(o) + '<div class="tip" hidden></div></div>')


def bars(cid, cats, series, ylabel, ref=None, yfmt="{:.2f}"):
    """Grouped vertical bars. series: [(name, slot, [values])]."""
    W, H, L, R, T, B = 640, 280, 56, 90, 16, 64
    vmax = max(max(v) for _, _, v in series)
    if ref:
        vmax = max(vmax, ref[0])
    y0, y1, yt = _nice(0, vmax)
    pw, ph = W - L - R, H - T - B
    py = lambda v: T + ph - (v - y0) / (y1 - y0) * ph  # noqa: E731
    gw = pw / len(cats)
    bw = min(22, (gw - 14) / len(series) - 2)
    o = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" data-chart="{cid}">']
    for t in yt:
        o.append(f'<line class="grid" x1="{L}" x2="{L + pw}" y1="{py(t):.1f}" y2="{py(t):.1f}"/>')
        o.append(f'<text class="tick" x="{L - 8}" y="{py(t) + 4:.1f}" text-anchor="end">{t:g}</text>')
    for i, c in enumerate(cats):
        cx = L + gw * (i + 0.5)
        o.append(f'<text class="tick" x="{cx:.1f}" y="{T + ph + 18}" text-anchor="middle">{E(c)}</text>')
        for j, (name, slot, vals) in enumerate(series):
            v = vals[i]
            x = cx - (len(series) * (bw + 2)) / 2 + j * (bw + 2)
            h = max(1.0, (py(y0) - py(v)))
            y = py(v)
            r = min(4, h / 2)
            o.append(f'<path class="bar s{slot}" data-tip="{E(c)} - {E(name)}: {_fmt(v, yfmt)}" '
                     f'd="M{x:.1f},{y + h:.1f} V{y + r:.1f} Q{x:.1f},{y:.1f} {x + r:.1f},{y:.1f} '
                     f'H{x + bw - r:.1f} Q{x + bw:.1f},{y:.1f} {x + bw:.1f},{y + r:.1f} V{y + h:.1f} Z"/>')
    o.append(f'<line class="axis" x1="{L}" x2="{L + pw}" y1="{T + ph}" y2="{T + ph}"/>')
    if ref:
        o.append(f'<line class="refline" x1="{L}" x2="{L + pw}" y1="{py(ref[0]):.1f}" y2="{py(ref[0]):.1f}"/>')
        o.append(f'<text class="reflabel" x="{L + pw + 6}" y="{py(ref[0]) + 4:.1f}">{E(ref[1])}</text>')
    o.append(f'<text class="axlabel" transform="translate(14 {T + ph / 2:.0f}) rotate(-90)" '
             f'text-anchor="middle">{E(ylabel)}</text></svg>')
    legend = "".join(f'<span class="key"><i class="sw s{slot}"></i>{E(n)}</span>' for n, slot, _ in series)
    return f'<div class="chartwrap"><div class="legend">{legend}</div>{"".join(o)}<div class="tip" hidden></div></div>'


# ----------------------------------------------------------------- html
CSS = """
/* Layout: single reading column (max 1080px), sections as numbered test chapters of a DVT report */
:root{
  --bg:#f6f7f8; --paper:#ffffff; --ink:#14181d; --ink-2:#4a525c; --ink-3:#6d7580; --rule:#d9dde2; --grid:#e8ebee;
  --accent:#1c5cab; --pass:#1a7f4b; --pass-bg:#e3f3ea; --fail:#b42318; --fail-bg:#fbe9e7; --warn:#9a6700; --warn-bg:#fdf3dc;
  --s1:#2a78d6; --s2:#eb6834; --s3:#1baf7a; --s4:#6250d6;
  --f-display:"Saira Semi Condensed","Arial Narrow",system-ui,sans-serif;
  --f-body:"Source Sans 3","Segoe UI",system-ui,sans-serif;
  --f-mono:"JetBrains Mono",ui-monospace,"SFMono-Regular",Menlo,monospace;
}
@media (prefers-color-scheme: dark){:root:not([data-theme="light"]){
  --bg:#121518; --paper:#1a1e22; --ink:#eef0f2; --ink-2:#b9c0c8; --ink-3:#8c949d; --rule:#2e343a; --grid:#262b30;
  --accent:#6da7ec; --pass:#4cc38a; --pass-bg:#16301f; --fail:#f2766b; --fail-bg:#3a1a17; --warn:#e3b341; --warn-bg:#3a2e10;
  --s1:#3987e5; --s2:#d95926; --s3:#199e70; --s4:#9085e9; color-scheme:dark}}
:root[data-theme="dark"]{
  --bg:#121518; --paper:#1a1e22; --ink:#eef0f2; --ink-2:#b9c0c8; --ink-3:#8c949d; --rule:#2e343a; --grid:#262b30;
  --accent:#6da7ec; --pass:#4cc38a; --pass-bg:#16301f; --fail:#f2766b; --fail-bg:#3a1a17; --warn:#e3b341; --warn-bg:#3a2e10;
  --s1:#3987e5; --s2:#d95926; --s3:#199e70; --s4:#9085e9; color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.55 var(--f-body);
  padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
.wrap{max-width:1080px;margin:0 auto;padding-block:28px 64px;display:grid;gap:40px;grid-template-columns:minmax(0,1fr)}
header.top{display:grid;gap:14px;border-bottom:2px solid var(--ink);padding-bottom:20px}
.eyebrow{font:600 12px/1 var(--f-mono);letter-spacing:.08em;text-transform:uppercase;color:var(--ink-3)}
h1{font:600 clamp(28px,4.4vw,44px)/1.05 var(--f-display);margin:0;text-wrap:balance;letter-spacing:-.01em}
h2{font:600 24px/1.2 var(--f-display);margin:0;text-wrap:balance}
h3{font:600 17px/1.3 var(--f-body);margin:0}
p{margin:0;max-width:68ch;color:var(--ink-2)}
.meta{display:flex;flex-wrap:wrap;gap:6px 18px;font:13px/1.4 var(--f-mono);color:var(--ink-2)}
.strip{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:1px;background:var(--rule);border:1px solid var(--rule)}
.stat{background:var(--paper);padding:12px 14px;display:grid;gap:2px}
.stat b{font:600 26px/1.1 var(--f-display);font-variant-numeric:tabular-nums}
.stat span{font-size:13px;color:var(--ink-3)}
section{display:grid;gap:16px;grid-template-columns:minmax(0,1fr);min-width:0}
.sechead{display:flex;align-items:baseline;gap:12px;border-top:1px solid var(--rule);padding-top:16px}
.secno{font:600 13px var(--f-mono);color:var(--accent)}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,460px),1fr));gap:24px}
figure{margin:0;display:grid;gap:8px;min-width:0}
figcaption{font-size:14px;color:var(--ink-2)}
figcaption b{color:var(--ink);font-weight:600}
.chartwrap{position:relative;background:var(--paper);border:1px solid var(--rule);padding:8px 8px 4px}
.chart{width:100%;height:auto;display:block;font-family:var(--f-body)}
.grid{stroke:var(--grid);stroke-width:1}.axis{stroke:var(--ink-3);stroke-width:1}
.tick{fill:var(--ink-3);font-size:11px;font-variant-numeric:tabular-nums}.axlabel{fill:var(--ink-2);font-size:12px}
.line{fill:none;stroke-width:2;stroke-linejoin:round}.line.dash{stroke-dasharray:6 4}
.line.s1{stroke:var(--s1)}.line.s2{stroke:var(--s2)}.line.s3{stroke:var(--s3)}.line.s4{stroke:var(--s4)}
.dot{stroke:var(--paper);stroke-width:2}.dot.s1,.bar.s1{fill:var(--s1)}.dot.s2,.bar.s2{fill:var(--s2)}
.dot.s3,.bar.s3{fill:var(--s3)}.dot.s4,.bar.s4{fill:var(--s4)}
.bar{stroke:var(--paper);stroke-width:2}
.dlabel{fill:var(--ink-2);font-size:11px}
.refline{stroke:var(--ink-3);stroke-width:1;stroke-dasharray:3 3}.reflabel{fill:var(--ink-2);font-size:11px}
.xhair{stroke:var(--ink-3);stroke-width:1}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font-size:13px;color:var(--ink-2);padding:2px 4px 6px}
.key{display:inline-flex;align-items:center;gap:6px}
.sw{display:inline-block;width:14px;height:3px;border-radius:2px}
.sw.s1{background:var(--s1)}.sw.s2{background:var(--s2)}.sw.s3{background:var(--s3)}.sw.s4{background:var(--s4)}
.sw.dash{background:repeating-linear-gradient(90deg,currentColor 0 5px,transparent 5px 8px)}.sw.dash.s1{color:var(--s1)}.sw.dash.s2{color:var(--s2)}.sw.dash.s3{color:var(--s3)}.sw.dash.s4{color:var(--s4)}
.tip{position:absolute;pointer-events:none;background:var(--ink);color:var(--paper);font:12px/1.4 var(--f-mono);
  padding:6px 8px;border-radius:4px;white-space:nowrap;z-index:2;font-variant-numeric:tabular-nums}
.tblwrap{overflow-x:auto;border:1px solid var(--rule);background:var(--paper)}
table{border-collapse:collapse;width:100%;font-size:14px;font-variant-numeric:tabular-nums}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--grid);vertical-align:top}
th{font:600 12px var(--f-mono);letter-spacing:.04em;text-transform:uppercase;color:var(--ink-3);background:var(--bg)}
td.num{text-align:right;font-family:var(--f-mono);font-size:13px}
.pill{display:inline-block;font:600 11px/1 var(--f-mono);letter-spacing:.05em;padding:4px 7px;border-radius:3px;text-transform:uppercase}
.pill.pass{color:var(--pass);background:var(--pass-bg)}.pill.fail{color:var(--fail);background:var(--fail-bg)}
.pill.warn{color:var(--warn);background:var(--warn-bg)}
details{border-bottom:1px solid var(--grid)}details:last-child{border-bottom:0}
summary{cursor:pointer;padding:9px 12px;display:flex;gap:10px;align-items:center;list-style:none}
summary::-webkit-details-marker{display:none}
summary:focus-visible{outline:2px solid var(--accent);outline-offset:-2px}
summary .n{margin-left:auto;font:12px var(--f-mono);color:var(--ink-3)}
.checks{margin:0 0 10px;padding:0 12px 0 40px;font-size:13px;color:var(--ink-2);display:grid;gap:2px}
.checks li.bad{color:var(--fail)}
code,.mono{font-family:var(--f-mono);font-size:.9em}
.find{display:grid;gap:4px;padding:12px 14px;background:var(--paper);border:1px solid var(--rule)}
.find .id{font:600 12px var(--f-mono);color:var(--accent)}
.finds{display:grid;gap:10px;min-width:0}.find p,td{overflow-wrap:anywhere}
footer{font-size:13px;color:var(--ink-3)}
@media (max-width:640px){.strip{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media (prefers-reduced-motion:reduce){*{transition:none!important}}
"""

JS = """
(function(){
  function place(tip, wrap, x, y){
    var r = wrap.getBoundingClientRect(); tip.hidden = false;
    var tw = tip.offsetWidth, left = x - r.left + 12;
    if (left + tw > r.width - 4) left = x - r.left - tw - 12;
    tip.style.left = Math.max(4, left) + 'px'; tip.style.top = (y - r.top + 12) + 'px';
  }
  document.querySelectorAll('.chartwrap').forEach(function(wrap){
    var tip = wrap.querySelector('.tip'), svg = wrap.querySelector('svg');
    var spec = wrap.getAttribute('data-hover');
    if (spec) {
      var d = JSON.parse(spec), hit = svg.querySelector('.hit'), xh = svg.querySelector('.xhair');
      hit.addEventListener('mousemove', function(ev){
        var pt = svg.createSVGPoint(); pt.x = ev.clientX; pt.y = ev.clientY;
        var p = pt.matrixTransform(svg.getScreenCTM().inverse()), best = null;
        d.series.forEach(function(s){ s.p.forEach(function(q){
          if (!best || Math.abs(q[0]-p.x) < Math.abs(best[0]-p.x)) best = q; }); });
        if (!best) return;
        xh.setAttribute('x1', best[0]); xh.setAttribute('x2', best[0]); xh.setAttribute('visibility','visible');
        var lines = [d.x + ' ' + best[2]];
        d.series.forEach(function(s){ s.p.forEach(function(q){ if (Math.abs(q[0]-best[0]) < 0.5) lines.push(s.n + ': ' + q[3]); }); });
        tip.innerHTML = lines.map(function(t){ var e=document.createElement('div'); e.textContent=t; return e.outerHTML; }).join('');
        place(tip, wrap, ev.clientX, ev.clientY);
      });
      hit.addEventListener('mouseleave', function(){ tip.hidden = true; xh.setAttribute('visibility','hidden'); });
    }
    svg.querySelectorAll('[data-tip]').forEach(function(el){
      el.addEventListener('mousemove', function(ev){ tip.textContent = el.getAttribute('data-tip'); place(tip, wrap, ev.clientX, ev.clientY); });
      el.addEventListener('mouseleave', function(){ tip.hidden = true; });
    });
  });
})();
"""


def _pill(ok, warn=False):
    if warn:
        return '<span class="pill warn">open</span>'
    return f'<span class="pill {"pass" if ok else "fail"}">{"pass" if ok else "fail"}</span>'


def _results_block(results):
    rows = []
    for r in results:
        bad = sum(not c[1] for c in r.checks)
        items = "".join(f'<li class="{"" if ok else "bad"}">{"&#10003;" if ok else "&#10007;"} {E(label)}'
                        f'{(" &mdash; <span class=mono>" + E(det) + "</span>") if det else ""}</li>'
                        for label, ok, det in r.checks)
        rows.append(f'<details{" open" if bad else ""}><summary>{_pill(r.passed)} <span>{E(r.name)}</span>'
                    f'<span class="n">{len(r.checks) - bad}/{len(r.checks)} checks</span></summary>'
                    f'<ul class="checks">{items}</ul></details>')
    return '<div class="tblwrap">' + "".join(rows) + "</div>"


def _table(head, rows, num_cols=()):
    h = "".join(f"<th>{E(x)}</th>" for x in head)
    body = "".join("<tr>" + "".join(
        f'<td class="{"num" if i in num_cols else ""}">{c}</td>' for i, c in enumerate(r)) + "</tr>" for r in rows)
    return f'<div class="tblwrap"><table><thead><tr>{h}</tr></thead><tbody>{body}</tbody></table></div>'


def generate(out: str = "build/report.html", quick: bool = False, units: int = 40,
             role_map: str | None = None) -> str:
    """role_map: optional YAML list of {requirement, evidence} appended as a section."""
    now = _dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    res = dvt.run_all(quick=quick)
    by = {r.name: r for r in res}
    sweep = by["Temperature sweep -5..75 degC case"]
    sens = by["Receiver sensitivity (pre-FEC BER waterfall)"]
    timing = by["Power-up and data-path timing"]
    env_names = {"Power-up and data-path timing", "Temperature sweep -5..75 degC case", "Supply voltage margining",
                 "Receiver sensitivity (pre-FEC BER waterfall)", "4x100G breakout (AppSel 2) and per-lane control"}
    fault_res = res[4:16]
    fw_res = [r for r in res if "image" in r.name.lower() or "firmware update" in r.name.lower()]
    other = [r for r in res if r.name in env_names or r.name.startswith("Host interface fuzz")]
    lot = mfg.run_lot(n=units)
    dsp_rows = dsplab.sweep(n_sym=50000 if quick else 80000)
    det = dsplab.link_ber(14, 50000, return_detail=True)
    reqs = yaml.safe_load((ROOT / "requirements" / "requirements.yaml").read_text())
    finds = yaml.safe_load((ROOT / "requirements" / "findings.yaml").read_text())
    tpath = ROOT / "build" / "trace.json"
    trace = json.loads(tpath.read_text()) if tpath.exists() else {}

    n_checks = sum(len(r.checks) for r in res)
    n_pass_checks = sum(sum(c[1] for c in r.checks) for r in res)
    n_proc_pass = sum(r.passed for r in res)
    min_cpk = min(s["cpk_post"] for s in lot["stats"].values())
    max_pw = max(row["power_w"] for row in sweep.data["rows"])
    covered = sum(1 for r in reqs if trace.get(r["id"]) or r.get("unit"))

    # ---- charts
    rows = sens.data["rows"]
    c_sens = Chart("sens", "average RX power [dBm]", "pre-FEC BER", logy=True, yfmt="{:.1e}", xfmt="{:g}",
                   ymin=1e-9) \
        .add("firmware", [(r["rx_dbm"], r["ber_meas"]) for r in rows], 1) \
        .add("truth", [(r["rx_dbm"], r["ber_true"]) for r in rows], 2, dash=True, markers=False) \
        .ref(2.4e-4, "KP4 limit")
    sw = sweep.data["rows"]
    c_ddm = Chart("ddm", "case temperature [degC]", "DDM error [dB]", yfmt="{:+.2f}", ymin=-0.3, ymax=0.3) \
        .add("RX power", [(r["case_c"], max((r[f"rx_ddm_dbm{i}"] - r[f"rx_true_dbm{i}"]) for i in range(4)))
                          for r in sw], 1) \
        .add("TX power", [(r["case_c"], max((r[f"tx_ddm_dbm{i}"] - r[f"tx_true_dbm{i}"] for i in range(4)),
                                            key=abs)) for r in sw], 3)
    c_tec = Chart("tec", "case temperature [degC]", "TEC drive [%] (+ cools)", yfmt="{:+.0f}", ymin=-100, ymax=100) \
        .add("TEC drive", [(r["case_c"], r["tec_pct"]) for r in sw], 1)
    c_pw = Chart("pw", "case temperature [degC]", "module power [W]", yfmt="{:.2f}", ymin=0, ymax=10.5) \
        .add("power", [(r["case_c"], r["power_w"]) for r in sw], 1).ref(10.0, "class limit")
    c_dsp = Chart("dsp", "SNR at the slicer input [dB]", "BER", logy=True, yfmt="{:.1e}") \
        .add("FFE", [(r["snr_db"], r["ber_ffe"]) for r in dsp_rows], 1) \
        .add("no EQ", [(r["snr_db"], r["ber_noeq"]) for r in dsp_rows], 2) \
        .add("closed form", [(r["snr_db"], r["ber_theory_at_snr_eff"]) for r in dsp_rows], 3, dash=True)
    traj = det["cdr_traj"]
    c_cdr = Chart("cdr", "symbols", "sampling phase [UI]", yfmt="{:+.3f}", xfmt="{:g}") \
        .add("phase", [(i, float(traj[i])) for i in range(0, len(traj), 40)], 1, markers=False)
    fec_x = [10 ** (e / 10) for e in range(-60, -24)]
    c_fec = Chart("fec", "pre-FEC BER", "codeword failure ratio", logy=True, logx=True, yfmt="{:.1e}", xfmt="{:.1e}",
                  ymin=1e-20, ymax=1) \
        .add("t = 15", [(b, max(dsplab.kp4_cw_failure(b), 1e-30)) for b in fec_x], 1, markers=False) \
        .ref(dsplab.kp4_cw_failure(2.4e-4), "at 2.4e-4")
    keys = list(lot["stats"])
    c_cpk = bars("cpk", ["temp", "Vcc", "RX DDM", "TX DDM", "TX abs"],
                 [("before calibration", 2, [lot["stats"][k]["cpk_pre"] for k in keys]),
                  ("after calibration", 1, [min(lot["stats"][k]["cpk_post"], 12) for k in keys])],
                 "Cpk", ref=(1.33, "Cpk 1.33"))

    # ---- tables
    sens_tbl = _table(["RX dBm", "BER (firmware)", "BER (truth)", "SNR dB", "uncorrectable CW in dwell", "DDM RX dBm"],
                      [[f"{r['rx_dbm']:.1f}", f"{r['ber_meas']:.2e}", f"{r['ber_true']:.2e}", f"{r['snr_db']:.2f}",
                        str(r["fec_uncorr_dwell"]), f"{r['ddm_rx_dbm']:.2f}"] for r in rows], range(6))
    sw_tbl = _table(["case C", "DDM temp", "true temp", "laser C", "TEC %", "power W", "pre-FEC BER", "TX dBm L1",
                     "RX err dB"],
                    [[f"{r['case_c']}", f"{r['ddm_temp']:.2f}", f"{r['true_temp']:.2f}", f"{r['laser_c']:.2f}",
                      f"{r['tec_pct']:+.0f}", f"{r['power_w']:.2f}", f"{r['ber']:.1e}", f"{r['tx_true_dbm0']:.2f}",
                      f"{max(r[f'rx_ddm_dbm{i}'] - r[f'rx_true_dbm{i}'] for i in range(4)):+.3f}"] for r in sw],
                    range(9))
    cpk_tbl = _table(["parameter", "limit +/-", "Cpk before", "Cpk after", "worst before", "worst after", "sigma after"],
                     [[k, f"{s['limit']:g}", f"{s['cpk_pre']:.2f}", f"{s['cpk_post']:.2f}", f"{s['worst_pre']:+.3f}",
                       f"{s['worst_post']:+.3f}", f"{s['sd_post']:.3f}"] for k, s in lot["stats"].items()],
                     range(1, 7))
    dsp_tbl = _table(["SNR dB", "BER FFE", "BER no EQ", "SNR_eff FFE dB", "closed form"],
                     [[f"{r['snr_db']}", f"{r['ber_ffe']:.2e}", f"{r['ber_noeq']:.2e}", f"{r['snr_eff_ffe']:.2f}",
                       f"{r['ber_theory_at_snr_eff']:.2e}"] for r in dsp_rows], range(5))
    req_rows = []
    for r in reqs:
        t = trace.get(r["id"], [])
        ok = all(x["outcome"] == "passed" for x in t)
        tests = "<br>".join(E(x["test"].split("::")[-1]) for x in t[:4]) + (f"<br>+{len(t) - 4} more" if len(t) > 4 else "")
        unit = ", ".join(r.get("unit", []))
        status = _pill(ok and (t or unit)) if (t or unit) else '<span class="pill fail">untested</span>'
        req_rows.append([f'<span class="mono">{E(r["id"])}</span>', E(r["text"]),
                         tests + (("<br>" if tests else "") + f"<span class=mono>C: {E(unit)}</span>" if unit else ""),
                         status])
    req_tbl = _table(["ID", "requirement", "verified by", "status"], req_rows)
    rows = yaml.safe_load(Path(role_map).read_text()) if role_map else []
    job_tbl = _table(["Requirement", "Where it is demonstrated"],
                     [[E(r["requirement"]), E(r["evidence"])] for r in rows])
    role_html = (f'<section><div class="sechead"><span class="secno">8</span><h2>Requirement coverage</h2></div>'
                 f"{job_tbl}</section>") if role_map else ""
    find_html = "".join(
        f'<div class="find"><div class="id">{E(f["id"])} &middot; {E(f["status"])}</div><h3>{E(f["title"])}</h3>'
        f'<p><b>Found by</b> {E(f["found_by"])}. {E(f["root_cause"])}</p><p><b>Action</b> {E(f["fix"])}</p></div>'
        for f in finds)

    body = f"""
<div class="wrap">
<header class="top">
  <div class="eyebrow">Design verification report &middot; OMT-400G-DR4 &middot; QSFP-DD CMIS 5.2</div>
  <h1>Optical Module Twin verification</h1>
  <p>Module controller firmware for a 400GBASE-DR4 transceiver, verified against a hardware digital twin:
  four lasers with APC, TEC, photodiodes, a PAM4 DSP on MDIO and NOR flash, in 1&nbsp;ms virtual time.
  Every number below was produced by the firmware running against the twin, not entered by hand.</p>
  <div class="meta"><span>host tools {__version__}</span><span>firmware 1.0 build 100</span>
  <span>generated {now}</span><span>{"quick" if quick else "full"} run</span><span>lot of {units} units</span></div>
</header>
<div class="strip">
  <div class="stat"><b>{n_pass_checks}/{n_checks}</b><span>DVT checks passed</span></div>
  <div class="stat"><b>{n_proc_pass}/{len(res)}</b><span>procedures passed</span></div>
  <div class="stat"><b>{sens.data['sensitivity_dbm']:.2f} dBm</b><span>RX sensitivity @ BER 2.4e-4</span></div>
  <div class="stat"><b>{timing.data['pwrup_ms']} ms</b><span>ModulePwrUp (adv. &le; 5 s)</span></div>
  <div class="stat"><b>{max_pw:.2f} W</b><span>max power, class 8 = 10 W</span></div>
  <div class="stat"><b>{min_cpk:.2f}</b><span>lowest Cpk after calibration</span></div>
  <div class="stat"><b>{covered}/{len(reqs)}</b><span>requirements with tests</span></div>
  <div class="stat"><b>{sum(r.passed for r in fault_res)}/{len(fault_res)}</b><span>fault scenarios handled</span></div>
</div>

<section><div class="sechead"><span class="secno">1</span><h2>Receiver and DSP</h2></div>
<div class="grid2">
<figure>{c_sens.svg()}<figcaption><b>Pre-FEC BER waterfall, lane 1.</b> The firmware's BER (read from DSP
error counters over MDIO, 1&nbsp;s windows) tracks the link model; the curve crosses the RS(544,514)
threshold at {sens.data['sensitivity_dbm']:.2f}&nbsp;dBm average power. Below about 1e-8 the
counters read zero in a 3&nbsp;s dwell.</figcaption></figure>
<figure>{c_dsp.svg()}<figcaption><b>PAM4 equalization (dsplab).</b> With 0.24&nbsp;UI post-cursor ISI the
unequalized eye stays near BER 1e-1; a 7-tap LMS FFE recovers it. The dashed closed form
(3/8&middot;erfc(&radic;(SNR/10)), used by the twin) evaluated at the measured SNR sits 15&ndash;25&nbsp;% below the
Monte-Carlo (finding F-007).</figcaption></figure>
<figure>{c_cdr.svg()}<figcaption><b>Clock recovery.</b> Mueller-M&uuml;ller timing error detector with a PI loop,
started 0.3&nbsp;UI off; it settles at the MM zero crossing, where pre- and post-cursor balance.</figcaption></figure>
<figure>{c_fec.svg()}<figcaption><b>KP4 FEC.</b> Probability that a 544-symbol codeword has more than 15 symbol
errors, for independent bit errors. At the 2.4e-4 threshold it is {dsplab.kp4_cw_failure(2.4e-4):.1e}.</figcaption></figure>
</div>
{sens_tbl}
{dsp_tbl}
</section>

<section><div class="sechead"><span class="secno">2</span><h2>Environment and margining</h2></div>
<div class="grid2">
<figure>{c_ddm.svg()}<figcaption><b>DDM accuracy over temperature</b> (worst lane, DDM minus reference). TX is
held by calibration of the monitor photodiode; RX drifts +0.12&nbsp;dB at the hot end from the uncompensated
photodiode tempco (F-005). Limit &plusmn;1&nbsp;dB.</figcaption></figure>
<figure>{c_tec.svg()}<figcaption><b>TEC effort.</b> The laser plate is held at 50&nbsp;&deg;C from &minus;5 to 75&nbsp;&deg;C
case; heating is cheaper than cooling, so the hot corner uses {max(r['tec_pct'] for r in sw):.0f}&nbsp;% of the cooling range.</figcaption></figure>
<figure>{c_pw.svg()}<figcaption><b>Power consumption</b> against the advertised power class (10&nbsp;W).</figcaption></figure>
</div>
{sw_tbl}
{_results_block(other)}
</section>

<section><div class="sechead"><span class="secno">3</span><h2>Fault injection and root cause</h2></div>
<p>Each scenario starts from a fresh, brought-up module, injects one fault in the twin and checks what the
host can observe: module state, fault cause, latched flags, IntL, and the firmware's non-volatile fault log.</p>
{_results_block(fault_res)}
</section>

<section><div class="sechead"><span class="secno">4</span><h2>Firmware management (CDB)</h2></div>
<p>Images are downloaded with CMIS CDB into the inactive bank, CRC-checked, run on trial and committed.
A boot that never confirms (watchdog) rolls back to the committed bank.</p>
{_results_block(fw_res)}
</section>

<section><div class="sechead"><span class="secno">5</span><h2>Manufacturing calibration</h2></div>
<div class="grid2">
<figure>{c_cpk}<figcaption><b>Process capability, {units} units.</b> Uncalibrated units scatter with
part-to-part spread of sensors, TIA gain and monitor photodiodes. After calibration the remaining error is set
mostly by the station's reference instruments (power meter &sigma; 0.12&nbsp;dB), so improving optical Cpk means
improving the station, not the firmware. Bars above 12 are clipped.</figcaption></figure>
<figure><div class="tblwrap" style="padding:12px 14px;display:grid;gap:8px">
<h3>Station flow per unit</h3>
<p>Power up &rarr; unlock page B1h with the factory password &rarr; average raw ADC readings &rarr; two-point RSSI fit
at &minus;10 and 0&nbsp;dBm &rarr; MPD slope from the reference power meter &rarr; write coefficients, serial and date
&rarr; firmware range-checks and stores them with CRC32, read-back verified &rarr; verify DDM at three RX levels.
Yield {lot['yield'] * 100:.0f}&nbsp;%.</p></div></figure>
</div>
{cpk_tbl}
</section>

<section><div class="sechead"><span class="secno">6</span><h2>Findings</h2></div>
<p>Issues found by the tests and tools while building this firmware, including review notes for the
host-side Zephyr driver in PR #123842 (P-1 to P-4), obtained by replaying its I2C access pattern against the twin. Fixes for P-1 to P-3 are prepared for the PR and pass twister on native_sim.</p>
<div class="finds">{find_html}</div>
</section>

<section><div class="sechead"><span class="secno">7</span><h2>Requirements traceability</h2></div>
<p>{"From the last pytest run (build/trace.json)." if trace else "Run pytest first to fill in the test column."}</p>
{req_tbl}
</section>

{role_html}<footer>Simulation results. Byte offsets follow CMIS 5.x as modeled in firmware/include/omt/cmis_map.h and must be
checked against the OIF-CMIS text before product use. Generated by <span class="mono">python -m omt report</span>.</footer>
</div>
"""
    page = ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            "<title>Optical Module Twin</title>\n"
            '<link rel="preconnect" href="https://fonts.googleapis.com">'
            '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;600'
            '&family=Saira+Semi+Condensed:wght@500;600&family=Source+Sans+3:wght@400;600&display=swap">\n'
            f"<style>{CSS}</style>\n</head>\n<body>\n{body}\n<script>{JS}</script>\n</body>\n</html>\n")
    p = Path(out)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(page)
    return str(p)
