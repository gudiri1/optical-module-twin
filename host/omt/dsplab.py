# SPDX-License-Identifier: Apache-2.0
"""PAM4 receiver lab: equalization, clock recovery and KP4 FEC.

Link-level Monte-Carlo of one 100G PAM4 lane, used to justify the DSP model
in the digital twin (``sim/src/twin.cpp``):

  * channel: band-limited (RC low-pass, tau = 0.7 UI) -> ISI with
    pre/post cursors, plus AWGN
  * CDR: baud-rate Mueller-Mueller timing error detector with a PI loop,
    recovering an initial sampling-phase error during link training (known
    PRBS training pattern, as in IEEE 802.3 PAM4 link training). Decision-
    directed MM on the *unequalized* eye is biased by ISI and walks off -
    kept as a documented experiment (``run_cdr(..., n_aided=1500)``).
  * FFE: 7-tap feed-forward equalizer, LMS adapted (data-aided training,
    then frozen)
  * slicer + Gray demapping -> BER, compared with the closed form
    BER = 3/8 erfc(sqrt(SNR/10)) the twin uses
  * RS(544,514) "KP4" FEC: 10-bit symbols, t = 15, codeword failure ratio
    vs pre-FEC BER (analytic), and an error-correlation check from the
    simulated error positions
"""
from __future__ import annotations

import math

import numpy as np

OS = 16                       # samples per UI in the waveform model
LEVELS = np.array([-3.0, -1.0, 1.0, 3.0])
GRAY_BITS = {-3: (0, 0), -1: (0, 1), 1: (1, 1), 3: (1, 0)}
KP4_N, KP4_T, KP4_M = 544, 15, 10
KP4_BER_THRESHOLD = 2.4e-4


def _erfc(x):
    return np.vectorize(math.erfc)(x)


def ber_pam4(snr_lin):
    """Closed form used in the twin: Gray PAM4, SNR = symbol power / noise power."""
    return 0.375 * _erfc(np.sqrt(np.asarray(snr_lin, dtype=float) / 10.0))


def pulse(tau_ui=0.7, span=8):
    """Rectangular NRZ pulse through a 1st-order RC, sampled at OS per UI."""
    t = np.arange(span * OS) / OS
    h = np.exp(-t / tau_ui)
    h /= h.sum()
    rect = np.ones(OS)
    p = np.convolve(rect, h)[: span * OS]
    return p / p.max()


def make_waveform(n_sym, rng, tau_ui=0.7):
    sym = rng.choice(LEVELS, n_sym)
    up = np.zeros(n_sym * OS)
    up[::OS] = sym
    p = pulse(tau_ui)
    y = np.convolve(up, p)[: n_sym * OS]
    peak = int(np.argmax(p))
    return sym, y, peak, p


def _sample(y, pos):
    i = np.floor(pos).astype(int)
    f = pos - i
    i = np.clip(i, 0, len(y) - 2)
    return y[i] * (1 - f) + y[i + 1] * f


def _slice(x):
    return np.clip(2 * np.round((x - 1) / 2) + 1, -3, 3)


def run_cdr(y, peak, n, phase0_ui, sym=None, kp=1.5e-3, ki=3e-6, n_aided=1500):
    """Mueller-Mueller TED + PI loop on baud-rate samples.

    The first ``n_aided`` symbols use the known training pattern (data-aided
    acquisition), then decisions take over (decision-directed tracking).
    Returns the phase trajectory (UI) and the final phase.
    """
    tau = phase0_ui
    integ = 0.0
    prev_x = prev_d = 0.0
    traj = np.empty(n)
    for k in range(n):
        x = float(_sample(y, np.array([peak + (k + tau) * OS]))[0])
        d = float(sym[k]) if (sym is not None and k < n_aided) else float(_slice(np.array([x]))[0])
        e = x * prev_d - prev_x * d          # MM timing error (S-curve: + when early)
        integ += ki * e
        tau = float(np.clip(tau + kp * e + integ, -0.5, 0.5))
        prev_x, prev_d = x, d
        traj[k] = tau
    return traj, tau


def ted_s_curve(tau_ui=0.7, n=20000, seed=3):
    """Mean MM timing error vs. sampling phase (ideal decisions)."""
    rng = np.random.default_rng(seed)
    sym, y, peak, _ = make_waveform(n, rng, tau_ui)
    phases = np.round(np.arange(-0.5, 0.51, 0.05), 3)
    out = []
    for ph in phases:
        x = _sample(y, peak + (np.arange(n) + ph) * OS)
        out.append(float(np.mean(x[1:] * sym[:-1] - x[:-1] * sym[1:])))
    return phases, np.array(out)


