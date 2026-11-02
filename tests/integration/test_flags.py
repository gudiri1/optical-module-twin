# SPDX-License-Identifier: Apache-2.0
"""Latched flags, masks and IntL."""
import pytest

from omt import cmis
from omt.host import CmisHost
from omt.sim import SimModule


@pytest.fixture
def up():
    m = SimModule(seed=8)
    h = CmisHost(m)
    h.bring_up()
    m.run(2000)
    h.read_flags()
    yield m, h
    m.close()


@pytest.mark.req("REQ-MGMT-004")
def test_rx_los_latches_and_clears_on_read(up):
    m, h = up
    assert m.get("intl") == 1
    m.set("rx_dbm.2", -40)
    m.run(300)
    assert m.get("intl") == 0
    f = h.read_flags()
    assert f["rx_los"] == 0x4 and f["rx_power_low_alarm"] == 0x4
    assert m.get("intl") == 1            # serviced -> IntL released
    assert h.read_flags()["rx_los"] == 0  # cleared on read


@pytest.mark.req("REQ-MGMT-004")
def test_masked_flag_does_not_assert_intl(up):
    m, h = up
    h.select(0x10)
    mask = bytearray(19)
    for i, name in enumerate(cmis.P11_FLAGS.values()):
        if name.startswith("rx_"):
            mask[i] = 0xFF
    h.write(213, bytes(mask))
    m.set("rx_dbm", -40)
    m.run(300)
    assert m.get("intl") == 1            # masked
    assert h.read_flags()["rx_los"] == 0x0F   # but still latched


@pytest.mark.req("REQ-DDM-003")
def test_no_spurious_flags_after_bring_up():
    m = SimModule(seed=12)
    h = CmisHost(m)
    h.bring_up()
    m.run(3000)
    f = h.read_flags()
    noisy = {k: v for k, v in f.items() if v and k not in ("module", "dp_state_changed")}
    assert noisy == {}, noisy


@pytest.mark.req("REQ-DDM-002")
def test_thresholds_reachable_before_protection():
    m = SimModule(seed=1)
    h = CmisHost(m)
    thr = cmis.decode_page02(h.upper(0x02))
    assert thr["tx_bias_ma"]["high_alarm"] < 105.0     # APC bias clamp in tx.c
    assert thr["temp_c"]["high_alarm"] < 85.0           # module shutdown in monitor.c
    assert thr["aux1_c"]["high_alarm"] < 72.0           # laser shutdown
    assert thr["vcc_v"]["low_alarm"] > 2.90             # brown-out fault
