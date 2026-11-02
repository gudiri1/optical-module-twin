#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Release: check versions agree, build the firmware image, write notes.

Produces dist/omt-fw-<ver>.bin (+ .sha256), dist/RELEASE_NOTES.md built from
CHANGELOG.md, open findings and the last test trace. Refuses to release with
open findings unless --allow-open is given.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))
from omt import image  # noqa: E402

import yaml  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=int, default=0)
    ap.add_argument("--allow-open", action="store_true")
    a = ap.parse_args()

    fw_h = (ROOT / "firmware/include/omt/fw.h").read_text()
    major = int(re.search(r"OMT_FW_VERSION_MAJOR (\d+)", fw_h).group(1))
    minor = int(re.search(r"OMT_FW_VERSION_MINOR (\d+)", fw_h).group(1))
    ver = f"{major}.{minor}"
    if f"## {ver}" not in (ROOT / "CHANGELOG.md").read_text():
        sys.exit(f"CHANGELOG.md has no section for {ver}")

    findings = yaml.safe_load((ROOT / "requirements/findings.yaml").read_text())
    open_f = [f for f in findings if f["status"] == "open"]
    if open_f and not a.allow_open:
        sys.exit("open findings block the release: " + ", ".join(f["id"] for f in open_f)
                 + " (use --allow-open for an engineering build)")

    trace_p = ROOT / "build/trace.json"
    trace = json.loads(trace_p.read_text()) if trace_p.exists() else {}
    failed = [t["test"] for v in trace.values() for t in v if t["outcome"] == "failed"]
    if not trace or failed:
        sys.exit("run pytest first; failing: " + ", ".join(failed[:5]))

    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    img = image.build(major, minor, a.build, 0, f"rel-{ver}")
    out = dist / f"omt-fw-{ver}.{a.build}.bin"
    out.write_bytes(img)
    sha = hashlib.sha256(img).hexdigest()
    (dist / (out.name + ".sha256")).write_text(f"{sha}  {out.name}\n")
    notes = [f"# OMT-400G-DR4 firmware {ver} build {a.build}", "",
             f"Image `{out.name}` ({len(img)} bytes), sha256 `{sha}`.", "",
             f"Requirements traced: {len(trace)}; all linked tests passed.", ""]
    if open_f:
        notes += ["Open findings (engineering build):"] + [f"* {f['id']} {f['title']}" for f in open_f] + [""]
    (dist / "RELEASE_NOTES.md").write_text("\n".join(notes))
    print(f"wrote {out} and RELEASE_NOTES.md")


if __name__ == "__main__":
    main()
