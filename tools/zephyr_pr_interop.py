#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Interop harness: decode the twin's memory with the Zephyr CMIS library
from zephyrproject-rtos/zephyr PR #123842 (host side, read-only use).

The PR is not vendored. Sources are taken from, in order:
  1. $OMT_ZEPHYR_DIR (a zephyr checkout that contains lib/cmis/cmis.c), or a
     checkout with the PR fetched as branch `pr123842`
  2. raw.githubusercontent.com (branch gudiri1/zephyr:sff/sff8472-module-mgmt)
The library is compiled for the host with two tiny shim headers replacing
<zephyr/sys/util.h> and <zephyr/sys/byteorder.h>.
"""
from __future__ import annotations

import ctypes as C
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

FILES = ("lib/cmis/cmis.c", "include/zephyr/cmis/cmis.h")
RAW = "https://raw.githubusercontent.com/gudiri1/zephyr/sff/sff8472-module-mgmt/"

SHIM_UTIL = """#pragma once
#include <stdint.h>
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
"""
SHIM_BYTEORDER = """#pragma once
#include <stdint.h>
static inline uint16_t sys_get_be16(const uint8_t src[2]) { return (uint16_t)((src[0] << 8) | src[1]); }
"""


def obtain(dest: Path) -> bool:
    dest.mkdir(parents=True, exist_ok=True)
    if all((dest / f).exists() for f in FILES):
        return True
    z = os.environ.get("OMT_ZEPHYR_DIR")
    if z:
        zp = Path(z)
        for f in FILES:
            src = zp / f
            out = dest / f
            out.parent.mkdir(parents=True, exist_ok=True)
            if src.exists():
                out.write_bytes(src.read_bytes())
            else:
                try:
                    blob = subprocess.run(["git", "-C", z, "show", f"pr123842:{f}"],
                                          check=True, capture_output=True).stdout
                    out.write_bytes(blob)
                except Exception:
                    pass
        if all((dest / f).exists() for f in FILES):
            return True
    try:
        for f in FILES:
            out = dest / f
            out.parent.mkdir(parents=True, exist_ok=True)
            with urllib.request.urlopen(RAW + f, timeout=15) as r:
                out.write_bytes(r.read())
        return True
    except Exception:
        return False


def build(dest: Path) -> Path | None:
    if not obtain(dest):
        return None
    shim = dest / "shim" / "zephyr" / "sys"
    shim.mkdir(parents=True, exist_ok=True)
    (shim / "util.h").write_text(SHIM_UTIL)
    (shim / "byteorder.h").write_text(SHIM_BYTEORDER)
    so = dest / "libzephyr_cmis.so"
    cmd = ["cc", "-std=c11", "-O2", "-shared", "-fPIC", "-Wall", "-Werror",
           "-I", str(dest / "shim"), "-I", str(dest / "include"),
           str(dest / "lib/cmis/cmis.c"), "-o", str(so)]
    subprocess.run(cmd, check=True)
    return so


class CmisId(C.Structure):
    _fields_ = [("identifier", C.c_uint8), ("cmis_major", C.c_uint8), ("cmis_minor", C.c_uint8),
                ("flat_memory", C.c_bool), ("module_state", C.c_uint8), ("media_type", C.c_uint8),
                ("power_class", C.c_uint8), ("max_power_mw", C.c_uint16),
                ("vendor_oui", C.c_uint8 * 3), ("vendor_name", C.c_char * 17),
                ("vendor_pn", C.c_char * 17), ("vendor_rev", C.c_char * 3),
                ("vendor_sn", C.c_char * 17), ("date_code", C.c_char * 9)]


class CmisMon(C.Structure):
    _fields_ = [("temp_mdegc", C.c_int32), ("vcc_uv", C.c_uint32), ("flags", C.c_uint8)]


class ZephyrCmis:
    def __init__(self, so: Path):
        L = C.CDLL(str(so))
        L.cmis_parse_id.argtypes = [C.c_char_p, C.POINTER(CmisId)]
        L.cmis_parse_monitors.argtypes = [C.c_char_p, C.POINTER(CmisMon)]
        L.cmis_module_state_name.restype = C.c_char_p
        L.cmis_identifier_name.restype = C.c_char_p
        self.L = L

    def parse(self, page00_flat: bytes):
        i, m = CmisId(), CmisMon()
        assert self.L.cmis_parse_id(page00_flat, C.byref(i)) == 0
        assert self.L.cmis_parse_monitors(page00_flat, C.byref(m)) == 0
        return i, m


def compare(zc: ZephyrCmis, lower: bytes, page00: bytes) -> list[str]:
    """Return a list of discrepancies between the two decoders."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
    from omt import cmis

    zi, zm = zc.parse(lower + page00)
    ol, o0 = cmis.decode_lower(lower), cmis.decode_page00(page00)
    diffs = []

    def chk(name, a, b):
        if a != b:
            diffs.append(f"{name}: zephyr={a!r} omt={b!r}")

    chk("identifier", zi.identifier, ol["identifier_code"])
    chk("cmis_rev", f"{zi.cmis_major}.{zi.cmis_minor}", ol["cmis_rev"])
    chk("flat_memory", bool(zi.flat_memory), ol["flat_memory"])
    chk("module_state", zi.module_state, ol["module_state_code"])
    chk("media_type", zi.media_type, ol["media_type_code"])
    chk("power_class(encoded+1)", zi.power_class + 1, o0["power_class"])
    chk("max_power_w", zi.max_power_mw / 1000, o0["max_power_w"])
    chk("vendor_name", zi.vendor_name.decode(), o0["vendor_name"])
    chk("vendor_pn", zi.vendor_pn.decode(), o0["vendor_pn"])
    chk("vendor_rev", zi.vendor_rev.decode(), o0["vendor_rev"])
    chk("vendor_sn", zi.vendor_sn.decode(), o0["vendor_sn"])
    chk("date_code", zi.date_code.decode(), o0["date_code"])
    chk("vendor_oui", bytes(zi.vendor_oui).hex(":"), o0["vendor_oui"])
    if abs(zm.temp_mdegc / 1000 - ol["temperature_c"]) > 0.004:
        diffs.append(f"temperature: zephyr={zm.temp_mdegc / 1000} omt={ol['temperature_c']}")
    chk("vcc_uV", zm.vcc_uv, round(ol["vcc_v"] * 1e6))
    fl = ol["flags"]
    mine = sum(1 << i for i, k in enumerate(cmis.FLAG_NAMES4) if fl["temp"][k]) | \
        sum(1 << (i + 4) for i, k in enumerate(cmis.FLAG_NAMES4) if fl["vcc"][k])
    chk("flags(byte9)", zm.flags, mine)
    return diffs


if __name__ == "__main__":
    dest = Path(sys.argv[1] if len(sys.argv) > 1 else "build/third_party/zephyr_pr123842")
    so = build(dest)
    print(so or "sources unavailable")
