// Optical Module Twin - hardware digital twin of a 400G QSFP-DD DR4 module.
//
// Models (all in 1 ms virtual-time steps):
//   * module thermal mass driven by case temperature and dissipated power
//   * laser plate thermal model with a Peltier TEC (asymmetric heat/cool)
//   * 4 directly biased lasers: temperature dependent threshold current and
//     slope efficiency, aging, back-facet monitor photodiode
//   * quad laser driver behind SPI, DSP behind MDIO Clause 45
//   * RX photodiodes + TIA RSSI into a 16-bit ADC with part-to-part spread
//   * PAM4 DSP: SNR from received OMA, BER = 3/8 erfc(sqrt(SNR/10)), Poisson
//     error counts, RS(544,514) KP4 FEC codeword statistics, CDR/FFE lock
//   * host-side SerDes (400GAUI-8, 8 x 26.5625 GBd PAM4): channel loss at
//     Nyquist vs. input CTLE peaking (adaptive or fixed), host CDR lock
//   * PRBS generators/checkers on host and media side, four loopbacks
//   * NOR flash with erase/program semantics, watchdog, reset pins
//
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "omt/fw.h"
}
#include "omt_sim.h"

namespace {

constexpr double kLineBitsPerMs = 106.25e6;          // 53.125 GBd PAM4
constexpr double kHostBitsPerMs = 53.125e6;          // 400GAUI-8 lane, 26.5625 GBd PAM4
constexpr double kHostSnrCeilDb = 27.0;              // host SerDes electrical ceiling
constexpr double kIsiDbPerDb = 1.2;                  // SNR loss per dB of residual ISI
constexpr double kNoiseEnhDbPerDb = 0.25;            // CTLE noise enhancement
constexpr double kHostLockSnrDb = 15.0;
constexpr int kMaxCtleDb = 12;
constexpr double kCodewordBits = 5440.0;             // RS(544,514), 10-bit symbols
constexpr double kNoiseOmaMw = 0.0231;               // noise-equivalent OMA (see dsp_lab)
constexpr double kSnrCeilingLin = 158.5;             // 22 dB implementation ceiling
constexpr double kOmaPerAvg = 1.333;                 // ER = 7 dB
constexpr uint32_t kWdtTimeoutMs = 300;
constexpr uint32_t kBootMs = 20;

double dbm_to_mw(double dbm) { return std::pow(10.0, dbm / 10.0); }
double mw_to_dbm(double mw) { return 10.0 * std::log10(std::max(mw, 1e-9)); }

// P(X >= 16) for X ~ Poisson(lambda): RS(544,514) corrects up to 15 symbols
double poisson_tail16(double lam)
{
	if (lam <= 0) return 0.0;
	if (lam > 16.0) {
		double cdf = 0;
		for (int k = 0; k < 16; k++) cdf += std::exp(-lam + k * std::log(lam) - std::lgamma(k + 1.0));
		return std::max(0.0, 1.0 - cdf);
	}
	double s = 0;
	for (int k = 16; k < 120; k++) s += std::exp(-lam + k * std::log(lam) - std::lgamma(k + 1.0));
	return s;
}

struct Laser {
	double ith0_ma, eta0_w_per_a, mpd_counts_per_mw;
	double aging = 0;       // 0 = new, 1 = end of life
	bool drv_fault_inject = false;
};

struct DspLane {
	uint16_t lctrl = 0;
	bool media_lock = false, ffe = false;
	uint64_t cond_since = 0;
	double snr_db = 0, ber = 0.375;
	uint64_t acc_err = 0, acc_bits = 0, acc_fc = 0, acc_fu = 0;
	uint32_t snap_err = 0, snap_bits16 = 0;
	uint16_t snap_fc = 0, snap_fu = 0;
	double cached_ber = -1, p_corr = 0, p_unc = 0;
};

// one host electrical lane (module Tx input) and its CDR
struct HostLane {
	uint16_t ctrl = 0x1;          // b0 adaptive, b7..4 fixed CTLE dB
	double loss_db = 8.0;         // host channel loss at Nyquist (stimulus)
	bool los = false;             // stimulus: no signal from the host
	bool lock = false, adapt_done = false;
	int eq_db = 0;
	double adapted_for = -1;      // loss the CTLE adapted to
	uint64_t since = 0, adapt_since = 0;
	double snr_db = 0, ber = 0.375;
};

// PRBS checker counters
struct Checker {
	bool lock = false;
	uint64_t acc_err = 0, acc_bits = 0;
	uint32_t snap_err = 0, snap_bits16 = 0;
};

double pam4_ber(double snr_db)
{
	return std::min(0.375, 0.375 * std::erfc(std::sqrt(std::pow(10.0, snr_db / 10.0) / 10.0)));
}

}  // namespace

struct omt_sim {
	// ---- time & randomness ----
	uint64_t t = 0;
	std::mt19937_64 rng;
	std::normal_distribution<double> n01{0.0, 1.0};
	std::uniform_real_distribution<double> u01{0.0, 1.0};

	// ---- firmware ----
	omt_fw fw{};
	omt_hal hal{};
	bool fw_running = false;
	bool reset_hold = false;
	uint64_t boot_at = 0;
	bool reset_pending = false;
	uint8_t reset_cause_pending = 0;
	uint8_t last_cause = OMT_RST_POWER_ON;
	bool fw_hang = false;
	uint64_t wdt_last = 0;
	uint32_t resets = 0, wdt_resets = 0;
	int irq_depth = 0;

	// ---- pins ----
	int lpmode = 1, resetl = 1;
	bool powered = true;
	int out[OMT_PIN_COUNT]{};

	// ---- environment & thermal ----
	double case_c = 25.0, vcc = 3.30;
	double mod_c = 25.0, laser_c = 25.0;
	double tec_u = 0.0;
	bool tec_open = false;
	double power_w = 0;
	// switched supply rails behind the enable pins (load switch + bulk C)
	double rail_tx_v = 0, rail_dsp_v = 0;

