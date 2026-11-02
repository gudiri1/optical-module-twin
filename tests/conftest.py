# SPDX-License-Identifier: Apache-2.0
"""pytest setup: import path, markers and requirement traceability capture."""
import json
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host"))

_TRACE: dict[str, list[dict]] = {}


def pytest_configure(config):
    config.addinivalue_line("markers", "req(*ids): requirement(s) verified by this test")
    config.addinivalue_line("markers", "dvt: slower design-verification tests")
    config.addinivalue_line("markers", "mfg: manufacturing calibration tests")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    out = yield
    rep = out.get_result()
    if rep.when != "call" and not (rep.when == "setup" and rep.skipped):
        return
    for m in item.iter_markers("req"):
        for rid in m.args:
            _TRACE.setdefault(rid, []).append({"test": item.nodeid, "outcome": rep.outcome})


def pytest_sessionfinish(session, exitstatus):
    out = Path(os.environ.get("OMT_TRACE_OUT", ROOT / "build" / "trace.json"))
    out.parent.mkdir(parents=True, exist_ok=True)
    prev = json.loads(out.read_text()) if out.exists() and os.environ.get("OMT_TRACE_APPEND") else {}
    for k, v in _TRACE.items():
        prev.setdefault(k, []).extend(v)
    out.write_text(json.dumps(prev, indent=1))
