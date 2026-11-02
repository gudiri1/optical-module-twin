# SPDX-License-Identifier: Apache-2.0
"""CMIS lint: consistency checks on a module memory image.

Usable for incoming inspection of vendor samples, as a manufacturing
final-test step, and on dumps captured from deployed modules
(``ethtool -m <if> hex on``). Each rule has an ID so findings can be
tracked in bug reports.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

from . import cmis

ERROR, WARN, INFO = "error", "warning", "info"


@dataclass
class Finding:
    rule: str
    severity: str
    message: str

    def __str__(self):
        return f"{self.severity.upper():7s} {self.rule} {self.message}"


POWER_CLASS_MAX_W = {1: 1.5, 2: 2.0, 3: 2.5, 4: 3.5, 5: 4.0, 6: 4.5, 7: 5.0}


def _ascii_ok(b: bytes) -> bool:
    s = b.rstrip(b" ")
    return all(32 <= c < 127 for c in s)


def lint(img: cmis.CmisImage) -> list[Finding]:
    f: list[Finding] = []
    add = lambda r, s, m: f.append(Finding(r, s, m))  # noqa: E731

    lo = img.lower
    if lo:
        if lo[0] not in cmis.IDENTIFIERS:
            add("L001", ERROR, f"identifier 0x{lo[0]:02X} is not a CMIS form factor")
        if lo[1] >> 4 not in (3, 4, 5):
            add("L002", WARN, f"CMIS revision {lo[1] >> 4}.{lo[1] & 0xF} unexpected")
        state = (lo[3] >> 1) & 7
        if state not in cmis.MODULE_STATES:
            add("L013", ERROR, f"reserved module state {state}")
        elif state == 5:
            add("L013", ERROR, f"module in ModuleFault (cause: {cmis.FAULT_CAUSES.get(lo[41], lo[41])})")
        d = cmis.decode_lower(lo)
        if not d["applications"]:
            add("L010", ERROR, "no application advertised")
        n_apps = 0
        for i in range(8):
            if lo[86 + 4 * i] == 0xFF:
                break
            n_apps += 1
        for a in d["applications"]:
            hl, ml, asg = a["host_lanes"], a["media_lanes"], a["host_lane_assign"]
            if not (1 <= hl <= 8) or not (1 <= ml <= 8):
                add("L010", ERROR, f"AppSel {a['appsel']}: invalid lane counts {hl}/{ml}")
                continue
            if asg == 0:
                add("L010", ERROR, f"AppSel {a['appsel']}: empty host lane assignment")
            for lane in range(8):
                if asg & (1 << lane) and lane + hl > 8:
                    add("L010", ERROR, f"AppSel {a['appsel']}: data path at lane {lane + 1} "
                                       f"with {hl} lanes exceeds 8 host lanes")
            if d["media_type_code"] == 2 and a["media_if_code"] not in cmis.SMF_MEDIA_IF:
                add("L011", INFO, f"AppSel {a['appsel']}: SMF media code 0x{a['media_if_code']:02X} "
                                  "not in this tool's table")
        if lo[8] or lo[9] or lo[10] or lo[11]:
            names = []
            fl = d["flags"]
            for grp in ("temp", "vcc", "aux1", "aux2"):
                names += [f"{grp}.{k}" for k, v in fl[grp].items() if v]
            if names:
                add("L016", WARN, "latched module flags: " + ", ".join(names))
            if fl["state_changed"]:
                add("L016", INFO, "ModuleStateChanged latched (normal after a state transition)")

    p0 = img.page(0x00)
    if p0 is not None:
        d0 = cmis.decode_page00(p0)
        if not d0["checksum_ok"]:
            add("L003", ERROR, "Page 00h checksum (byte 222) mismatch")
        b = bytes(128) + p0
        for name, a, z in (("vendor name", 129, 145), ("part number", 148, 164),
                           ("serial number", 166, 182)):
            if not _ascii_ok(b[a:z]):
                add("L006", ERROR, f"{name} contains non-printable bytes")
            elif not b[a:z].strip(b" \x00"):
                add("L006", ERROR, f"{name} is empty")
        dc = b[182:190].decode("ascii", "replace")
        m = re.match(r"(\d\d)(\d\d)(\d\d)", dc)
        if not m or not (1 <= int(m.group(2)) <= 12 and 1 <= int(m.group(3)) <= 31):
            add("L007", WARN, f"date code '{dc.strip()}' is not YYMMDD")
        cls, maxp = d0["power_class"], d0["max_power_w"]
        if cls < 8 and maxp and maxp > POWER_CLASS_MAX_W[cls]:
            add("L012", ERROR, f"power class {cls} but max power {maxp} W")
        if cls == 8 and maxp <= 5.0:
            add("L012", ERROR, f"power class 8 requires max power > 5 W, got {maxp} W")
        if lo and b[128] != lo[0]:
            add("L001", WARN, "Page 00h identifier differs from lower memory byte 0")

    p1 = img.page(0x01)
    if p1 is not None:
        d1 = cmis.decode_page01(p1)
        if not d1["checksum_ok"]:
            add("L004", ERROR, "Page 01h checksum (byte 255) mismatch")
        if not (800 <= d1["wavelength_nm"] <= 1700):
            add("L020", WARN, f"nominal wavelength {d1['wavelength_nm']:.2f} nm implausible")
        for k in ("max_dp_init", "max_module_pwrup"):
            if d1[k] == "reserved":
                add("L017", ERROR, f"{k} uses a reserved duration code")

    p2 = img.page(0x02)
    if p2 is not None:
        d2 = cmis.decode_page02(p2)
        if not d2["checksum_ok"]:
            add("L005", ERROR, "Page 02h checksum (byte 255) mismatch")
        for mon, t in d2.items():
            if mon == "checksum_ok":
                continue
            if not (t["high_alarm"] >= t["high_warning"] > t["low_warning"] >= t["low_alarm"]):
                add("L008", ERROR, f"{mon} thresholds not ordered HA>=HW>LW>=LA: {t}")
        if lo:
            dl = cmis.decode_lower(lo)
            for mon, val in (("temp_c", dl["temperature_c"]), ("vcc_v", dl["vcc_v"])):
                t = d2[mon]
                if not (t["low_alarm"] <= val <= t["high_alarm"]):
                    add("L009", WARN, f"{mon} = {val:.3f} outside alarm thresholds")

    p11 = img.page(0x11)
    if p11 is not None:
        d11 = cmis.decode_page11(p11)
        for lane, s in enumerate(d11["dp_state"]):
            if s.startswith("reserved"):
                add("L014", ERROR, f"host lane {lane + 1}: {s} data path state")
        for lane, s in enumerate(d11["config_status"]):
            if s.startswith("Rejected"):
                add("L015", WARN, f"host lane {lane + 1}: last configuration {s}")
        if lo:
            n_apps = len(cmis.decode_lower(lo)["applications"])
            for lane, a in enumerate(d11["active_appsel"]):
                if a == 0 or a > n_apps:
                    add("L014", ERROR, f"host lane {lane + 1}: active AppSel {a} not advertised")
        active = all(s == "DPActivated" for s in d11["dp_state"])
        for ln in d11["lanes"]:
            if active and ln["tx_power_uw"] == 0:
                add("L019", WARN, f"media lane {ln['lane']}: data path active but TX power 0")
            if ln["flags"]:
                add("L016", WARN, f"media lane {ln['lane']} latched flags: {', '.join(ln['flags'])}")
        if p2 is not None:
            d2 = cmis.decode_page02(p2)
            for ln in d11["lanes"]:
                for key, thr in (("rx_power_uw", "rx_power_uw"),):
                    t = d2[thr]
                    if active and not (t["low_alarm"] <= ln[key] <= t["high_alarm"]):
                        add("L009", WARN, f"media lane {ln['lane']}: {key} {ln[key]:.1f} outside alarms")
    return f


def summary(findings: list[Finding]) -> dict:
    return {s: sum(1 for x in findings if x.severity == s) for s in (ERROR, WARN, INFO)}
