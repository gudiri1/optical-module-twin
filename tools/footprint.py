#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cortex-M4F footprint of the module controller firmware.

Cross-compiles firmware/src/*.c unchanged with arm-none-eabi-gcc (-Os,
hard-float FPv4-SP), links it with newlib-nano and a stub board (empty HAL
functions, a 1 ms main loop), and reports:

  * flash (text + rodata + data init) and RAM (data + bss) of the linked image
  * the share of each firmware module and of the C library
  * static stack frames from -fstack-usage (worst frames; no call-graph
    analysis, so the real worst case is a sum along the deepest call chain)

    python tools/footprint.py [--cpu cortex-m4] [--json build/footprint.json]

Needs the GNU Arm Embedded toolchain on PATH (or ARM_GCC=<path to gcc>).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STUB = r"""
#include "omt/fw.h"
/* Stub board: every HAL hook is an empty function so the linker keeps
 * exactly what the firmware itself needs. */
static volatile uint32_t ticks;
static uint32_t ms(void *c) { (void)c; return ticks; }
static uint8_t rc(void *c) { (void)c; return 0; }
static int spi(void *c, const uint8_t *t, uint8_t *r, size_t n) { (void)c; (void)t; (void)r; (void)n; return 0; }
static int mrd(void *c, uint8_t p, uint8_t d, uint16_t g, uint16_t *v) { (void)c; (void)p; (void)d; (void)g; *v = 0; return 0; }
static int mwr(void *c, uint8_t p, uint8_t d, uint16_t g, uint16_t v) { (void)c; (void)p; (void)d; (void)g; (void)v; return 0; }
static int adc(void *c, uint8_t ch, uint16_t *v) { (void)c; (void)ch; *v = 0; return 0; }
static int dac(void *c, uint8_t ch, uint16_t v) { (void)c; (void)ch; (void)v; return 0; }
static int gget(void *c, uint8_t p) { (void)c; (void)p; return 0; }
static void gset(void *c, uint8_t p, int l) { (void)c; (void)p; (void)l; }
static int nrd(void *c, uint32_t a, void *b, size_t n) { (void)c; (void)a; (void)b; (void)n; return 0; }
static int nwr(void *c, uint32_t a, const void *b, size_t n) { (void)c; (void)a; (void)b; (void)n; return 0; }
static int ner(void *c, uint32_t a, size_t n) { (void)c; (void)a; (void)n; return 0; }
static void kick(void *c) { (void)c; }
static void utx(void *c, const char *s, size_t n) { (void)c; (void)s; (void)n; }
static void srst(void *c, uint8_t r) { (void)c; (void)r; }
static void lk(void *c) { (void)c; }
static const struct omt_hal hal = { 0, ms, rc, spi, mrd, mwr, adc, dac, gget, gset, nrd, nwr, ner,
				    kick, utx, srst, lk, lk };
static struct omt_fw fw;

/* I2C target ISR entry points the board would call */
void i2c_isr_write(const uint8_t *b, size_t n) { omt_i2c_write(&fw, b, n); }
void i2c_isr_read(uint8_t *b, size_t n) { omt_i2c_read(&fw, b, n); }

