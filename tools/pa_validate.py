#!/usr/bin/env python3
"""Inspect or turn off the already-flashed LPF-AUTO PA console; never key RF.

Default --mode off recognizes the LPF-AUTO status reply, requests off, then
checks the reported DISARMED/G48 LOW/G47 LOW/sensing-off state. --mode status
sends only status requests. IMPORTANT: firmware treats USB input during a job
as cancellation, so even status can stop an active scan or burst.

This utility never requests reset, scan, arm, PA/leak transmission or fault
clearing. Opening a USB port can nevertheless reset some boards/drivers; that
can trigger the installed firmware's automatic power-on LPF scan. Use only on
the intended, already-flashed image and with appropriate hardware precautions.
No pin voltage, RF envelope, dummy load, temperature or PA quality is measured.
Old --disconnected/--expiry transmit-negative tests are intentionally removed:
a formerly "unarmed" pa request can now be valid after LPF auto-qualification.
"""

import argparse
import re
import sys
import time
from pathlib import Path

import serial


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
STATUS = re.compile(
    r"PA STATUS: (?P<state>ARMED_AUTO|DISARMED); G48=(?P<g48>[01]) G47=(?P<g47>[01]); "
    r"band=40m source=7075000 Hz; cooldown_ms=(?P<cooldown>\d+); "
    r"shutdown_fault=(?P<fault>none|LATCHED); sensing=(?P<sensing>on|off); "
    r"LPF=(?P<lpf>[^\r\n]+?) mV=(?P<mv>-?\d+)(?=\r?\n)"
)
CRASH = re.compile(r"Guru Meditation|abort\(\)|PA INIT FAILED")


class Validation:
    def __init__(self, port, output, timeout):
        self.port = port
        self.output = output
        self.timeout = timeout
        self.history = ""
        self.recognized = False

    def log(self, message):
        self.emit(f"\nPA VALIDATE: {message}\n")

    def emit(self, text):
        print(text, end="", flush=True)
        self.output.write(text)
        self.output.flush()

    def read(self):
        data = self.port.read(self.port.in_waiting or 1).decode("utf-8", "replace")
        if data:
            self.history = ANSI.sub("", self.history + data)
            self.emit(ANSI.sub("", data))

    def capture(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.read()

    def send(self, command):
        # Keep the allowlist here, not just in the CLI: no alternate mode may
        # accidentally restore the old harness's arm/PA/scan/reset behavior.
        if command not in ("status", "off"):
            raise ValueError("only status/off are permitted")
        offset = len(self.history)
        self.log(f"send {command}")
        self.port.write(command.encode("ascii") + b"\n")
        self.port.flush()
        return offset

    def status(self):
        # If the first status input cancels a busy job, its response is a
        # cancellation, not status. Allow bounded status-only retries once the
        # worker has had a chance to finish its safe cleanup.
        deadline = time.monotonic() + self.timeout
        for attempt in range(3):
            self.capture(0.15)
            offset = self.send("status")
            attempt_deadline = min(deadline, time.monotonic() + self.timeout / 3)
            while time.monotonic() < attempt_deadline:
                match = STATUS.search(self.history[offset:])
                if match:
                    self.recognized = True
                    return match.groupdict()
                self.read()
            if time.monotonic() >= deadline:
                break
            self.log(f"status reply not yet available (attempt {attempt + 1}); "
                     "input may have cancelled an active job")
        raise RuntimeError("no recognized LPF-AUTO status reply; no power-on or TX commands sent")

    def off(self):
        if not self.recognized:
            raise RuntimeError("refusing off workflow until LPF-AUTO firmware is recognized")
        offset = self.send("off")
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            response = self.history[offset:]
            if (re.search(r"PA POWER OFF: G48=0 G47=0;", response) and
                    "PA IDLE:" in response):
                break
            self.read()
        else:
            raise RuntimeError("off requested but POWER OFF/IDLE confirmation was not received")
        state = self.status()
        if any(state[key] != expected for key, expected in (
                ("state", "DISARMED"), ("g48", "0"), ("g47", "0"), ("sensing", "off"))):
            raise RuntimeError(f"off was not confirmed by final status: {state}")
        if "PA PREP:" in self.history[offset:]:
            raise RuntimeError("an unexpected PA job appeared after off was requested")
        return state


def inspect(check, mode):
    check.log("no explicit reset; allowed commands are status/off only; opening USB can still reset hardware")
    check.capture(0.5)
    initial = check.status()
    check.log(f"recognized LPF-AUTO: {initial['state']}; G48={initial['g48']}; "
              f"LPF={initial['lpf']} {initial['mv']} mV; fault={initial['fault']}")
    final = check.off() if mode == "off" else initial
    check.capture(0.2)
    if CRASH.search(check.history):
        raise RuntimeError("firmware initialization/crash evidence in transcript; inspect logs")
    if mode == "off":
        check.log("OFF VERIFIED BY FIRMWARE: DISARMED; G48/G47 reported LOW; sensing off. "
                  "This is not a measurement of zero RF or rail voltage.")
    else:
        check.log("STATUS CAPTURED ONLY: no scan/arm/key/off/reset requested. "
                  "Status input may have cancelled a job under firmware policy.")
    check.log(f"shutdown_fault={final['fault']}; cooldown_ms={final['cooldown']}; "
              "RF/PA performance NOT tested; no fault cleared")
    if final["fault"] == "LATCHED":
        check.log("Fault remains latched: follow the documented full hardware power-cycle "
                  "and explicit confirmation procedure; USB reset alone is insufficient.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tab5", required=True, help="Tab5 USB serial device")
    parser.add_argument("--output", required=True, type=Path,
                        help="New transcript file; refuses to overwrite an existing file")
    parser.add_argument("--mode", choices=("off", "status"), default="off",
                        help="off (default) verifies disarmed power-off; status only reports state")
    parser.add_argument("--timeout", type=float, default=9,
                        help="Maximum seconds per status/off phase (2..30, default 9)")
    args = parser.parse_args()
    if not 2 <= args.timeout <= 30:
        parser.error("timeout must be between 2 and 30 seconds")
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    # Avoid requesting an intentional reset. Some OS/bridge combinations may
    # still glitch these signals on open; the tool cannot guarantee otherwise.
    port.dtr = False
    port.rts = False
    port.port = args.tab5
    try:
        with args.output.open("x", encoding="utf-8") as output:
            check = Validation(port, output, args.timeout)
            try:
                port.open()
                inspect(check, args.mode)
                return 0
            except (RuntimeError, serial.SerialException, OSError, KeyboardInterrupt) as error:
                check.log(f"FAIL: {error}")
                if args.mode == "off" and check.recognized and port.is_open:
                    try:
                        check.log("requesting off again after failure; final state is not verified")
                        check.send("off")
                        check.capture(1)
                    except (serial.SerialException, OSError):
                        pass
                return 1
            finally:
                port.close()
    except OSError as error:
        print(f"PA VALIDATE FAIL: cannot create transcript: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