	// ---- per unit spread (manufacturing variation) ----
	double temp_sensor_off, vcc_gain_err, thermistor_off;
	double rssi_gain[4], rssi_dark[4];
	Laser las[4];

	// ---- laser driver (SPI) ----
	uint16_t ld_ctrl = 0, ld_status = 0, ld_bias[4]{};
	double tx_mw[4]{};    // laser (facet) power
	double fiber_mw[4]{}; // launched power

	// ---- optics in ----
	double rx_ext_dbm[4]{-3, -3, -3, -3};
	bool loopback = false;
	double loop_atten_db = 3.0;
	double rx_mw[4]{};

	// ---- DSP ----
	bool dsp_started = false, dsp_ready = false, dsp_hang = false, dsp_hang_sticky = false;
	uint64_t dsp_start_t = 0;
	uint16_t dsp_hb = 0, dsp_mode = 0;
	DspLane dl[4];
	HostLane hl[8];
	uint16_t hgen = 0, hchk = 0, mgen = 0, mchk = 0, lb_m = 0, lb_ho = 0, lb_hi = 0;
	uint16_t pat_h[8]{}, pat_m[4]{};
	Checker hc[8], mc[4];
	int host_prbs = -1;           // pattern the host ASIC transmits (-1: traffic)
	int far_prbs = -1;            // pattern arriving on the fiber from far end
	double mdio_fail_p = 0;

	// ---- flash ----
	std::vector<uint8_t> flash = std::vector<uint8_t>(OMT_NVM_SIZE, 0xFF);
	uint32_t flash_erases = 0;

	// ---- host bus trace & UART ----
	bool trace_on = true;
	std::vector<uint8_t> trace;
	uint64_t bus_free_us = 0;
	std::string uart;

	// =================================================================
	explicit omt_sim(uint32_t seed) : rng(seed)
	{
		temp_sensor_off = 1.2 * n01(rng);
		vcc_gain_err = 0.006 * n01(rng);
		thermistor_off = 0.5 * n01(rng);
		for (int l = 0; l < 4; l++) {
			rssi_gain[l] = 19000.0 * (1.0 + 0.08 * n01(rng));
			rssi_dark[l] = 200.0 + 25.0 * n01(rng);
			las[l].ith0_ma = 12.0 * (1.0 + 0.08 * n01(rng));
			las[l].eta0_w_per_a = 0.035 * (1.0 + 0.08 * n01(rng));
			las[l].mpd_counts_per_mw = 1000.0 * (1.0 + 0.10 * n01(rng));
		}
		setup_hal();
	}

	// ------------------------------------------------------------- HAL
	static omt_sim *S(void *c) { return static_cast<omt_sim *>(c); }

	void setup_hal()
	{
		hal.ctx = this;
		hal.millis = [](void *c) { return (uint32_t)S(c)->t; };
		hal.reset_cause = [](void *c) { return S(c)->last_cause; };
		hal.spi_xfer = [](void *c, const uint8_t *tx, uint8_t *rx, size_t n) { return S(c)->spi(tx, rx, n); };
		hal.mdio_read = [](void *c, uint8_t, uint8_t dev, uint16_t reg, uint16_t *v) {
			return S(c)->mdio_rd(dev, reg, v);
		};
		hal.mdio_write = [](void *c, uint8_t, uint8_t dev, uint16_t reg, uint16_t v) {
			return S(c)->mdio_wr(dev, reg, v);
		};
		hal.adc_read = [](void *c, uint8_t ch, uint16_t *raw) { return S(c)->adc(ch, raw); };
		hal.dac_write = [](void *c, uint8_t ch, uint16_t v) {
			if (ch == OMT_DAC_TEC) S(c)->tec_u = ((double)v - 32768.0) / 32767.0;
			return 0;
		};
		hal.gpio_get = [](void *c, uint8_t pin) {
			omt_sim *s = S(c);
			if (pin == OMT_PIN_LPMODE) return s->lpmode;
			if (pin == OMT_PIN_RESETL) return s->resetl;
			return pin < OMT_PIN_COUNT ? s->out[pin] : 0;
		};
		hal.gpio_set = [](void *c, uint8_t pin, int lvl) {
			if (pin < OMT_PIN_COUNT) S(c)->out[pin] = lvl ? 1 : 0;
		};
		hal.nvm_read = [](void *c, uint32_t a, void *b, size_t n) {
			omt_sim *s = S(c);
			if (a + n > s->flash.size()) return -1;
			std::memcpy(b, &s->flash[a], n);
			return 0;
		};
		hal.nvm_write = [](void *c, uint32_t a, const void *b, size_t n) {
			omt_sim *s = S(c);
			if (a + n > s->flash.size()) return -1;
			const uint8_t *p = static_cast<const uint8_t *>(b);
			for (size_t i = 0; i < n; i++) s->flash[a + i] &= p[i];  // NOR: program clears bits
			return 0;
		};
		hal.nvm_erase = [](void *c, uint32_t a, size_t n) {
			omt_sim *s = S(c);
			if ((a % OMT_NVM_SECTOR) || (n % OMT_NVM_SECTOR) || a + n > s->flash.size()) return -1;
			std::fill(s->flash.begin() + a, s->flash.begin() + a + n, 0xFF);
			s->flash_erases += (uint32_t)(n / OMT_NVM_SECTOR);
			return 0;
		};
		hal.wdt_kick = [](void *c) { S(c)->wdt_last = S(c)->t; };
		hal.uart_tx = [](void *c, const char *str, size_t n) { S(c)->uart.append(str, n); };
		hal.system_reset = [](void *c, uint8_t cause) {
			S(c)->reset_pending = true;
			S(c)->reset_cause_pending = cause;
		};
		hal.irq_lock = [](void *c) { S(c)->irq_depth++; };
		hal.irq_unlock = [](void *c) { S(c)->irq_depth--; };
	}

