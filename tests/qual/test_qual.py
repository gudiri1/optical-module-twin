# SPDX-License-Identifier: Apache-2.0
"""Qualification campaign on the twin (reduced cycle counts for CI)."""
import pytest
from helpers import assert_result

from omt import qual

pytestmark = pytest.mark.dvt


@pytest.mark.req("REQ-QUAL-001", "REQ-TX-001")
def test_htol_readpoints_and_life_prediction():
    r = qual.htol()
    assert_result(r)
    assert all(lead >= 1.0 for lead in r.data["warn_lead_years"])


@pytest.mark.req("REQ-QUAL-002", "REQ-TEC-001")
def test_temperature_cycling():
    assert_result(qual.temp_cycling(cycles=5, dwell_s=90))


@pytest.mark.req("REQ-QUAL-003", "REQ-MGMT-006")
def test_power_cycling_and_nvm_wear():
    r = qual.power_cycling(cycles=400, eeprom_bursts=120)
    assert_result(r)
    assert r.data["erases_per_eeprom_burst"] < 0.05     # was 1.0 before F-010
