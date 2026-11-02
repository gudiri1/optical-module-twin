# SPDX-License-Identifier: Apache-2.0
"""Platform-aware discovery of the compiled sim library.

Regression for the Windows/macOS case: the loader previously only looked for
``libomt_sim.so`` and raised FileNotFoundError on other platforms.
"""
import types
from pathlib import Path

from omt import sim


def test_lib_filenames_per_platform(monkeypatch):
    monkeypatch.setattr(sim, "sys", types.SimpleNamespace(platform="win32"))
    assert sim._lib_filenames()[0] == "libomt_sim.dll"

    monkeypatch.setattr(sim, "sys", types.SimpleNamespace(platform="darwin"))
    assert sim._lib_filenames()[0] == "libomt_sim.dylib"

    monkeypatch.setattr(sim, "sys", types.SimpleNamespace(platform="linux"))
    assert sim._lib_filenames() == ("libomt_sim.so",)


def test_find_lib_honors_env_override(monkeypatch, tmp_path):
    override = tmp_path / "somewhere" / "custom_sim.bin"
    monkeypatch.setenv("OMT_SIM_LIB", str(override))
    assert sim._find_lib() == Path(str(override))