	// ----------------------------------------------------- laser driver
	double bias_ma(int l) const { return ld_bias[l] * 120.0 / 4095.0; }

	int spi(const uint8_t *tx, uint8_t *rx, size_t n)
	{
		if (n != 3) return -1;
		std::memset(rx, 0xFF, n);
		if (!out[OMT_PIN_TX_PWR]) return 0;  // unpowered: bus reads high
		bool rd = tx[0] & 0x80;
		uint8_t reg = tx[0] & 0x7F;
		uint16_t wv = (uint16_t)((tx[1] << 8) | tx[2]);
		uint16_t v = 0;
		if (rd) {
			if (reg == 0x00) v = 0x4C44;
			else if (reg == 0x01) v = ld_ctrl;
			else if (reg == 0x02) { v = ld_status; ld_status = 0; }
			else if (reg >= 0x10 && reg < 0x14) v = ld_bias[reg - 0x10];
			else if (reg >= 0x14 && reg < 0x18) {
				int l = reg - 0x14;
				v = (ld_ctrl & (1u << l)) ? ld_bias[l] : 0;
			} else if (reg >= 0x18 && reg < 0x1C) {
				int l = reg - 0x18;
				double c = tx_mw[l] * las[l].mpd_counts_per_mw + 0.7 * n01(rng);
				v = (uint16_t)std::clamp(c, 0.0, 4095.0);
			}
			rx[1] = (uint8_t)(v >> 8);
			rx[2] = (uint8_t)v;
		} else {
			if (reg == 0x01) {
				ld_ctrl = (wv & 0x8000) ? 0 : (wv & 0x00FF);
				if (wv & 0x8000) std::fill(std::begin(ld_bias), std::end(ld_bias), 0);
			} else if (reg >= 0x10 && reg < 0x14) {
				ld_bias[reg - 0x10] = wv & 0x0FFF;
			}
		}
		return 0;
	}

	// -------------------------------------------------------------- DSP
	bool dsp_alive() const { return out[OMT_PIN_DSP_PWR] && out[OMT_PIN_DSP_RSTN]; }

	void dsp_reset_state()
	{
		dsp_started = dsp_ready = false;
		if (!dsp_hang_sticky) dsp_hang = false;
		for (auto &d : dl) d = DspLane{};
		for (auto &h : hl) {  // keep the stimuli, lose the register state
			HostLane k;
			k.loss_db = h.loss_db;
			k.los = h.los;
			h = k;
		}
		hgen = hchk = mgen = mchk = lb_m = lb_ho = lb_hi = 0;
		std::fill(std::begin(pat_h), std::end(pat_h), 0);
		std::fill(std::begin(pat_m), std::end(pat_m), 0);
		for (auto &c : hc) c = Checker{};
		for (auto &c : mc) c = Checker{};
	}

	int mdio_rd(uint8_t dev, uint16_t reg, uint16_t *v)
	{
		if (!dsp_alive()) return -1;
		if (mdio_fail_p > 0 && u01(rng) < mdio_fail_p) return -1;
		*v = 0;
		if (dev == 1) {
			if (reg == 2) *v = 0x7F44;
			else if (reg == 3) *v = 0x5350;
			return 0;
		}
		if (dev != 30) return 0;
		if (reg == 0x0000) *v = (uint16_t)(dsp_mode << 1);
		else if (reg == 0x0001) *v = (dsp_ready && !dsp_hang) ? 1 : 0;
		else if (reg == 0x0003) *v = dsp_hb;
		else if (reg >= 0x0010 && reg < 0x0014) *v = dl[reg - 0x10].lctrl;
		else if (reg >= 0x0020 && reg < 0x0024) {
			int l = reg - 0x20;
			const DspLane &d = dl[l];
			bool hlk = hl[2 * l].lock && hl[2 * l + 1].lock;
			*v = (uint16_t)((hlk ? 1 : 0) | (d.media_lock ? 2 : 0) | (d.ffe ? 4 : 0));
		} else if (reg >= 0x0030 && reg < 0x0034) {
			*v = (uint16_t)std::clamp(dl[reg - 0x30].snr_db * 100.0, 0.0, 65535.0);
		} else if (reg >= 0x0040 && reg < 0x0050) {
			const DspLane &d = dl[(reg - 0x40) / 4];
			switch ((reg - 0x40) % 4) {
			case 0: *v = (uint16_t)(d.snap_err >> 16); break;
			case 1: *v = (uint16_t)d.snap_err; break;
			case 2: *v = (uint16_t)(d.snap_bits16 >> 16); break;
			default: *v = (uint16_t)d.snap_bits16; break;
			}
		} else if (reg >= 0x0060 && reg < 0x0064) *v = dl[reg - 0x60].snap_fc;
		else if (reg >= 0x0068 && reg < 0x006C) *v = dl[reg - 0x68].snap_fu;
		else if (reg == 0x0070) *v = (uint16_t)(int16_t)std::lround((mod_c + 8.0) * 10.0);
		else if (reg >= 0x0100 && reg < 0x0108) *v = hl[reg - 0x100].ctrl;
		else if (reg >= 0x0110 && reg < 0x0118) {
			const HostLane &h = hl[reg - 0x110];
			bool sig = (dl[(reg - 0x110) / 2].lctrl & 1) && !h.los;
			*v = (uint16_t)((sig ? 1 : 0) | (h.lock ? 2 : 0) | (h.adapt_done ? 4 : 0));
		} else if (reg >= 0x0120 && reg < 0x0128) {
			*v = (uint16_t)std::clamp(hl[reg - 0x120].snr_db * 100.0, 0.0, 65535.0);
		} else if (reg >= 0x0130 && reg < 0x0138) *v = (uint16_t)hl[reg - 0x130].eq_db;
		else if (reg == 0x0200) *v = hgen;
		else if (reg == 0x0201) *v = hchk;
		else if (reg == 0x0202) *v = mgen;
		else if (reg == 0x0203) *v = mchk;
		else if (reg == 0x0204) *v = lb_m;
		else if (reg == 0x0205) *v = lb_ho;
		else if (reg == 0x0206) *v = lb_hi;
		else if (reg >= 0x0208 && reg < 0x0210) *v = pat_h[reg - 0x208];
		else if (reg >= 0x0210 && reg < 0x0214) *v = pat_m[reg - 0x210];
		else if (reg == 0x0218) {
			uint16_t m = 0;
			for (int i = 0; i < 8; i++) m |= (uint16_t)(hc[i].lock ? 1u << i : 0u);
			for (int i = 0; i < 4; i++) m |= (uint16_t)(mc[i].lock ? 1u << (8 + i) : 0u);
			*v = m;
		} else if (reg >= 0x0220 && reg < 0x0250) {
			int idx = (reg - 0x220) / 4;
			const Checker &c = idx < 8 ? hc[idx] : mc[idx - 8];
			switch ((reg - 0x220) % 4) {
			case 0: *v = (uint16_t)(c.snap_err >> 16); break;
			case 1: *v = (uint16_t)c.snap_err; break;
			case 2: *v = (uint16_t)(c.snap_bits16 >> 16); break;
			default: *v = (uint16_t)c.snap_bits16; break;
			}
		}
		return 0;
	}

