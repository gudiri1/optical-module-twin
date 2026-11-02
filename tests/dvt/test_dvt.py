# SPDX-License-Identifier: Apache-2.0
"""Design verification (environmental, margining, receiver)."""
import pytest
from helpers import assert_result

from omt import dvt

pytestmark = pytest.mark.dvt


@pytest.mark.req("REQ-DDM-001", "REQ-TX-001", "REQ-TEC-001")
def test_temperature_sweep():
    assert_result(dvt.temp_sweep())


@pytest.mark.req("REQ-DDM-001")
def test_vcc_margining():
    assert_result(dvt.vcc_margin())


@pytest.mark.req("REQ-DSP-003", "REQ-DSP-001")
def test_rx_sensitivity():
    assert_result(dvt.rx_sensitivity())


@pytest.mark.req("REQ-DDM-001", "REQ-TX-001")
@pytest.mark.parametrize("seed", [101, 202, 303])
def test_temperature_sweep_other_units(seed):
    assert_result(dvt.temp_sweep(seed=seed, temps=(-5, 25, 75), settle_s=100))
