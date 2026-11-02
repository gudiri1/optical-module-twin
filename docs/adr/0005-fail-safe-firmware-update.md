# ADR 0005: A/B images with trial boot and confirmation

**Decision.** CDB downloads go to the inactive bank and are CRC-checked at
"Complete". "Run" marks a one-shot trial and resets. The trial confirms itself
after 2 s of healthy operation; a trial that resets before confirming rolls
back to the committed bank. "Commit" makes the running image the default.

**Why.** A module in the field must never brick on a bad image or a power
loss during update; the host keeps control over when a new image becomes
permanent.

**Tested.** Corrupted image rejected, crash-on-boot image rolled back by the
watchdog, uncommitted image gone after a power cycle (`tests/integration/test_cdb.py`).