	int mdio_wr(uint8_t dev, uint16_t reg, uint16_t v)
	{
		if (!dsp_alive()) return -1;
		if (mdio_fail_p > 0 && u01(rng) < mdio_fail_p) return -1;
		if (dev != 30 || dsp_hang) return 0;  // a hung DSP ignores writes
		if (reg == 0x0000) {
			if (v & 0x8000) { dsp_reset_state(); return 0; }
			uint16_t mode = (v >> 1) & 1;
			if (dsp_ready && mode != dsp_mode)
				for (auto &d : dl) { d.media_lock = d.ffe = false; d.cond_since = t; }
			dsp_mode = mode;
			if ((v & 1) && !dsp_started) { dsp_started = true; dsp_start_t = t; }
		} else if (reg == 0x0002 && (v & 1)) {
			for (auto &d : dl) {
				d.snap_err = (uint32_t)std::min<uint64_t>(d.acc_err, 0xFFFFFFFFu);
				d.snap_bits16 = (uint32_t)std::min<uint64_t>(d.acc_bits >> 16, 0xFFFFFFFFu);
				d.snap_fc = (uint16_t)std::min<uint64_t>(d.acc_fc, 0xFFFF);
				d.snap_fu = (uint16_t)std::min<uint64_t>(d.acc_fu, 0xFFFF);
				d.acc_err = d.acc_bits = d.acc_fc = d.acc_fu = 0;
			}

		} else if (reg >= 0x0010 && reg < 0x0014) {
			DspLane &d = dl[reg - 0x10];
			if ((v & 2) && !(d.lctrl & 2)) d.cond_since = t;
			d.lctrl = v & 3;
			if (!(v & 2)) d.media_lock = d.ffe = false;
		} else if (reg >= 0x0100 && reg < 0x0108) {
			HostLane &h = hl[reg - 0x100];
			uint16_t nv = v & 0xF1;
			if (nv != h.ctrl) { h.adapted_for = -1; h.lock = false; h.since = t; }
			h.ctrl = nv;
		} else if (reg == 0x0200) hgen = v & 0xFF;
		else if (reg == 0x0201) hchk = v & 0xFF;
		else if (reg == 0x0202) mgen = v & 0x0F;
		else if (reg == 0x0203) mchk = v & 0x0F;
		else if (reg == 0x0204) lb_m = v & 0xFF;
		else if (reg == 0x0205) lb_ho = v & 0xFF;
		else if (reg == 0x0206) lb_hi = v & 0xFF;
		else if (reg >= 0x0208 && reg < 0x0210) pat_h[reg - 0x208] = v & 0xF;
		else if (reg >= 0x0210 && reg < 0x0214) pat_m[reg - 0x210] = v & 0xF;
		else if (reg == 0x021A) {  // checker snapshot (b0) / discard (b1)
			for (Checker *c : {&hc[0], &hc[1], &hc[2], &hc[3], &hc[4], &hc[5], &hc[6], &hc[7],
					   &mc[0], &mc[1], &mc[2], &mc[3]}) {
				if (v & 1) {
					c->snap_err = (uint32_t)std::min<uint64_t>(c->acc_err, 0xFFFFFFFFu);
					c->snap_bits16 = (uint32_t)std::min<uint64_t>(c->acc_bits >> 16, 0xFFFFFFFFu);
				}
				if (v & 3) c->acc_err = c->acc_bits = 0;
			}
		}
		return 0;
	}

	void update_fec_probs(DspLane &d)
	{
		if (std::fabs(d.ber - d.cached_ber) <= 0.01 * d.cached_ber) return;
		d.cached_ber = d.ber;
		double ps = 1.0 - std::pow(1.0 - d.ber, 10.0);
		double lam = 544.0 * ps;
		d.p_unc = poisson_tail16(lam);
		d.p_corr = std::max(0.0, 1.0 - std::exp(-lam) - d.p_unc);
	}

	uint64_t poisson(double mean)
	{
		if (mean <= 0) return 0;
		if (mean > 1e5) return (uint64_t)std::max(0.0, mean + std::sqrt(mean) * n01(rng));
		return std::poisson_distribution<uint64_t>(mean)(rng);
	}

