# SPDX-License-Identifier: Apache-2.0
"""Interop: the twin (module side) vs. the Zephyr CMIS decoder of PR #123842
(host side).

Opt-in, because it downloads and compiles third-party code:
    OMT_INTEROP_PR=1 [OMT_ZEPHYR_DIR=/path/to/zephyr] pytest tests/interop
Skipped when not enabled or when the PR sources cannot be obtained."""
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import zephyr_pr_interop as zi  # noqa: E402

from omt.host import CmisHost  # noqa: E402
from omt.sim import SimModule  # noqa: E402


@pytest.fixture(scope="module")
def zcmis():
    if os.environ.get("OMT_INTEROP_PR") != "1":
        pytest.skip("opt-in: set OMT_INTEROP_PR=1 to build the Zephyr PR decoder")
    so = zi.build(ROOT / "build" / "third_party" / "zephyr_pr123842")
    if so is None:
        pytest.skip("Zephyr PR #123842 sources not available (offline?)")
    return zi.ZephyrCmis(so)


def _states():
    """Module images in several states, including active alarm flags."""
    m = SimModule(seed=77)
    h = CmisHost(m)
    yield "LowPwr", h.lower(), h.upper(0)
    h.bring_up()
    m.run(2000)
    h.read_flags()
    yield "Ready", h.lower(), h.upper(0)
    m.set("case_temp", 76)
    m.set("vcc", 3.10)
    m.run(150000)
    yield "Ready+temp/vcc warnings", h.lower(), h.upper(0)
    m.set("case_temp", 95)
    m.run(150000)
    yield "Fault", h.lower(), h.upper(0)
    m.set("case_temp", -20)
    m.run(150000)
    yield "Fault, negative temperature", h.lower(), h.upper(0)


@pytest.mark.req("REQ-INT-001")
def test_zephyr_decoder_agrees(zcmis):
    seen_flags = False
    for label, lower, p00 in _states():
        diffs = zi.compare(zcmis, lower, p00)
        assert diffs == [], f"{label}: {diffs}"
        seen_flags |= lower[9] != 0
    assert seen_flags, "no alarm flags exercised"
