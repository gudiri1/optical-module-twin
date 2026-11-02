# SPDX-License-Identifier: Apache-2.0
"""Host management interface: memory map, state machines, data paths."""
import pytest
from helpers import assert_result

from omt import cmis, dvt
from omt.host import CmisHost
from omt.sim import SimModule


@pytest.fixture
def mod():
    m = SimModule(seed=42)
    yield m, CmisHost(m)
    m.close()


@pytest.mark.req("REQ-MGMT-001")
def test_pages_present_and_identity(mod):
    m, h = mod
    img = h.snapshot(pages=(0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x9F))
    d = cmis.decode(img)
    assert d["lower"]["identifier"] == "QSFP-DD"
    assert d["lower"]["cmis_rev"] == "5.2"
    assert not d["lower"]["flat_memory"]
    assert d["page00h"]["vendor_pn"] == "OMT-400G-DR4"
    assert d["page00h"]["vendor_sn"].startswith("OMT26")
    assert [a["media_if"] for a in d["lower"]["applications"]] == ["400GBASE-DR4", "100GBASE-DR"]


@pytest.mark.req("REQ-MGMT-003")
def test_checksums_valid_in_every_state(mod):
    m, h = mod
    for step in ("lowpwr", "ready", "after_fwinfo"):
        if step == "ready":
            h.bring_up()
        if step == "after_fwinfo":
            h.fw_info()
            m.run(2500)
        assert cmis.decode_page00(h.upper(0))["checksum_ok"]
        assert cmis.decode_page01(h.upper(1))["checksum_ok"]
        assert cmis.decode_page02(h.upper(2))["checksum_ok"]


@pytest.mark.req("REQ-MSM-001")
def test_module_state_machine_lpmode_and_sw(mod):
    m, h = mod
    assert h.module_state() == 1                      # LPMode pin asserted
    m.set("lpmode", 0)
    h.wait(lambda: h.module_state() == 3, 5000)       # pin released -> Ready
    h.write(26, 0x50)                                  # LowPwrRequestSW
    h.wait(lambda: h.module_state() == 1, 1000)
    assert m.get("power_w") < 1.5                      # DSP and lasers off
    h.write(26, 0x00)                                  # ignore pin, no SW request
    m.set("lpmode", 1)
    h.wait(lambda: h.module_state() == 3, 5000)


@pytest.mark.req("REQ-MSM-002")
def test_power_up_timing_within_advertised():
    assert_result(dvt.power_up_timing(seed=3))


@pytest.mark.req("REQ-DP-001", "REQ-DP-003", "REQ-DP-002")
def test_breakout_and_lane_control():
    assert_result(dvt.breakout())


@pytest.mark.req("REQ-DP-001")
def test_400g_bring_up_all_lanes_light(mod):
    m, h = mod
    h.bring_up()
    m.run(500)
    for i in range(4):
        assert abs(m.get(f"tx_dbm.{i}") - 1.0) < 0.3
        assert m.get(f"media_lock.{i}") and m.get(f"host_lock.{i}")


@pytest.mark.req("REQ-MGMT-005")
def test_monitor_reads_coherent_during_ramp(mod):
    """16-bit monitors read in one transaction never show byte tearing."""
    m, h = mod
    h.bring_up()
    m.set("case_temp", 70)
    prev = None
    for _ in range(600):
        b = m.read(14, 2)
        t = cmis.s16(b, 0) / 256
        if prev is not None:
            assert abs(t - prev) < 0.5, (prev, t)
        prev = t
        m.run(37)
    assert m.get("irq_depth") == 0


@pytest.mark.req("REQ-MGMT-002", "REQ-ROB-001")
def test_host_fuzz():
    assert_result(dvt.host_fuzz(n=3000))


@pytest.mark.req("REQ-MGMT-006")
def test_user_eeprom_survives_power_cycle(mod):
    m, h = mod
    h.select(0x03)
    h.write(130, b"host-note")
    m.run(800)
    m.set("power_cycle", 1)
    m.run(100)
    h._page = None
    h.select(0x03)
    assert h.read(130, 9) == b"host-note"


def test_module_nacks_during_boot_and_host_recovers(mod):
    m, h = mod
    m.set("power_cycle", 1)
    with pytest.raises(Exception):
        m.read(0, 1)
    assert h.read(0, 1) == b"\x18"                      # host retries through boot
    assert h.nacks >= 1
