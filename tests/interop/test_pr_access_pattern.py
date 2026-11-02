# SPDX-License-Identifier: Apache-2.0
"""Host access pattern of Zephyr PR #123842 (drivers/sfp/cmis_module.c),
replayed against the twin.

Only the *I2C transaction sequence* of the driver is reproduced here (in
Python, from reading the driver source); no PR code is executed:

  read_monitors(): write_read(offset 0x00, 18 bytes)
  read_id():       write(0x7F = 0), write_read(0x00, 128), write_read(0x80, 128)

Both reads cover lower-memory bytes 8..11, which CMIS defines as latched,
clear-on-read flags. The tests document the consequences for review
(docs/findings.md, P-1).
"""
import pytest

from omt.host import CmisHost
from omt.sim import SimModule


def pr_read_monitors(m):
    return m.read(0x00, 18)


def pr_read_id(m):
    m.write(0x7F, b"\x00")
    return m.read(0x00, 128) + m.read(0x80, 128)


@pytest.fixture
def mod():
    m = SimModule(seed=88)
    h = CmisHost(m)
    h.bring_up()
    m.run(2000)
    h.read_flags()
    yield m, h
    m.close()


@pytest.mark.req("REQ-INT-001")
def test_read_monitors_reports_temp_flags_once(mod):
    m, h = mod
    m.set("case_temp", 76)
    m.run(150000)
    first = pr_read_monitors(m)
    second = pr_read_monitors(m)
    assert first[9] & 0x04          # temp high warning reported ...
    assert second[9] == 0           # ... exactly once (COR), condition still present
    assert m.get("module_temp") > 75


@pytest.mark.req("REQ-INT-001")
def test_read_monitors_consumes_flags_it_does_not_report(mod):
    """Byte 8 (ModuleStateChanged, CdbCmdComplete) and bytes 10/11 (Aux) are
    cleared by the 18-byte read although cmis_parse_monitors() does not
    return them: an interrupt handler running afterwards finds nothing."""
    m, h = mod
    h.low_power(True)
    m.run(200)                      # ModuleReady -> LowPwr latches byte 8 bit 0
    assert m.get("intl") == 0       # module raised IntL
    raw = pr_read_monitors(m)       # e.g. a periodic `cmis mon`
    assert raw[8] & 0x01            # the driver saw ModuleStateChanged ...
    assert h.read(8, 1) == b"\x00"  # ... dropped it, and the host's IRQ path finds nothing


@pytest.mark.req("REQ-INT-001")
def test_read_id_also_clears_flags(mod):
    m, h = mod
    m.set("rx_dbm", -40)            # fiber cut -> lane flags (page 11h, not affected)
    m.set("vcc", 3.10)              # + Vcc low warning (lower byte 9, affected)
    m.run(2000)
    pr_read_id(m)
    assert h.read(9, 1) == b"\x00"
    h._page = None                  # see test_page_select_cache_hazard
    assert h.read_flags()["rx_los"] == 0x0F   # page 11h flags survive


def test_page_select_cache_hazard(mod):
    """Two host agents on one module (e.g. a transceiver manager caching the
    page select, plus `cmis info` from the Zephyr shell): the foreign page
    write makes the caching agent read Page 00h believing it is Page 11h."""
    m, h = mod
    m.set("rx_dbm", -40)
    m.run(1000)
    h.select(0x11)                  # agent A caches page 11h
    pr_read_id(m)                   # agent B selects page 00h
    stale = h.read(148, 1)[0]       # agent A: "RX CDR LOL flags" - actually Page 00h byte 148
    assert stale == ord("O")        # 'O' of part number "OMT-400G-DR4" = 0x4F -> phantom LOL on 5 lanes
    h._page = None                  # mitigation: re-select (or never cache) per access
    assert h.read_flags()["rx_los"] == 0x0F
