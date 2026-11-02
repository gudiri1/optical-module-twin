# ADR 0004: Latched flags are edge-triggered by default

**Context.** CMIS flags are latched and clear-on-read. Whether a flag must be
set again while its condition persists after a read could not be confirmed
from accessible sources (finding F-004).

**Decision.** Default `OMT_FLAGS_RELATCH=0`: set on a rising edge of the
condition only. `OMT_FLAGS_RELATCH=1` builds level re-latching. Conditions use
hysteresis in both modes.

**Consequences.** No interrupt storm while an alarm persists; hosts must read
the monitor value (or keep their own state) to know a condition is still
active. Revisit against the OIF-CMIS text before release.
