# SPDX-License-Identifier: Apache-2.0
"""Host tooling: image builder parity, lint, dump formats, trace, console."""
import pytest

from omt import cmis, image, lint, vcd
from omt.host import CmisHost
from omt.sim import SimModule, build_image


def test_python_image_builder_matches_firmware():
    for args in [(1, 0, 100, 0, "factory"), (2, 7, 65535, 1, "x" * 20)]:
        assert image.build(*args) == build_image(*args)


@pytest.fixture(scope="module")
def snap():
    m = SimModule(seed=4)
    h = CmisHost(m)
    h.bring_up()
    m.run(3000)
    h.read_flags()
    return h.snapshot()


def test_lint_clean_module(snap):
    errs = [f for f in lint.lint(snap) if f.severity != "info"]
    assert errs == []


def test_lint_catches_corruption(snap):
    p00 = bytearray(snap.pages[0])
    p00[148 - 128] = 0x07                        # control char in part number
    p02 = bytearray(snap.pages[2])
    p02[0], p02[1] = 0x00, 0x00                  # temp high alarm below warning
    bad = cmis.CmisImage(snap.lower, {**snap.pages, 0: bytes(p00), 2: bytes(p02)})
    rules = {f.rule for f in lint.lint(bad) if f.severity == "error"}
    assert {"L003", "L006", "L008", "L005"} <= rules


def test_dump_roundtrip_json_raw_hexdump(tmp_path, snap):
    j = tmp_path / "d.json"
    j.write_text(snap.to_json())
    assert cmis.CmisImage.load(j).pages[0x11] == snap.pages[0x11]
    raw = snap.lower + snap.pages[0] + snap.pages[1] + snap.pages[2] + snap.pages[0x10] + snap.pages[0x11]
    rb = tmp_path / "d.bin"
    rb.write_bytes(raw)
    assert cmis.CmisImage.load(rb).pages[0x11] == snap.pages[0x11]
    lines = ["Offset\t\tValues", "------\t\t------"]
    blob = snap.lower + snap.pages[0]
    for off in range(0, 256, 16):
        lines.append(f"0x{off:04x}:\t\t" + " ".join(f"{b:02x}" for b in blob[off:off + 16]))
    hx = tmp_path / "ethtool.txt"
    hx.write_text("\n".join(lines))
    img = cmis.CmisImage.load(hx)
    assert cmis.decode_page00(img.pages[0])["vendor_pn"] == "OMT-400G-DR4"


def test_i2c_trace_to_vcd(tmp_path):
    m = SimModule(seed=5)
    h = CmisHost(m)
    h.bring_up()
    recs = m.trace_take()
    assert any(k == "W" and off == 126 for _, k, off, _ in recs)   # page selects
    n = vcd.to_vcd(recs[:50], str(tmp_path / "bus.vcd"))
    txt = (tmp_path / "bus.vcd").read_text()
    assert n > 1000 and "$var wire 1 ! SCL $end" in txt


def test_console_commands():
    m = SimModule(seed=6)
    for cmd, needle in [("help", "mdio"), ("ver", "fw 1.0"), ("state", "LowPwr"),
                        ("mdio r 1 2", "mdio error"), ("ld r 0", "0xFFFF") , ("bogus", "unknown")]:
        assert needle in m.console(cmd), cmd
    CmisHost(m).bring_up()
    assert "0x7F44" in m.console("mdio r 1 2")
    assert "0x4C44" in m.console("ld r 0")   # unpowered bus floats high (0xFFFF) above
