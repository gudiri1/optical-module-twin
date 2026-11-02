# ADR 0001: Build the module side, not another host stack

**Context.** Zephyr PR #123842 adds host-side SFF-8472/CMIS decoding. The
target role develops firmware *inside* 100G+ optical modules.

**Decision.** Implement the module controller firmware and simulate the
module hardware. Treat the PR as an external peer: replay its access pattern
and (opt-in) compare decoders, never modify or vendor it.

**Consequences.** No overlap with the PR whether or not it is merged. The twin
doubles as a test peer for host stacks (Zephyr, Linux ethtool users, SONiC-like
managers) and produced three review notes for the PR (P-1..P-3).
