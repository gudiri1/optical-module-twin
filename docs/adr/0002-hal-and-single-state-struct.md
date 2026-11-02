# ADR 0002: HAL function table and one state struct

**Decision.** All hardware access goes through `struct omt_hal` (function
pointers + context). All firmware state lives in one `struct omt_fw`.

**Why.** The same C sources run on the twin, on Zephyr and in unit tests with
a fake HAL. A reset is re-initialization of one struct. Many instances can run
in one process (lot statistics). Function-pointer cost is negligible at the
task rates used (≤ 1 kHz).

**Trade-off.** No compile-time inlining of register access; a production port
with tight ISR budgets can still implement the HAL with static inline wrappers.
