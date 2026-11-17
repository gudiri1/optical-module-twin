# SPDX-License-Identifier: Apache-2.0
"""Host electrical channel: FFT channel estimate, CTLE tuning, TX FIR (dsplab)."""
import numpy as np
import pytest

from omt import dsplab as d

IL = 16.0


@pytest.fixture(scope="module")
def link():
    return d.host_link(IL, ctle_db=0.0, return_wave=True)


@pytest.mark.req("REQ-SIG-001")
@pytest.mark.parametrize("blind", [False, True])
def test_fft_estimates_channel_loss(link, blind):
    f, h = d.estimate_channel_fft(link["x"], link["y"], blind=blind)
    true = float(d.insertion_loss_db(d.F_NYQ_HZ, IL) - d.insertion_loss_db(1e9, IL))
    assert abs(d.loss_at_nyquist_db(f, h) - true) < 1.0


@pytest.mark.req("REQ-SIG-002")
def test_fft_start_value_is_close_to_tuned_optimum(link):
    f, h = d.estimate_channel_fft(link["x"], link["y"], blind=True)
    start = d.ctle_from_fft(f, h)
    sweep = d.ctle_sweep(IL)
    best = max(sweep, key=sweep.get)
    assert abs(start - best) <= 4.0           # FFT gives the start value, the sweep refines it
    assert best <= start                      # noise enhancement pulls the optimum below "flat"
    assert sweep[best] > sweep[0.0] + 8.0     # the CTLE is what opens the eye at 16 dB loss


@pytest.mark.req("REQ-SIG-003")
def test_tx_fir_deemphasis_helps_and_wrong_sign_hurts():
    base = d.host_link(IL, ctle_db=12.0)["esnr_db"]
    good = d.host_link(IL, ctle_db=12.0, tx_post=-0.15)["esnr_db"]
    bad = d.host_link(IL, ctle_db=12.0, tx_post=+0.15)["esnr_db"]
    assert good > base + 1.0
    assert bad < base - 3.0
