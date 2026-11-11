# SPDX-License-Identifier: Apache-2.0
"""Turn the module's host-bus trace into a logic-analyzer view.

Produces a Value Change Dump (open in PulseView or GTKWave; PulseView's I2C
decoder reproduces the transactions) plus a human-readable listing.
TWI timing: 400 kHz, 8-bit address 0xA0/0xA1 (7-bit 0x50).
"""
from __future__ import annotations

HALF_NS = 1250  # half bit period at 400 kHz


def write_waveforms(path: str, dt_s: float, waves: dict[str, list[float]], t0_index: int = 0) -> None:
    """Scope capture to VCD: one `real` variable per channel (GTKWave shows
    them as analog traces; PulseView imports them as analog channels).
    VCD time starts at the first sample (it cannot be negative); the
    trigger is at t0_index * dt_s and is marked by a `trigger` wire."""
    ids = {name: chr(33 + i) for i, name in enumerate(waves)}
    n = min(len(v) for v in waves.values())
    lines = ["$timescale 1 us $end", "$scope module omt $end"]
    lines += [f"$var real 64 {ids[k]} {k.replace('.', '_')} $end" for k in waves]
    lines += ["$var wire 1 ~ trigger $end", "$upscope $end", "$enddefinitions $end"]
    last: dict[str, float | None] = {k: None for k in waves}
    for i in range(n):
        ch = [(k, waves[k][i]) for k in waves if waves[k][i] != last[k]]
        if ch or i in (0, t0_index):
            lines.append(f"#{int(round(i * dt_s * 1e6))}")
            if i in (0, t0_index):
                lines.append(f"{1 if i == t0_index else 0}~")
            for k, v in ch:
                lines.append(f"r{v:.6g} {ids[k]}")
                last[k] = v
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


class _Wave:
    def __init__(self):
        self.ev: list[tuple[int, str, str]] = []  # (t_ns, signal, value)
        self.scl = self.sda = "1"

    def set(self, t, sig, v):
        cur = self.scl if sig == "scl" else self.sda
        if cur != v:
            self.ev.append((t, sig, v))
            if sig == "scl":
                self.scl = v
            else:
                self.sda = v

    def start(self, t):
        self.set(t, "sda", "1"); self.set(t, "scl", "1")
        self.set(t + HALF_NS // 2, "sda", "0")
        self.set(t + HALF_NS, "scl", "0")
        return t + HALF_NS

    def bit(self, t, b):
        self.set(t + HALF_NS // 4, "sda", "1" if b else "0")
        self.set(t + HALF_NS, "scl", "1")
        self.set(t + 2 * HALF_NS, "scl", "0")
        return t + 2 * HALF_NS

    def byte(self, t, v, ack=True):
        for i in range(7, -1, -1):
            t = self.bit(t, (v >> i) & 1)
        return self.bit(t, 0 if ack else 1)

    def stop(self, t):
        self.set(t + HALF_NS // 4, "sda", "0")
        self.set(t + HALF_NS, "scl", "1")
        self.set(t + HALF_NS + HALF_NS // 2, "sda", "1")
        return t + 2 * HALF_NS


def to_vcd(records, path: str) -> int:
    w = _Wave()
    for t_us, kind, off, data in records:
        t = max(t_us * 1000, w.ev[-1][0] + HALF_NS if w.ev else 0)
        t = w.start(t)
        if kind == "N":
            t = w.byte(t, 0xA0, ack=False)
        elif kind == "W":
            t = w.byte(t, 0xA0)
            t = w.byte(t, off)
            for b in data:
                t = w.byte(t, b)
        else:
            t = w.byte(t, 0xA0)
            t = w.byte(t, off)
            t = w.start(t)  # repeated start
            t = w.byte(t, 0xA1)
            for i, b in enumerate(data):
                t = w.byte(t, b, ack=i < len(data) - 1)
        w.stop(t)
    with open(path, "w") as fh:
        fh.write("$date optical-module-twin $end\n$timescale 1ns $end\n")
        fh.write("$scope module host_twi $end\n$var wire 1 ! SCL $end\n$var wire 1 \" SDA $end\n")
        fh.write("$upscope $end\n$enddefinitions $end\n#0\n1!\n1\"\n")
        last = -1
        for t, sig, v in sorted(w.ev, key=lambda e: e[0]):
            if t != last:
                fh.write(f"#{t}\n")
                last = t
            fh.write(f"{v}{'!' if sig == 'scl' else chr(34)}\n")
    return len(w.ev)


def listing(records, limit: int | None = None) -> str:
    out = []
    for t_us, kind, off, data in records[:limit]:
        if kind == "N":
            out.append(f"{t_us / 1000:10.3f} ms  NACK  @0x{off:02X}")
        else:
            arrow = "W" if kind == "W" else "R"
            out.append(f"{t_us / 1000:10.3f} ms  {arrow} @0x{off:02X} [{len(data):3d}] "
                       + data[:16].hex(" ") + (" ..." if len(data) > 16 else ""))
    return "\n".join(out)