	void step_dsp()
	{
		if (!dsp_alive()) {
			if (dsp_started) dsp_reset_state();
			return;
		}
		if (dsp_started && !dsp_ready && t - dsp_start_t >= 120) dsp_ready = true;
		if (!dsp_ready || dsp_hang) return;  // a hung DSP freezes everything
		dsp_hb++;
		for (int l = 0; l < 4; l++) {
			DspLane &d = dl[l];
			double oma = kOmaPerAvg * rx_mw[l];
			double snr = oma > 0 ? 1.0 / (std::pow(kNoiseOmaMw / oma, 2) + 1.0 / kSnrCeilingLin) : 0.0;
			bool rx_on = d.lctrl & 2;
			if (rx_on && !d.ffe) snr /= 4.0;  // unequalized
			d.snr_db = snr > 0 ? 10.0 * std::log10(snr) : 0.0;
			d.ber = std::min(0.375, 0.375 * std::erfc(std::sqrt(snr / 10.0)));

			// acquisition: FFE adapts first (30 ms), CDR declares lock after (50 ms)
			bool cond = rx_on && d.snr_db > 10.0 && mw_to_dbm(rx_mw[l]) > -18.0;
			if (!cond) { d.media_lock = d.ffe = false; d.cond_since = t; }
			else {
				if (!d.ffe && t - d.cond_since >= 30) d.ffe = true;
				if (d.ffe && !d.media_lock && t - d.cond_since >= 50) d.media_lock = true;
			}


			if (d.media_lock) {
				d.acc_bits += (uint64_t)kLineBitsPerMs;
				d.acc_err += poisson(d.ber * kLineBitsPerMs);
				update_fec_probs(d);
				double cw = kLineBitsPerMs / kCodewordBits;
				d.acc_fu += poisson(cw * d.p_unc);
				d.acc_fc += poisson(cw * d.p_corr);
			}
		}
		step_host();
		step_checkers();
	}

	// host lane: residual ISI after the CTLE costs SNR, peaking adds noise
	void step_host()
	{
		for (int i = 0; i < 8; i++) {
			HostLane &h = hl[i];
			bool sig = (dl[i / 2].lctrl & 1) && !h.los;
			bool adaptive = h.ctrl & 1;
			if (!sig) {
				h.lock = h.adapt_done = false;
				h.adapted_for = -1;
				h.since = h.adapt_since = t;
				h.snr_db = 0;
				h.ber = 0.375;
				continue;
			}
			if (adaptive) {
				if (std::fabs(h.loss_db - h.adapted_for) > 0.5) {  // (re)adapt
					h.adapted_for = h.loss_db;
					h.adapt_done = false;
					h.adapt_since = t;
				}
				if (!h.adapt_done && t - h.adapt_since >= 25) {
					h.adapt_done = true;
					h.eq_db = std::clamp((int)std::lround(h.loss_db), 0, kMaxCtleDb);
				}
				if (!h.adapt_done) h.eq_db = 0;
			} else {
				h.adapt_done = false;
				h.eq_db = std::min<int>((h.ctrl >> 4) & 0xF, kMaxCtleDb);
			}
			h.snr_db = kHostSnrCeilDb - kIsiDbPerDb * std::fabs(h.loss_db - h.eq_db) - kNoiseEnhDbPerDb * h.eq_db;
			h.ber = pam4_ber(h.snr_db);
			bool cond = h.snr_db >= kHostLockSnrDb && (!adaptive || h.adapt_done);
			if (!cond) { h.lock = false; h.since = t; }
			else if (!h.lock && t - h.since >= 10) h.lock = true;
		}
	}

	void count(Checker &c, bool lock, double ber, double bits)
	{
		c.lock = lock;
		if (!lock) return;
		c.acc_bits += (uint64_t)bits;
		c.acc_err += poisson(ber * bits);
	}

	// PRBS checkers. Output loopbacks turn the module's own generator back
	// into its checker inside the DSP (no channel, no optics).
	void step_checkers()
	{
		for (int i = 0; i < 8; i++) {
			if (!(hchk & (1u << i))) { hc[i].lock = false; continue; }
			if (lb_ho & (1u << i)) count(hc[i], hgen & (1u << i), 0.0, kHostBitsPerMs);
			else count(hc[i], hl[i].lock && host_prbs == pat_h[i], hl[i].ber, kHostBitsPerMs);
		}
		for (int m = 0; m < 4; m++) {
			if (!(mchk & (1u << m))) { mc[m].lock = false; continue; }
			if (lb_m & (1u << m)) { count(mc[m], mgen & (1u << m), 0.0, kLineBitsPerMs); continue; }
			int src = loopback ? ((mgen & (1u << m)) ? pat_m[m] : -1) : far_prbs;
			count(mc[m], dl[m].media_lock && src == pat_m[m], dl[m].ber, kLineBitsPerMs);
		}
	}

	// -------------------------------------------------------------- ADC
	int adc(uint8_t ch, uint16_t *raw)
	{
		double v = 0, counts = -1;
		auto temp_to_v = [](double c) { return 0.5 + 0.01 * c; };
		switch (ch) {
		case OMT_ADC_TEMP: v = temp_to_v(mod_c + temp_sensor_off) + 0.0004 * n01(rng); break;
		case OMT_ADC_VCC: v = vcc / 2.0 * (1.0 + vcc_gain_err) + 0.0002 * n01(rng); break;
		case OMT_ADC_LASER_TEMP: v = temp_to_v(laser_c + thermistor_off) + 0.0002 * n01(rng); break;
		case OMT_ADC_TEC_I: v = 1.25 + 0.5 * tec_current() + 0.0005 * n01(rng); break;
		case OMT_ADC_RSSI0: case OMT_ADC_RSSI1: case OMT_ADC_RSSI2: case OMT_ADC_RSSI3: {
			int l = ch - OMT_ADC_RSSI0;
			double tc = 1.0 + 0.0005 * (mod_c - 25.0);  // responsivity tempco
			counts = rssi_dark[l] + rx_mw[l] * rssi_gain[l] * tc + 3.0 * n01(rng);
			break;
		}
		default: return -1;
		}
		if (counts < 0) counts = v / 2.5 * 65535.0;
		*raw = (uint16_t)std::clamp(std::lround(counts), 0L, 65535L);
		return 0;
	}

