# SPDX-License-Identifier: Apache-2.0
"""Host-side signal integrity (Page 10h/11h) and PRBS diagnostics (Page 13h/14h)."""
import pytest

from omt import cmis
from omt.host import CmisHost
from omt.sim import SimModule

PRBS31Q = cmis.PATTERN_CODES["PRBS31Q"]


@pytest.fixture
def up():
    m = SimModule(seed=41)
    h = CmisHost(m)
    h.bring_up()
    m.run(500)
    h.read_flags()
    yield m, h
    m.close()


def test_f16_roundtrip():
    # CMIS F16: value = mantissa * 10^(exponent - 24)
    assert cmis.f16((21 << 11) | 240) == pytest.approx(2.4e-1)
    assert cmis.f16((6 << 11) | 1234) == pytest.approx(1.234e-15)
    assert cmis.f16(0) == 0.0


@pytest.mark.req("REQ-MGMT-001", "REQ-DIAG-001")
def test_diagnostic_pages_advertised(up):
    m, h = up
    p01 = h.upper(0x01)
    assert p01[142 - 128] & 0x20                      # pages 13h-14h supported
    p13 = cmis.decode_page13(h.upper(0x13))
    assert set(p13["loopback_caps"]) == set(cmis.LOOPBACKS)
    for k in ("host_generator_patterns", "media_checker_patterns"):
        assert {"PRBS31Q", "PRBS13Q", "SSPRQ"} <= set(p13[k])


@pytest.mark.req("REQ-SI-001")
def test_adaptive_eq_follows_channel_loss(up):
    m, h = up
    for loss, eq in ((4, 4), (10, 10), (16, 12)):     # CTLE limited to 12 dB
        m.set("host_loss_db", loss)
        m.run(300)
        lanes = h.host_lanes()
        assert all(x["eq_db"] == eq for x in lanes), (loss, [x["eq_db"] for x in lanes])
        assert all(x["adaptive"] for x in lanes)


@pytest.mark.req("REQ-SI-001")
def test_fixed_eq_applied_immediately_without_dp_reinit(up):
    m, h = up
    status = h.set_host_eq(adaptive=False, fixed_db=3, lanes=0x0F)
    assert status[:4] == ["Success"] * 4
    m.run(300)
    lanes = h.host_lanes()
    assert [x["eq_db"] for x in lanes] == [3, 3, 3, 3, 8, 8, 8, 8]
    assert [x["adaptive"] for x in lanes] == [False] * 4 + [True] * 4
    assert all(s == 4 for s in h.dp_states())        # data path stayed activated
    active = cmis.decode_page11(h.upper(0x11))["active_si"]
    assert [a["fixed_eq_db"] for a in active[:4]] == [3] * 4


@pytest.mark.req("REQ-SI-002")
def test_host_lol_is_per_host_lane(up):
    m, h = up
    m.set("host_los.5", 1)                            # host lane 6 (media lane 3)
    m.run(400)
    f = h.read_flags()
    assert f["tx_los"] == 0x20 and f["tx_cdr_lol"] == 0x20
    assert f["rx_cdr_lol"] == 0                       # optical side unaffected


@pytest.mark.req("REQ-SI-001", "REQ-SI-002")
def test_channel_beyond_ctle_range_flags_adaptation_fault(up):
    m, h = up
    m.set("host_loss_db.2", 22)
    m.run(400)
    f = h.read_flags()
    assert f["tx_adapt_eq_fault"] == 0x04 and f["tx_cdr_lol"] == 0x04
    assert f["tx_los"] == 0


@pytest.mark.req("REQ-DIAG-001")
def test_host_checker_measures_host_channel_ber(up):
    m, h = up
    m.set("host_prbs", PRBS31Q)                       # switch ASIC sends PRBS31Q
    m.set("host_loss_db", 19)                         # at the edge of the CTLE
    m.run(300)
    r = h.prbs_measure("host", 2.0)
    assert r["lol"] == 0
    assert all(1e-4 < b < 1e-2 for b in r["ber"]), r["ber"]
    m.set("host_loss_db", 8)
    m.run(300)
    r = h.prbs_measure("host", 2.0)
    assert all(b < 1e-9 for b in r["ber"]), r["ber"]


@pytest.mark.req("REQ-DIAG-001")
def test_checker_reports_lol_on_pattern_mismatch(up):
    m, h = up
    m.set("host_prbs", cmis.PATTERN_CODES["PRBS13Q"])
    r = h.prbs_measure("host", 1.0, pattern="PRBS31Q", lanes=0x03)
    assert r["lol"] == 0x03 and r["ber"][:2] == [None, None]


@pytest.mark.req("REQ-DIAG-001", "REQ-DIAG-002")
def test_loopbacks_separate_optics_from_dsp(up):
    m, h = up
    # dirty connector: fiber loopback with too much loss on the optical path
    m.set("loopback", 1)
    m.set("loop_atten_db", 11)
    m.run(500)
    optical = h.prbs_measure("media", 2.0)
    electrical = h.prbs_measure("media", 2.0, loopback="media-output")
    assert optical["lol"] == 0 and all(b > 1e-4 for b in optical["ber"]), optical
    assert electrical["lol"] == 0 and all(b == 0 for b in electrical["ber"]), electrical
    host_lb = h.prbs_measure("host", 1.0, loopback="host-output")
    assert host_lb["lol"] == 0 and all(b == 0 for b in host_lb["ber"])


@pytest.mark.req("REQ-DIAG-001")
def test_diagnostics_survive_dsp_recovery(up):
    m, h = up
    m.set("host_prbs", PRBS31Q)
    h.prbs("host", "PRBS31Q")
    m.run(300)
    assert all(m.get(f"host_chk_lock.{i}") for i in range(8))
    m.set("dsp_hang", 1)
    m.run(400)
    m.set("dsp_hang", 0)
    m.run(1500)
    assert all(m.get(f"host_chk_lock.{i}") for i in range(8))  # re-programmed
    assert all(x["eq_db"] == 8 for x in h.host_lanes())        # Eq restored too


@pytest.mark.req("REQ-ROB-001")
def test_diag_pages_reject_writes_outside_controls(up):
    m, h = up
    h.select(0x13)
    before = h.read(128, 16)
    h.write(128, bytes(16))                          # capabilities are read-only
    assert h.read(128, 16) == before
    h.select(0x14)
    h.write(192, b"\x12\x34")                        # results are read-only
    assert h.read(192, 2) != b"\x12\x34"