int main(void)
{
	omt_fw_init(&fw, &hal);
	for (;;) {
		ticks++;
		omt_fw_tick(&fw);
	}
}
"""


def gcc() -> str:
    g = os.environ.get("ARM_GCC") or shutil.which("arm-none-eabi-gcc")
    if not g:
        sys.exit("arm-none-eabi-gcc not found (set ARM_GCC)")
    return g


def tool(g: str, name: str) -> str:
    return str(Path(g).with_name(Path(g).name.replace("gcc", name)))


def run(cmd: list[str], cwd: Path) -> str:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if p.returncode:
        sys.exit(f"{' '.join(cmd)}\n{p.stdout}{p.stderr}")
    return p.stdout


def measure(cpu: str = "cortex-m4") -> dict:
    g = gcc()
    flags = [f"-mcpu={cpu}", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16", "-Os", "-std=gnu11",
             "-ffunction-sections", "-fdata-sections", "-fstack-usage", "-Wall", "-Wextra", "-Wdouble-promotion",
             f"-I{ROOT / 'firmware' / 'include'}"]
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "stub.c").write_text(STUB)
        srcs = sorted((ROOT / "firmware" / "src").glob("*.c")) + [d / "stub.c"]
        objs, warnings = [], []
        for s in srcs:
            o = d / (s.stem + ".o")
            p = subprocess.run([g, *flags, "-c", str(s), "-o", str(o)], cwd=d, capture_output=True, text=True)
            if p.returncode:
                sys.exit(p.stderr)
            warnings += [ln for ln in p.stderr.splitlines() if "warning:" in ln]
            objs.append(o)
        elf = d / "fw.elf"
        run([g, *flags[:4], "--specs=nano.specs", "--specs=nosys.specs", "-u", "_printf_float",
             "-Wl,--gc-sections", f"-Wl,-Map={d / 'fw.map'}", *map(str, objs), "-lm", "-o", str(elf)], d)
        sec = {}
        for ln in run([tool(g, "size"), "-A", str(elf)], d).splitlines():
            parts = ln.split()
            if len(parts) >= 2 and parts[1].isdigit():
                sec[parts[0]] = int(parts[1])
        # per-module attribution from the linker map (input sections kept)
        per = defaultdict(lambda: {"flash": 0, "ram": 0})
        cur = None
        text = (d / "fw.map").read_text(errors="replace")
        text = text[text.find("Linker script and memory map"):]   # skip discarded sections
        for ln in text.splitlines():
            m = re.match(r"^ (\.\S+)\s*$", ln)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r"^ ?(\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+\.(?:o|a\(.+\.o\)|obj))\s*$", ln)
            if not m:
                continue
            name = m.group(1) or cur or ""
            size = int(m.group(3), 16)
            if not size or not name:
                continue
            src = Path(m.group(4).replace("\\", "/")).name
            mod = src[:-2] if src.endswith(".o") and (ROOT / "firmware" / "src" / (src[:-2] + ".c")).exists() \
                else ("board stub" if src == "stub.o" else "C library (newlib-nano, libm, libgcc)")
            ram = name.startswith((".bss", ".data", "COMMON"))
            flash = name.startswith((".text", ".rodata", ".data"))
            if ram:
                per[mod]["ram"] += size
            if flash:
                per[mod]["flash"] += size
        stack = []
        for su in d.glob("*.su"):
            for ln in su.read_text().splitlines():
                f, n, kind = ln.rsplit("\t", 2) if ln.count("\t") >= 2 else (ln, "0", "")
                stack.append({"function": f.split(":")[-1], "file": su.stem, "bytes": int(n), "kind": kind})
        stack.sort(key=lambda x: -x["bytes"])
        fw_sym = run([tool(g, "nm"), "-S", "--size-sort", str(elf)], d)
        fw_state = next((int(ln.split()[1], 16) for ln in fw_sym.splitlines() if ln.endswith(" fw")), None)
    flash = sec.get(".text", 0) + sec.get(".rodata", 0) + sec.get(".data", 0) + sec.get(".ARM.exidx", 0)
    ram = sec.get(".data", 0) + sec.get(".bss", 0)
    ver = run([g, "-dumpversion"], ROOT).strip()
    return {"cpu": cpu, "gcc": ver, "flash_bytes": flash, "ram_bytes": ram, "sections": sec,
            "fw_state_bytes": fw_state, "modules": dict(sorted(per.items(), key=lambda kv: -kv[1]["flash"])),
            "stack_top": stack[:8], "dynamic_frames": [s for s in stack if s["kind"] != "static"][:5],
            "warnings": warnings}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cpu", default="cortex-m4")
    ap.add_argument("--json", type=Path)
    a = ap.parse_args(argv)
    r = measure(a.cpu)
    print(f"{a.cpu}, arm-none-eabi-gcc {r['gcc']}, -Os, newlib-nano with float printf")
    print(f"flash {r['flash_bytes']:7d} B   ram {r['ram_bytes']:6d} B   "
          f"(firmware state struct omt_fw: {r['fw_state_bytes']} B)")
    print(f"\n{'module':40s} {'flash':>7s} {'ram':>6s}")
    for k, v in r["modules"].items():
        print(f"{k:40s} {v['flash']:7d} {v['ram']:6d}")
    print("\nlargest stack frames (static, bytes):")
    for s in r["stack_top"]:
        print(f"  {s['bytes']:5d}  {s['file']}.c {s['function']} ({s['kind']})")
    if r["warnings"]:
        print("\ncompiler warnings:")
        print("\n".join("  " + w for w in r["warnings"]))
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(r, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