	// ---------------------------------------------------------- physics
	double tec_current() const
	{
		return (out[OMT_PIN_TEC_EN] && !tec_open) ? 1.5 * tec_u : 0.0;
	}

	void step_physics(double dt)
	{
		// lasers
		double laser_diss = 0;
		for (int l = 0; l < 4; l++) {
			Laser &L = las[l];
			bool on = out[OMT_PIN_TX_PWR] && (ld_ctrl & (1u << l));
			if (on && L.drv_fault_inject) {
				ld_status |= (uint16_t)(1u << l);
				ld_ctrl &= (uint16_t)~(0x11u << l);
				on = false;
			}
			double I = on ? bias_ma(l) : 0.0;
			double dT = laser_c - 25.0;
			double ith = L.ith0_ma * std::exp(dT / 60.0) * (1.0 + 1.5 * L.aging);
			double eta = L.eta0_w_per_a * (1.0 - 0.004 * dT) * (1.0 - 0.4 * std::min(L.aging, 1.5));
			tx_mw[l] = I > ith ? std::max(0.0, eta * (I - ith)) : 0.0;
			bool out_en = ld_ctrl & (0x10u << l);
			fiber_mw[l] = out_en ? tx_mw[l] : tx_mw[l] * 1e-4;
			laser_diss += I * 1.6e-3 - tx_mw[l] * 1e-3;
		}
		// optical input
		for (int l = 0; l < 4; l++) {
			rx_mw[l] = loopback ? fiber_mw[l] * dbm_to_mw(-loop_atten_db)
					    : (rx_ext_dbm[l] < -60 ? 0.0 : dbm_to_mw(rx_ext_dbm[l]));
		}
		// power budget (W)
		double p = 0.9;
		if (out[OMT_PIN_DSP_PWR]) p += dsp_ready ? 4.2 : 1.0;
		if (out[OMT_PIN_TX_PWR]) p += 0.3;
		for (int l = 0; l < 4; l++)
			if (out[OMT_PIN_TX_PWR] && (ld_ctrl & (1u << l))) p += 0.15 + bias_ma(l) * 1.6e-3;
		double itec = tec_current();
		p += itec * itec * 1.2;
		power_w = p;

		// rails: first order, 0.4 ms (laser driver 3.3 V) and 1.5 ms (DSP core 0.75 V)
		rail_tx_v += ((out[OMT_PIN_TX_PWR] ? 3.3 : 0.0) - rail_tx_v) * std::min(1.0, dt / 0.4e-3);
		rail_dsp_v += ((out[OMT_PIN_DSP_PWR] ? 0.75 : 0.0) - rail_dsp_v) * std::min(1.0, dt / 1.5e-3);

		// module thermal mass (tau 20 s, 0.6 K/W to case)
		mod_c += (case_c + 0.6 * power_w - mod_c) * dt / 20.0;
		// laser plate: C = 0.05 J/K, Rth = 40 K/W, TEC pumps heat
		double u = (out[OMT_PIN_TEC_EN] && !tec_open) ? std::clamp(tec_u, -1.0, 1.0) : 0.0;
		double q_tec = u >= 0 ? 1.6 * u - 0.2 * u * u : 2.0 * u;  // >0 removes heat
		double dTl = ((mod_c - laser_c) / 40.0 + laser_diss - q_tec) / 0.05;
		laser_c += dTl * dt;
	}

	// ----------------------------------------------------------- resets
	void power_outputs_off()
	{
		std::fill(std::begin(out), std::end(out), 0);
		ld_ctrl = 0;
		ld_status = 0;
		std::fill(std::begin(ld_bias), std::end(ld_bias), 0);
		tec_u = 0;
		dsp_reset_state();
	}

	void do_reset(uint8_t cause)
	{
		power_outputs_off();
		last_cause = cause;
		resets++;
		if (cause == OMT_RST_WATCHDOG) wdt_resets++;
		fw_running = false;
		fw_hang = false;
		reset_pending = false;
		boot_at = t + kBootMs;
	}

	void boot()
	{
		omt_fw_init(&fw, &hal);
		fw_running = true;
		wdt_last = t;
	}

	void run(uint32_t ms)
	{
		for (uint32_t i = 0; i < ms; i++) {
			t++;
			step_physics(1e-3);
			step_dsp();
			if (reset_hold) continue;
			if (!fw_running) {
				if (t >= boot_at) boot();
				continue;
			}
			if (!fw_hang) omt_fw_tick(&fw);
			if (reset_pending) { do_reset(reset_cause_pending); continue; }
			if (t - wdt_last > kWdtTimeoutMs) do_reset(OMT_RST_WATCHDOG);
		}
	}

	// ---------------------------------------------------------- host bus
	void trace_rec(char kind, uint8_t off, const uint8_t *d, size_t n)
	{
		if (!trace_on) return;
		// TWI at 400 kHz: (address + offset + data) * 9 bits * 2.5 us (+ restart)
		uint64_t start = std::max<uint64_t>(t * 1000, bus_free_us);
		uint64_t dur = (uint64_t)((n + (kind == 'R' ? 3 : 2)) * 9 * 2.5) + 5;
		bus_free_us = start + dur;
		size_t chunk = std::min<size_t>(n, 254);
		uint32_t tu = (uint32_t)start;
		uint8_t hdr[6] = {(uint8_t)tu, (uint8_t)(tu >> 8), (uint8_t)(tu >> 16), (uint8_t)(tu >> 24),
				  (uint8_t)kind, (uint8_t)(chunk + 1)};
		trace.insert(trace.end(), hdr, hdr + 6);
		trace.push_back(off);
		trace.insert(trace.end(), d, d + chunk);
		if (trace.size() > (8u << 20)) trace.erase(trace.begin(), trace.begin() + (trace.size() / 2));
	}

