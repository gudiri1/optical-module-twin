# SPDX-License-Identifier: Apache-2.0
"""SCPI instrument layer: drivers, simulated bench, TCP transport, logging."""
import csv

import pytest

from omt import instruments as ins
from omt.host import CmisHost
from omt.sim import SimModule


@pytest.fixture
def bench():
    m = SimModule(seed=61)
    h = CmisHost(m)
    h.bring_up()
    m.run(1000)
    yield m, h, ins.Bench.simulated(m)
    m.close()


@pytest.mark.req("REQ-LAB-001")
def test_every_instrument_identifies_and_reports_no_error(bench):
    _, _, b = bench
    for role, inst in b.instruments().items():
        assert inst.idn().startswith("OMT,SIM"), role
        assert inst.errors() == [], role


@pytest.mark.req("REQ-LAB-001")
def test_unknown_command_lands_in_error_queue(bench):
    _, _, b = bench
    b.supply.write("VOLT:PROT 4.0")
    errs = b.supply.errors()
    assert len(errs) == 1 and errs[0].startswith("-113")


@pytest.mark.req("REQ-LAB-001")
def test_instruments_act_on_the_module(bench):
    m, h, b = bench
    b.voa.set_output_dbm(-8.0)
    b.forcer.set_temp(60)
    b.wait_s(120)
    assert b.voa.output_dbm() == pytest.approx(-8.0)
    assert m.get("rx_dbm.3") == pytest.approx(-8.0, abs=1e-6)
    assert b.forcer.temp() == pytest.approx(60)
    assert b.dmm.temperature(1) > 58                # module case follows the forcer
    assert b.dmm.temperature(2) == pytest.approx(50, abs=1)   # TEC holds the laser
    assert 3.0 < b.supply.power_w() < 10.0
    assert all(-1.0 < b.pm.read_dbm(i) < 4.0 for i in (1, 2, 3, 4))


@pytest.mark.req("REQ-LAB-001")
def test_supply_output_off_on_power_cycles_the_module(bench):
    m, h, b = bench
    resets = m.get("resets")
    b.supply.output(False)
    b.wait_s(0.2)
    b.supply.output(True)
    b.wait_s(0.2)
    assert m.get("resets") == resets + 1
    assert b.supply.voltage() == pytest.approx(3.3)


@pytest.mark.req("REQ-LAB-001")
def test_socket_transport_against_networked_instrument(bench):
    m, _, _ = bench
    srv, port = ins.serve(ins.SimSupply(m))
    try:
        psu = ins.PowerSupply(ins.open_transport(f"socket://127.0.0.1:{port}"), "psu")
        assert psu.idn() == "OMT,SIM-PSU,0,1.0"
        psu.set_voltage(3.25)
        assert psu.voltage() == pytest.approx(3.25)
        assert m.get("vcc") == pytest.approx(3.25)
        psu.t.close()
    finally:
        srv.shutdown()


@pytest.mark.req("REQ-LAB-002")
def test_transaction_log_is_time_stamped_and_exportable(bench, tmp_path):
    m, _, b = bench
    t0 = m.time_ms / 1000
    b.supply.set_voltage(3.2)
    b.wait_s(1.5)
    v = b.supply.voltage()
    out = tmp_path / "scpi.csv"
    b.write_log(out)
    rows = list(csv.DictReader(out.open()))
    assert rows[-2]["command"] == "VOLT 3.2000" and rows[-2]["response"] == ""
    assert rows[-1]["command"] == "MEAS:VOLT?" and float(rows[-1]["response"]) == pytest.approx(v)
    assert float(rows[-1]["t_s"]) - t0 == pytest.approx(1.5, abs=0.01)


@pytest.mark.req("REQ-LAB-001", "REQ-DIAG-001")
def test_bert_drives_module_checkers(bench):
    m, h, b = bench
    b.bert.pattern(1, "PRBS13Q")
    r = h.prbs_measure("host", 1.0, pattern="PRBS13Q")
    assert r["lol"] == 0
    b.bert.pattern(1, None)
    r = h.prbs_measure("host", 1.0, pattern="PRBS13Q", lanes=0x01)
    assert r["lol"] == 0x01
