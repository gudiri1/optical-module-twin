# SPDX-License-Identifier: Apache-2.0
"""Manufacturing: calibration flow and lot capability."""
import pytest

from omt import mfg
from omt.sim import SimModule
from omt.host import CmisHost

pytestmark = pytest.mark.mfg


@pytest.fixture(scope="module")
def lot():
    return mfg.run_lot(n=24, seed0=500)


@pytest.mark.req("REQ-MFG-001")
def test_all_units_pass_final_test(lot):
    bad = [(u.serial, u.post) for u in lot["units"] if not u.passed]
    assert lot["yield"] == 1.0, bad


@pytest.mark.req("REQ-MFG-001")
@pytest.mark.parametrize("param", ["temp_err_c", "vcc_err_pct", "rx_err_db", "tx_err_db", "tx_abs_db"])
def test_post_cal_capability(lot, param):
    s = lot["stats"][param]
    assert s["cpk_post"] >= 1.33, (param, s["cpk_post"], s["mean_post"], s["sd_post"])


@pytest.mark.req("REQ-MFG-001")
def test_calibration_improves_rx_accuracy(lot):
    s = lot["stats"]["rx_err_db"]
    assert abs(s["worst_post"]) < abs(s["worst_pre"])


@pytest.mark.req("REQ-MFG-001")
def test_calibration_page_locked_without_password():
    m = SimModule(seed=9, golden_cal=False)
    h = CmisHost(m)
    h.select(0xB1)
    before = h.read(160, 4)
    h.write(160, b"\x11\x22\x33\x44")
    assert h.read(160, 4) == before


@pytest.mark.req("REQ-MFG-001")
def test_insane_coefficients_rejected():
    m = SimModule(seed=9, golden_cal=False)
    h = CmisHost(m)
    h.password(mfg.FACTORY_PASSWORD)
    h.select(0xB1)
    h.write(164, b"\x40\x00\x00\x00")   # vcc_gain = 2.0
    h.write(255, 0xA5)
    m.run(100)
    assert h.read(255, 1) == b"\xFE"