	bool i2c_ok() const { return fw_running && !reset_hold; }

	int i2c_write(const uint8_t *d, size_t n)
	{
		if (n == 0) return -1;
		if (!i2c_ok()) { trace_rec('N', d[0], nullptr, 0); return -1; }
		trace_rec('W', d[0], d + 1, n - 1);
		return omt_i2c_write(&fw, d, n);
	}

	int i2c_read(uint8_t off, uint8_t *o, size_t n)
	{
		if (!i2c_ok()) { trace_rec('N', off, nullptr, 0); return -1; }
		uint8_t a = off;
		omt_i2c_write(&fw, &a, 1);  // set address pointer, no data
		int rc = omt_i2c_read(&fw, o, n);
		trace_rec('R', off, o, n);
		return rc;
	}

	// ------------------------------------------------ factory programming
	void program_default_image()
	{
		uint8_t img[1024];
		omt_tunables tun;
		img_default_tunables(&tun);
		size_t n = omt_img_build(img, sizeof(img), 1, 0, 100, 0, "factory", &tun);
		for (size_t i = 0; i < n; i++) flash[OMT_NVM_IMAGE_A + i] &= img[i];
	}

	void program_golden_cal(uint32_t seed)
	{
		omt_calib c;
		nvm_default_calibration(&c);
		c.temp_offset_c = (float)-temp_sensor_off;
		c.vcc_gain = (float)(1.0 / (1.0 + vcc_gain_err));
		for (int l = 0; l < 4; l++) {
			c.rssi_slope[l] = (float)(1000.0 / rssi_gain[l]);
			c.rssi_offset[l] = (float)rssi_dark[l];
			c.mpd_slope[l] = (float)(1000.0 / las[l].mpd_counts_per_mw);
		}
		c.laser_temp_offset_c = (float)-thermistor_off;
		char sn[17];
		std::snprintf(sn, sizeof sn, "OMT26%07u", seed % 10000000u);
		std::memcpy(c.serial, sn, 16);
		std::memcpy(c.date_code, "261031  ", 8);
		c.crc = omt_crc32(0, &c, offsetof(omt_calib, crc));
		std::memcpy(&flash[OMT_NVM_CAL], &c, sizeof(c));
	}

	// ------------------------------------------------------ parameters
	int set(const std::string &name, double v)
	{
		auto lane = [&](const char *prefix) -> int {
			size_t pl = std::strlen(prefix);
			if (name.compare(0, pl, prefix) != 0 || name.size() != pl + 1) return -1;
			int l = name[pl] - '0';
			return (l >= 0 && l < 4) ? l : -1;
		};
		auto lane8 = [&](const char *prefix) -> int {
			size_t pl = std::strlen(prefix);
			if (name.compare(0, pl, prefix) != 0 || name.size() != pl + 1) return -1;
			int k = name[pl] - '0';
			return (k >= 0 && k < 8) ? k : -1;
		};
		int l;
		if (name == "case_temp") case_c = v;
		else if (name == "module_temp_now") { case_c = v; mod_c = v; laser_c = v; }
		else if (name == "vcc") vcc = v;
		else if (name == "lpmode") lpmode = v != 0;
		else if (name == "resetl") {
			int nv = v != 0;
			if (resetl && !nv) { reset_hold = true; power_outputs_off(); fw_running = false; }
			if (!resetl && nv && powered) { reset_hold = false; do_reset(OMT_RST_HOST_PIN); }
			resetl = nv;
		} else if (name == "loopback") loopback = v != 0;
		else if (name == "loop_atten_db") loop_atten_db = v;
		else if (name == "rx_dbm") std::fill(std::begin(rx_ext_dbm), std::end(rx_ext_dbm), v);
		else if ((l = lane("rx_dbm.")) >= 0) rx_ext_dbm[l] = v;
		else if ((l = lane("aging.")) >= 0) las[l].aging = v;
		else if ((l = lane("ld_fault.")) >= 0) las[l].drv_fault_inject = v != 0;
		else if ((l = lane8("host_los.")) >= 0) hl[l].los = v != 0;
		else if ((l = lane8("host_loss_db.")) >= 0) hl[l].loss_db = v;
		else if (name == "host_loss_db") for (auto &h : hl) h.loss_db = v;
		else if (name == "host_prbs") host_prbs = (int)v;
		else if (name == "far_prbs") far_prbs = (int)v;
		else if (name == "tec_open") tec_open = v != 0;
		else if (name == "dsp_hang") dsp_hang = v != 0;
		else if (name == "dsp_hang_sticky") { dsp_hang_sticky = v != 0; dsp_hang = dsp_hang || dsp_hang_sticky; }
		else if (name == "mdio_fail_p") mdio_fail_p = v;
		else if (name == "fw_hang") fw_hang = v != 0;
		else if (name == "trace") trace_on = v != 0;
		else if (name == "power_cycle") { power_outputs_off(); do_reset(OMT_RST_POWER_ON); }
		else if (name == "power") {  // supply off: MCU unpowered; on: power-on reset
			bool on = v != 0;
			if (!on && powered) { powered = false; reset_hold = true; power_outputs_off(); fw_running = false; }
			if (on && !powered) { powered = true; reset_hold = !resetl; do_reset(OMT_RST_POWER_ON); }
		}
		else return -1;
		return 0;
	}

