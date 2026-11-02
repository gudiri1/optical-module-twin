# ADR 0003: Run-to-completion tasks instead of blocking threads

**Decision.** Firmware jobs are periodic, non-blocking functions; sequences
(power-up, DSP recovery, CDB) are explicit state machines with timestamps.

**Why.** Deterministic in simulation (bit-exact runs per seed), trivially
mapped to Zephyr work items or a single thread, no stack sizing per job, and
every wait has a visible timeout, which is where bring-up bugs hide.

**Consequences.** Long operations (flash erase) are modeled as busy periods
(CDB status busy) instead of blocking the loop. The watchdog is kicked once
per scheduler pass; a stuck job starves it (tested: "Controller firmware
hang").