def train_ffe(x, sym, ntaps=7, pre=2, mu=4e-4, n_train=6000):
    w = np.zeros(ntaps)
    w[pre] = 1.0
    mse = []
    for k in range(ntaps, n_train):
        v = x[k - ntaps + 1: k + 1][::-1]       # newest first
        target = sym[k - pre]              # main tap sits on x[k - pre]
        z = w @ v
        e = target - z
        w += mu * e * v
        mse.append(e * e)
    return w, np.array(mse)


def link_ber(snr_db, n_sym=60000, ffe=True, seed=1, phase0_ui=0.3, tau_ui=0.7, return_detail=False):
    rng = np.random.default_rng(seed)
    sym, y, peak, p = make_waveform(n_sym, rng, tau_ui)
    main = p[peak]
    sigma = math.sqrt(5.0 * main ** 2 / 10 ** (snr_db / 10))
    y = y + rng.normal(0, sigma, len(y))

    traj, tau = run_cdr(y, peak, 4000, phase0_ui, sym, n_aided=4000)
    x = _sample(y, peak + (np.arange(n_sym) + tau) * OS)

    ntaps, pre = 7, 2
    if ffe:
        w, mse = train_ffe(x, sym, ntaps, pre)
    else:
        w, mse = np.eye(ntaps)[pre], np.array([])
    z = np.convolve(x, w, mode="full")[: n_sym]        # z[k] = sum w[j] x[k-j]
    delay = pre
    z = z[delay:]
    ref = sym[: len(z)]
    skip = 8000
    z, ref = z[skip:], ref[skip:]
    dec = _slice(z)
    sym_err = dec != ref
    bits_ref = np.array([GRAY_BITS[int(s)] for s in ref])
    bits_dec = np.array([GRAY_BITS[int(s)] for s in dec])
    bit_err = (bits_ref != bits_dec)
    ber = bit_err.mean()
    mse_post = float(np.mean((z - ref) ** 2))
    out = {"snr_db": snr_db, "ber": float(ber), "ser": float(sym_err.mean()),
           "snr_eff_db": 10 * math.log10(5.0 / mse_post), "phase_ui": float(tau),
           "taps": w.tolist(), "n_bits": int(bit_err.size)}
    if return_detail:
        out.update({"cdr_traj": traj, "mse": mse, "bit_err": bit_err.reshape(-1)})
    return out


# ------------------------------------------------------------------- FEC

def kp4_cw_failure(pre_fec_ber):
    """P(codeword uncorrectable) for independent bit errors."""
    ps = 1 - (1 - pre_fec_ber) ** KP4_M
    tot = 0.0
    for k in range(KP4_T + 1, KP4_N + 1):
        lt = (math.lgamma(KP4_N + 1) - math.lgamma(k + 1) - math.lgamma(KP4_N - k + 1)
              + k * math.log(ps) + (KP4_N - k) * math.log1p(-ps))
        term = math.exp(lt)
        tot += term
        if term < tot * 1e-17 and k > KP4_N * ps + 10:
            break
    return tot


def symbol_error_correlation(bit_err: np.ndarray):
    """Compare measured 10-bit RS symbol error ratio with the independence model."""
    n = (bit_err.size // KP4_M) * KP4_M
    sym = bit_err[:n].reshape(-1, KP4_M).any(axis=1)
    ber = bit_err.mean()
    expected = 1 - (1 - ber) ** KP4_M
    return {"ber": float(ber), "rs_symbol_err": float(sym.mean()), "independent_model": float(expected),
            "ratio": float(sym.mean() / expected) if expected else float("nan")}


def sweep(snrs=(11, 12, 13, 14, 15, 16, 17, 18), n_sym=80000):
    rows = []
    for s in snrs:
        a = link_ber(s, n_sym, ffe=True)
        b = link_ber(s, n_sym, ffe=False)
        rows.append({"snr_db": s, "ber_ffe": a["ber"], "ber_noeq": b["ber"],
                     "snr_eff_ffe": a["snr_eff_db"], "snr_eff_noeq": b["snr_eff_db"],
                     "ber_theory_at_snr_eff": float(ber_pam4(10 ** (a["snr_eff_db"] / 10)))})
    return rows
