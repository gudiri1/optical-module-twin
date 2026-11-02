# SPDX-License-Identifier: Apache-2.0
"""Fault injection matrix (root-cause scenarios)."""
import pytest
from helpers import assert_result

from omt import dvt

REQ = {
    "Laser end-of-life": ("REQ-TX-002", "REQ-FLT-002"),
    "Laser driver": ("REQ-TX-002",),
    "TEC open circuit at 25": ("REQ-TEC-001",),
    "TEC open circuit at 70": ("REQ-TEC-001", "REQ-FLT-002"),
    "DSP firmware hang": ("REQ-DSP-002",),
    "DSP permanently": ("REQ-DSP-002",),
    "Marginal MDIO": ("REQ-DSP-001",),
    "Controller firmware hang": ("REQ-FLT-003",),
    "Module over-temperature": ("REQ-FLT-001",),
    "Supply brown-out": ("REQ-FLT-001",),
    "Fiber cut": ("REQ-MGMT-004",),
    "Host electrical LOS": ("REQ-DSP-001",),
}

_RESULTS = None


def _results():
    global _RESULTS
    if _RESULTS is None:
        _RESULTS = {r.name: r for r in dvt.fault_matrix()}
    return _RESULTS


def _params():
    out = []
    for prefix, reqs in REQ.items():
        out.append(pytest.param(prefix, marks=[pytest.mark.req(*reqs)], id=prefix))
    return out


@pytest.mark.parametrize("prefix", _params())
def test_fault_scenario(prefix):
    matches = [r for n, r in _results().items() if n.startswith(prefix)]
    assert len(matches) == 1
    assert_result(matches[0])
