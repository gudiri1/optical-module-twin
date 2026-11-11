# SPDX-License-Identifier: Apache-2.0
"""Lab instrument control over SCPI.

Drivers speak SCPI only, through a transport:

  VisaTransport    pyvisa resource string ("USB0::...", "TCPIP0::...::INSTR"),
                   imported lazily so the rest of the package needs no VISA
  SocketTransport  raw SCPI over TCP (port 5025 on most LXI instruments)
  SimTransport     an in-process simulated instrument bound to the twin

so a DVT procedure written against a :class:`Bench` runs unchanged on the
simulated bench and on a real one described by a YAML file. Every SCPI
transaction is logged with a timestamp for the test record.

The simulated instruments implement the small SCPI subset the drivers use
(command forms follow common bench instruments; check your instrument's
programming guide before pointing a driver at it).
"""
from __future__ import annotations

import csv
import socket
import socketserver
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from . import cmis


class ScpiError(RuntimeError):
    pass


# ------------------------------------------------------------------ transports

class SocketTransport:
    """Raw SCPI over TCP, newline-terminated."""

    def __init__(self, host: str, port: int = 5025, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    def write(self, cmd: str) -> None:
        self.sock.sendall(cmd.encode() + b"\n")

    def query(self, cmd: str) -> str:
        self.write(cmd)
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ScpiError("connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode().strip()

    def close(self) -> None:
        self.sock.close()


class VisaTransport:
    def __init__(self, resource: str, timeout_ms: int = 5000):
        import pyvisa  # optional dependency, only for real benches
        self.inst = pyvisa.ResourceManager().open_resource(resource)
        self.inst.timeout = timeout_ms
        self.inst.read_termination = "\n"
        self.inst.write_termination = "\n"

    def write(self, cmd: str) -> None:
        self.inst.write(cmd)

    def query(self, cmd: str) -> str:
        return self.inst.query(cmd).strip()

    def close(self) -> None:
        self.inst.close()


class SimTransport:
    def __init__(self, sim: "SimInstrument"):
        self.sim = sim

    def write(self, cmd: str) -> None:
        self.sim.handle(cmd)

    def query(self, cmd: str) -> str:
        r = self.sim.handle(cmd)
        if r is None:
            raise ScpiError(f"no response to {cmd!r}")
        return r

    def close(self) -> None:
        pass


def open_transport(spec: str):
    """'socket://host[:port]' or a VISA resource string."""
    if spec.startswith("socket://"):
        hp = spec[len("socket://"):]
        host, _, port = hp.partition(":")
        return SocketTransport(host, int(port or 5025))
    return VisaTransport(spec)


# ------------------------------------------------------------------ drivers

@dataclass
class LogEntry:
    t: float
    instrument: str
    command: str
    response: str | None


class Instrument:
    def __init__(self, transport, name: str, log: list[LogEntry] | None = None,
                 clock: Callable[[], float] = time.monotonic):
        self.t = transport
        self.name = name
        self.log = log if log is not None else []
        self.clock = clock

    def write(self, cmd: str) -> None:
        self.t.write(cmd)
        self.log.append(LogEntry(self.clock(), self.name, cmd, None))

    def query(self, cmd: str) -> str:
        r = self.t.query(cmd)
        self.log.append(LogEntry(self.clock(), self.name, cmd, r))
        return r

    def qf(self, cmd: str) -> float:
        return float(self.query(cmd))

    def idn(self) -> str:
        return self.query("*IDN?")

    def reset(self) -> None:
        self.write("*RST")

    def errors(self) -> list[str]:
        out = []
        for _ in range(20):
            e = self.query("SYST:ERR?")
            if e.startswith("0") or e.startswith("+0"):
                break
            out.append(e)
        return out


class PowerSupply(Instrument):
    def set_voltage(self, v: float) -> None:
        self.write(f"VOLT {v:.4f}")

    def set_current_limit(self, a: float) -> None:
        self.write(f"CURR {a:.3f}")

    def output(self, on: bool) -> None:
        self.write(f"OUTP {'ON' if on else 'OFF'}")

    def voltage(self) -> float:
        return self.qf("MEAS:VOLT?")

    def current(self) -> float:
        return self.qf("MEAS:CURR?")

    def power_w(self) -> float:
        return self.voltage() * self.current()


class ThermalForcer(Instrument):
    """Thermal stream / chamber controlling the module case temperature."""

    def set_temp(self, c: float) -> None:
        self.write(f"TEMP {c:.2f}")

    def temp(self) -> float:
        return self.qf("TEMP?")


class OpticalAttenuator(Instrument):
    """Calibrated source + VOA feeding all four receive lanes (MPO fan-in)."""

    def set_att(self, db: float) -> None:
        self.write(f"INP:ATT {db:.3f}")

    def output_dbm(self) -> float:
        return self.qf("OUTP:POW?")

    def set_output_dbm(self, dbm: float) -> None:
        """Power-control mode: attenuation follows the calibrated source."""
        self.set_att(self.qf("SOUR:POW?") - dbm)


class PowerMeter(Instrument):
    """Optical power meter behind a 1x4 switch on the transmit fan-out."""

    def set_wavelength(self, nm: float) -> None:
        self.write(f"SENS:POW:WAV {nm:.1f}NM")

    def read_dbm(self, lane: int) -> float:
        self.write(f"ROUT:CHAN {lane}")
        return self.qf("READ:POW?")


class Multimeter(Instrument):
    """DMM with a thermocouple scanner: channel 1 module case, 2 laser plate."""

    def temperature(self, channel: int = 1) -> float:
        return self.qf(f"MEAS:TEMP? TC,K,(@{channel})")

    def vdc(self) -> float:
        return self.qf("MEAS:VOLT:DC?")


class PatternGenerator(Instrument):
    """BERT pattern generator: port 1 drives the host lanes (stands in for
    the switch ASIC), port 2 the optical input via a reference transmitter."""

    def pattern(self, port: int, name: str | None) -> None:
        if name is None:
            self.write(f"OUTP{port} OFF")
        else:
            self.write(f"SOUR{port}:PATT {name}")
            self.write(f"OUTP{port} ON")


class Oscilloscope(Instrument):
    """4-channel scope: single-shot capture on an edge trigger."""

    def setup(self, s_per_div: float, trig_ch: int, level_v: float, slope: str = "POS",
              pretrigger: float = 0.1) -> None:
        self.write(f":TIM:SCAL {s_per_div:g}")
        self.write(f":TIM:PRET {pretrigger:g}")
        self.write(f":TRIG:SOUR CHAN{trig_ch}")
        self.write(f":TRIG:LEV {level_v:g}")
        self.write(f":TRIG:SLOP {slope}")

    def probe(self, ch: int, point: str) -> None:
        """Simulation only: where the probe tip sits (a net name of the twin).
        On a real scope the probe is placed by hand and this is a label."""
        self.write(f":CHAN{ch}:LAB \"{point}\"")

    def single(self) -> None:
        self.write(":SING")

    def triggered(self) -> bool:
        return self.query(":TRIG:STAT?").upper().startswith("STOP")

    def waveform(self, ch: int) -> tuple[float, list[float]]:
        """(seconds per sample, samples in volts); t = 0 at the trigger."""
        self.write(f":WAV:SOUR CHAN{ch}")
        dt = self.qf(":WAV:XINC?")
        return dt, [float(x) for x in self.query(":WAV:DATA?").split(",")]

    def pretrigger_s(self) -> float:
        return self.qf(":WAV:XOR?")


# ------------------------------------------------------------------ simulation

class SimInstrument:
    """Minimal SCPI parser; subclasses map commands onto the twin."""

    ident = "OMT,SIM,0,1.0"

    def __init__(self, m):
        self.m = m
        self.err: list[str] = []

    def handle(self, line: str) -> str | None:
        cmd, _, arg = line.strip().partition(" ")
        cmd = cmd.upper()
        if cmd == "*IDN?":
            return self.ident
        if cmd == "*RST":
            self.rst()
            return None
        if cmd == "SYST:ERR?":
            return self.err.pop(0) if self.err else '+0,"No error"'
        r = self.cmd(cmd, arg.strip())
        if r is NotImplemented:
            self.err.append(f'-113,"Undefined header: {cmd}"')
            return None
        return r

    def rst(self) -> None:
        pass

    def cmd(self, cmd: str, arg: str):
        return NotImplemented


class SimSupply(SimInstrument):
    ident = "OMT,SIM-PSU,0,1.0"

    def __init__(self, m):
        super().__init__(m)
        self.v = m.get("vcc")
        self.on = True

    def cmd(self, cmd, arg):
        if cmd == "VOLT":
            self.v = float(arg)
            if self.on:
                self.m.set("vcc", self.v)
        elif cmd == "CURR":
            pass
        elif cmd == "OUTP":
            on = arg.upper() in ("ON", "1")
            if on and not self.on:
                self.m.set("vcc", self.v)
                self.m.set("power", 1)          # power-on reset
            elif not on and self.on:
                self.m.set("power", 0)          # controller unpowered
                self.m.set("vcc", 0.0)
            self.on = on
        elif cmd == "MEAS:VOLT?":
            return f"{self.m.get('vcc'):.5f}"
        elif cmd == "MEAS:CURR?":
            v = self.m.get("vcc")
            return f"{self.m.get('power_w') / v if v > 0.5 else 0.0:.5f}"
        else:
            return NotImplemented
        return None


class SimForcer(SimInstrument):
    ident = "OMT,SIM-THERMOSTREAM,0,1.0"

    def cmd(self, cmd, arg):
        if cmd == "TEMP":
            self.m.set("case_temp", float(arg))
        elif cmd == "TEMP?":
            return f"{self.m.get('case_temp'):.2f}"
        else:
            return NotImplemented
        return None


class SimVoa(SimInstrument):
    ident = "OMT,SIM-VOA,0,1.0"
    SOURCE_DBM = 0.0          # calibrated reference transmitter at the VOA input

    def __init__(self, m):
        super().__init__(m)
        self.att = self.SOURCE_DBM - m.get("rx_dbm.0")

    def cmd(self, cmd, arg):
        if cmd == "INP:ATT":
            self.att = float(arg)
            self.m.set("rx_dbm", self.SOURCE_DBM - self.att)
        elif cmd == "INP:ATT?":
            return f"{self.att:.3f}"
        elif cmd == "SOUR:POW?":
            return f"{self.SOURCE_DBM:.3f}"
        elif cmd == "OUTP:POW?":
            return f"{self.SOURCE_DBM - self.att:.3f}"
        else:
            return NotImplemented
        return None


class SimPowerMeter(SimInstrument):
    ident = "OMT,SIM-OPM,0,1.0"

    def __init__(self, m):
        super().__init__(m)
        self.ch = 1
        self.wl = 1311.0

    def cmd(self, cmd, arg):
        if cmd == "ROUT:CHAN":
            self.ch = int(arg)
        elif cmd == "SENS:POW:WAV":
            self.wl = float(arg.upper().rstrip("NM"))
        elif cmd == "READ:POW?":
            return f"{self.m.get(f'tx_dbm.{self.ch - 1}'):.4f}"
        else:
            return NotImplemented
        return None


class SimDmm(SimInstrument):
    ident = "OMT,SIM-DMM,0,1.0"

    def cmd(self, cmd, arg):
        if cmd == "MEAS:TEMP?":
            ch = int(arg.rsplit("@", 1)[-1].rstrip(")")) if "@" in arg else 1
            return f"{self.m.get('module_temp' if ch == 1 else 'laser_temp'):.4f}"
        if cmd == "MEAS:VOLT:DC?":
            return f"{self.m.get('vcc'):.5f}"
        return NotImplemented


class SimBert(SimInstrument):
    ident = "OMT,SIM-BERT,0,1.0"

    def __init__(self, m):
        super().__init__(m)
        self.pat = {1: "PRBS31Q", 2: "PRBS31Q"}

    def cmd(self, cmd, arg):
        for port, param in ((1, "host_prbs"), (2, "far_prbs")):
            if cmd == f"SOUR{port}:PATT":
                self.pat[port] = arg.upper()
                return None
            if cmd == f"OUTP{port}":
                on = arg.upper() in ("ON", "1")
                self.m.set(param, cmis.PATTERN_CODES[self.pat[port]] if on else -1)
                return None
        return NotImplemented


class SimScope(SimInstrument):
    """Samples twin nets once per virtual millisecond while armed (the twin's
    time step, so edges are resolved to 1 ms). Logic nets read 0 / 3.3 V."""
    ident = "OMT,SIM-SCOPE,0,1.0"
    LOGIC_V = 3.3

    def __init__(self, m):
        super().__init__(m)
        self.label = {1: "rail_tx_v", 2: "rail_dsp_v", 3: "pin.dsp_rstn", 4: "pin.intl"}
        self.scale, self.pret, self.src, self.lev, self.slope = 0.1, 0.1, 1, 1.65, "POS"
        self.armed = False
        self.done = False
        self.buf: list[list[float]] = []
        self.trig_i: int | None = None
        self.wav_ch = 1

    @property
    def n(self) -> int:
        return int(round(self.scale * 10 * 1000))       # 10 divisions, 1 ms per sample

    def value(self, ch: int) -> float:
        net = self.label[ch]
        v = self.m.get(net)
        return v * self.LOGIC_V if net.startswith("pin.") else v

    def sample(self) -> None:
        if not self.armed:
            return
        row = [self.value(c) for c in (1, 2, 3, 4)]
        pre = int(self.n * self.pret)
        if self.trig_i is None:
            self.buf.append(row)
            self.buf = self.buf[-max(pre, 1) - 1:]
            if len(self.buf) >= 2:
                a, b = self.buf[-2][self.src - 1], self.buf[-1][self.src - 1]
                hit = a < self.lev <= b if self.slope == "POS" else a > self.lev >= b
                if hit:
                    self.buf = self.buf[-pre - 1:] if pre else self.buf[-1:]
                    self.trig_i = len(self.buf) - 1
        else:
            self.buf.append(row)
            if len(self.buf) >= self.n:
                self.armed, self.done = False, True

    def cmd(self, cmd, arg):
        if cmd == ":TIM:SCAL":
            self.scale = float(arg)
        elif cmd == ":TIM:PRET":
            self.pret = float(arg)
        elif cmd == ":TRIG:SOUR":
            self.src = int(arg.upper().replace("CHAN", ""))
        elif cmd == ":TRIG:LEV":
            self.lev = float(arg)
        elif cmd == ":TRIG:SLOP":
            self.slope = arg.upper()[:3]
        elif cmd.startswith(":CHAN") and cmd.endswith(":LAB"):
            self.label[int(cmd[5:-4])] = arg.strip('"')
        elif cmd == ":SING":
            self.armed, self.done, self.buf, self.trig_i = True, False, [], None
        elif cmd == ":TRIG:STAT?":
            return "STOP" if self.done else ("WAIT" if self.armed else "STOP")
        elif cmd == ":WAV:SOUR":
            self.wav_ch = int(arg.upper().replace("CHAN", ""))
        elif cmd == ":WAV:XINC?":
            return "0.001"
        elif cmd == ":WAV:XOR?":
            return f"{-(self.trig_i or 0) * 0.001:g}"
        elif cmd == ":WAV:DATA?":
            return ",".join(f"{r[self.wav_ch - 1]:.4f}" for r in self.buf)
        else:
            return NotImplemented
        return None


# ------------------------------------------------------------------ bench

ROLES = {"supply": (PowerSupply, SimSupply), "forcer": (ThermalForcer, SimForcer),
         "voa": (OpticalAttenuator, SimVoa), "pm": (PowerMeter, SimPowerMeter),
         "dmm": (Multimeter, SimDmm), "bert": (PatternGenerator, SimBert),
         "scope": (Oscilloscope, SimScope)}


@dataclass
class Bench:
    supply: PowerSupply
    forcer: ThermalForcer
    voa: OpticalAttenuator
    pm: PowerMeter
    dmm: Multimeter
    bert: PatternGenerator
    scope: Oscilloscope
    wait_s: Callable[[float], None]
    log: list[LogEntry] = field(default_factory=list)

    @classmethod
    def simulated(cls, m) -> "Bench":
        """Simulated bench on one twin module; time advances with the twin."""
        log: list[LogEntry] = []
        clock = lambda: m.time_ms / 1000.0  # noqa: E731
        sims = {role: sim(m) for role, (_, sim) in ROLES.items()}
        inst = {role: drv(SimTransport(sims[role]), role, log, clock) for role, (drv, _) in ROLES.items()}
        scope: SimScope = sims["scope"]

        def wait_s(s: float) -> None:
            ms = int(round(s * 1000))
            if not scope.armed:
                m.run(ms)
                return
            for _ in range(ms):          # an armed scope samples every millisecond
                m.run(1)
                scope.sample()
        return cls(**inst, wait_s=wait_s, log=log)

    @classmethod
    def from_yaml(cls, path: str | Path) -> "Bench":
        """Real bench: {role: resource} with resource 'socket://host:port' or VISA."""
        import yaml
        cfg = yaml.safe_load(Path(path).read_text())
        log: list[LogEntry] = []
        inst = {role: drv(open_transport(cfg[role]), role, log) for role, (drv, _) in ROLES.items()}
        return cls(**inst, wait_s=time.sleep, log=log)

    def instruments(self) -> dict[str, Instrument]:
        return {role: getattr(self, role) for role in ROLES}

    def write_log(self, path: str | Path) -> None:
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "instrument", "command", "response"])
            for e in self.log:
                w.writerow([f"{e.t:.3f}", e.instrument, e.command, "" if e.response is None else e.response])


# ------------------------------------------------------------------ SCPI server

class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for raw in self.rfile:
            line = raw.decode().strip()
            if not line:
                continue
            with self.server.lock:  # the twin is not thread safe
                r = self.server.sim.handle(line)
            if r is not None:
                self.wfile.write(r.encode() + b"\n")


def serve(sim: SimInstrument, host: str = "127.0.0.1", port: int = 0):
    """Expose a simulated instrument on a TCP SCPI port (for testing a
    SocketTransport, or for lab software that expects a networked bench).
    Returns (server, port); stop with server.shutdown()."""
    srv = socketserver.ThreadingTCPServer((host, port), _Handler)
    srv.daemon_threads = True
    srv.sim = sim
    srv.lock = threading.Lock()
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]
