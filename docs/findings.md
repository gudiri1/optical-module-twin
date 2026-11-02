# Findings

Source of truth: `requirements/findings.yaml` (also rendered in the HTML report).

## F-001 - Spurious laser-temperature alarms on every power-up

* **Status:** fixed
* **Found by:** omt lint (L016) on a freshly brought-up module
* **Root cause:** Aux1/Aux2 (laser plate temperature, TEC drive) alarms were evaluated while the TEC was still settling in ModulePwrUp, so low alarms latched on every bring-up.
* **Action:** Evaluate Aux alarms only in ModuleReady (monitor.c). Regression test test_no_spurious_flags_after_bring_up.

## F-002 - TX bias high alarm could never fire

* **Status:** fixed
* **Found by:** DVT fault matrix, laser end-of-life scenario
* **Root cause:** Bias high alarm threshold (110 mA) sat above the APC bias clamp (105 mA); a dying laser saturated silently.
* **Action:** Thresholds 100/90 mA (alarm/warning), consistency test test_thresholds_reachable_before_protection.

## F-003 - Degrading laser hid its own TX alarms

* **Status:** fixed
* **Found by:** DVT fault matrix, laser end-of-life scenario
* **Root cause:** Lane alarms were gated on "APC converged"; a laser that loses convergence because it is failing dropped out of alarm evaluation.
* **Action:** Alarms arm on first convergence after enable and stay armed until the lane is disabled (tx.c, monitor.c).

## F-004 - Latched-flag semantics after clear-on-read are a spec interpretation

* **Status:** open
* **Found by:** DVT temperature sweep (flag read ordering)
* **Root cause:** Whether a latched flag is set again while its condition persists after a read could not be confirmed from accessible sources. Default is edge-triggered (no interrupt storms); OMT_FLAGS_RELATCH=1 builds level-relatch.
* **Action:** Build option + documentation; confirm against the OIF-CMIS text before release.

## F-005 - RX power DDM drifts +0.12 dB from 25 to 79 degC module temperature

* **Status:** accepted
* **Found by:** DVT temperature sweep data
* **Root cause:** Photodiode responsivity temperature coefficient is not compensated; calibration is single-temperature.
* **Action:** Within the +/-1 dB budget today. Option - add a tempco coefficient to the factory record and calibrate at two temperatures.

## F-006 - Debug console printed stale SPI register values

* **Status:** fixed
* **Found by:** pytest test_console_commands
* **Root cause:** omt_printf(fmt, ld_read(..., &v) ? ... , reg, v) - C leaves argument evaluation order unspecified, so v was read before the SPI transfer.
* **Action:** Evaluate the transfer into rc first (console.c).

## F-007 - Twin DSP model is ~0.2 dB optimistic vs. the equalized link

* **Status:** accepted
* **Found by:** dsplab Monte-Carlo (7-tap FFE + MM CDR)
* **Root cause:** Measured BER after FFE is ~1.2-1.3x the closed form at the measured SNR (residual ISI is not Gaussian, LMS misadjustment).
* **Action:** Accepted for system-level simulation; noted where sensitivity margins are quoted.

## P-1 - Review note for Zephyr PR #123842: monitor/id reads clear latched flags

* **Status:** fix prepared
* **Found by:** tests/interop/test_pr_access_pattern.py (driver access pattern replayed against the twin)
* **Root cause:** cmis_module_read_monitors() reads lower memory 0..17 and read_id() reads 0..127 in one transfer. Bytes 8..11 are clear-on-read latched flags, so every call consumes ModuleStateChanged, CdbCmdComplete and Aux flags that the API does not return, and can release IntL before an interrupt handler runs.
* **Action:** Fix prepared for #123842: read_id() reads bytes 0..7 and 85, read_monitors() byte 9 and 14..17; the Zephyr emulator now models clear-on-read and test_reads_keep_unreported_flags fails on the old driver.

## P-2 - Page-select caching breaks with a second host agent

* **Status:** fix prepared
* **Found by:** tests/interop/test_pr_access_pattern.py::test_page_select_cache_hazard
* **Root cause:** A host that caches the page select reads the wrong page after another agent (for example the Zephyr `cmis info` shell command) selects Page 00h. The twin reproduces phantom RX LOL flags from part-number ASCII.
* **Action:** Within one Zephyr image: per-device mutex across select and read in a prepared follow-up PR; test_concurrent_paged_reads fails without it (37 of 50 reads wrong). Across independent host agents the rule stays: one owner per module.

## P-3 - Review note for Zephyr PR #123842: power_class is the encoded value

* **Status:** fix prepared
* **Found by:** interop field comparison
* **Root cause:** struct cmis_id.power_class holds bits 7..5 of byte 200 (0..7); CMIS names the classes 1..8. Callers may display "class 7" for a class 8 module.
* **Action:** Fix prepared as fixup for #123842: class = encoded + 1, as ethtool prints it; test image corrected to class 8 for 14 W.

## P-4 - Tx bias scaling factor direction differs between implementations

* **Status:** open
* **Found by:** Implementing Page 01h byte 160 for the follow-up PR, compared with ethtool cmis.c
* **Root cause:** ethtool right-shifts the raw bias value by the code; the follow-up PR multiplies the 2 uA LSB by x1/x2/x4. The OIF text was not accessible here.
* **Action:** Stated as an open question in the follow-up PR description; one-line change plus tests either way.
