# SPDX-License-Identifier: Apache-2.0
"""CMIS CDB firmware management."""
import struct

import pytest
from helpers import assert_result

from omt import dvt
from omt.host import CmisHost
from omt.sim import SimModule

_R = None


def _suite():
    global _R
    if _R is None:
        _R = dvt.fw_update_suite()
    return _R


@pytest.mark.req("REQ-FW-001")
def test_update_run_commit():
    assert_result(_suite()[0])


@pytest.mark.req("REQ-FW-001")
def test_corrupted_image_rejected():
    assert_result(_suite()[1])


@pytest.mark.req("REQ-FW-002", "REQ-FLT-003")
def test_boot_failure_rolls_back():
    assert_result(_suite()[2])


@pytest.mark.req("REQ-FW-002")
def test_uncommitted_not_persistent():
    assert_result(_suite()[3])


@pytest.mark.req("REQ-FW-001")
def test_bad_checkcode_and_unsupported_command():
    m = SimModule(seed=2)
    h = CmisHost(m)
    h.select(0x9F)
    h.write(130, bytes([0, 0, 0, 0x00, 0, 0]))   # wrong check code
    h.write(128, b"\x00\x00")
    m.run(5)
    assert h.read(37, 1)[0] == 0x45
    st, _ = h.cdb(0x0777)
    assert st == 0x41
    st, _ = h.cdb(0x0103, struct.pack(">I", 0) + b"x")   # write without start
    assert st == 0x43