	double get(const std::string &name)
	{
		auto lane = [&](const char *prefix) -> int {
			size_t pl = std::strlen(prefix);
			if (name.compare(0, pl, prefix) != 0 || name.size() != pl + 1) return -1;
			int l = name[pl] - '0';
			return (l >= 0 && l < 4) ? l : -1;
		};
		auto lane8 = [&](const char *prefix) -> int {
			size_t pl = std::strlen(prefix);
			if (name.compare(0, pl, prefix) != 0 || name.size() != pl + 1) return -1;
			int k = name[pl] - '0';
			return (k >= 0 && k < 8) ? k : -1;
		};
		int l;
		if (name == "time_ms") return (double)t;
		if (name == "case_temp") return case_c;
		if (name == "module_temp") return mod_c;
		if (name == "laser_temp") return laser_c;
		if (name == "vcc") return vcc;
		if (name == "tec_current") return tec_current();
		if (name == "power_w") return power_w;
		if (name == "intl") return fw_running ? out[OMT_PIN_INTL] : 1;
		if (name == "powered") return powered;
		if (name == "rail_tx_v") return rail_tx_v;
		if (name == "rail_dsp_v") return rail_dsp_v;
		if (name == "pin.tx_pwr") return out[OMT_PIN_TX_PWR];
		if (name == "pin.dsp_pwr") return out[OMT_PIN_DSP_PWR];
		if (name == "pin.dsp_rstn") return out[OMT_PIN_DSP_RSTN];
		if (name == "pin.tec_en") return out[OMT_PIN_TEC_EN];
		if (name == "pin.intl") return fw_running ? out[OMT_PIN_INTL] : 1;
		if (name == "pin.lpmode") return lpmode;
		if (name == "pin.resetl") return resetl;
		if (name == "resets") return resets;
		if (name == "wdt_resets") return wdt_resets;
		if (name == "flash_erases") return flash_erases;
		if (name == "fw_running") return fw_running;
		if (name == "module_state") return fw_running ? fw.mstate : 0;
		if (name == "dsp_ready") return dsp_ready && !dsp_hang;
		if (name == "irq_depth") return irq_depth;
		if ((l = lane("tx_dbm.")) >= 0) return mw_to_dbm(fiber_mw[l]);
		if ((l = lane("tx_mw.")) >= 0) return fiber_mw[l];
		if ((l = lane("laser_mw.")) >= 0) return tx_mw[l];
		if ((l = lane("bias_ma.")) >= 0) return (ld_ctrl & (1u << l)) ? bias_ma(l) : 0.0;
		if ((l = lane("rx_dbm.")) >= 0) return mw_to_dbm(rx_mw[l]);
		if ((l = lane("rx_mw.")) >= 0) return rx_mw[l];
		if ((l = lane("snr_db.")) >= 0) return dl[l].snr_db;
		if ((l = lane("ber.")) >= 0) return dl[l].ber;
		if ((l = lane("media_lock.")) >= 0) return dl[l].media_lock;
		if ((l = lane8("host_lock.")) >= 0) return hl[l].lock;
		if ((l = lane8("host_snr_db.")) >= 0) return hl[l].snr_db;
		if ((l = lane8("host_ber.")) >= 0) return hl[l].ber;
		if ((l = lane8("host_eq_db.")) >= 0) return hl[l].eq_db;
		if ((l = lane8("host_loss_db.")) >= 0) return hl[l].loss_db;
		if ((l = lane8("host_chk_lock.")) >= 0) return hc[l].lock;
		if ((l = lane("media_chk_lock.")) >= 0) return mc[l].lock;
		if (name == "lb_media") return lb_m;
		if (name == "lb_host_out") return lb_ho;
		if (name == "lb_host_in") return lb_hi;
		return NAN;
	}
};

// ===================================================================== C API
extern "C" {

omt_sim *omt_sim_create(uint32_t seed, uint32_t flags)
{
	auto *s = new omt_sim(seed);
	if (!(flags & OMT_SIM_BLANK_FLASH)) s->program_default_image();
	if (flags & OMT_SIM_GOLDEN_CAL) s->program_golden_cal(seed);
	s->do_reset(OMT_RST_POWER_ON);
	s->resets = 0;
	return s;
}

void omt_sim_destroy(omt_sim *s) { delete s; }
void omt_sim_run(omt_sim *s, uint32_t ms) { s->run(ms); }
uint64_t omt_sim_time_ms(const omt_sim *s) { return s->t; }
int omt_sim_i2c_write(omt_sim *s, const uint8_t *d, size_t n) { return s->i2c_write(d, n); }
int omt_sim_i2c_read(omt_sim *s, uint8_t off, uint8_t *o, size_t n) { return s->i2c_read(off, o, n); }
int omt_sim_set(omt_sim *s, const char *name, double v) { return s->set(name, v); }
double omt_sim_get(omt_sim *s, const char *name) { return s->get(name); }

size_t omt_sim_console(omt_sim *s, const char *line, char *out, size_t cap)
{
	s->uart.clear();
	if (s->fw_running) {
		std::string l(line);
		l += "\n";
		omt_console_rx(&s->fw, l.data(), l.size());
	}
	s->run(20);
	size_t n = std::min(cap ? cap - 1 : 0, s->uart.size());
	std::memcpy(out, s->uart.data(), n);
	if (cap) out[n] = 0;
	return n;
}

size_t omt_sim_trace_take(omt_sim *s, uint8_t *buf, size_t cap)
{
	size_t n = std::min(cap, s->trace.size());
	std::memcpy(buf, s->trace.data(), n);
	s->trace.erase(s->trace.begin(), s->trace.begin() + n);
	return n;
}

size_t omt_sim_flash_read(omt_sim *s, uint32_t addr, uint8_t *buf, size_t n)
{
	if (addr >= s->flash.size()) return 0;
	n = std::min(n, s->flash.size() - addr);
	std::memcpy(buf, &s->flash[addr], n);
	return n;
}

size_t omt_sim_build_image(uint8_t *buf, size_t cap, uint8_t major, uint8_t minor, uint16_t build,
			   uint32_t flags, const char *tag)
{
	omt_tunables tun;
	img_default_tunables(&tun);
	return omt_img_build(buf, cap, major, minor, build, flags, tag, &tun);
}

}  // extern "C"
