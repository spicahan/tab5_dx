#!/usr/bin/env python3
"""Validate the already-flashed disarmed PA console, not RF/PA performance.

Default checks never send an accepted PA/leak burst. --disconnected explicitly
asserts the RF daughter board is physically disconnected and permits one pa 100
job, which MUST fail at the missing Si5351 probe before any CLK0 enable attempt.
That optional negative test intentionally leaves a persistent shutdown fault;
the script never clears it, including across USB resets.
Use only after flashing the PA-test image: resetting an older automatic-loopback
image could enable its outputs. This script cannot detect a physical connection,
prove a pin voltage, clear a shutdown fault, or certify hardware safety.
"""

import argparse
import re
import sys
import time
from pathlib import Path

import serial
from esptool.reset import HardReset


ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
FATAL = re.compile(r"Guru Meditation|abort\(\)|PA INIT FAILED")


class Validation:
    def __init__(self, port, output):
        self.port = port
        self.output = output
        self.history = ""
        self.expect_latched_fault = False

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

    def check_fatal(self):
        match = FATAL.search(self.history)
        if match:
            raise RuntimeError(f"firmware failure/fault: {match.group(0)}")
        if not self.expect_latched_fault and re.search(
                r"shutdown_fault=LATCHED|PA SHUTDOWN FAULT LATCHED", self.history):
            raise RuntimeError("unexpected shutdown fault; a physical power-cycle/confirmation is required")

    def capture(self, seconds, check=True):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.read()
            if check:
                self.check_fatal()

    def wait(self, pattern, offset=0, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.check_fatal()
            match = re.search(pattern, self.history[offset:])
            if match:
                return self.history[offset:]
            self.read()
        raise RuntimeError(f"timed out waiting for {pattern!r}")

    def command(self, payload, expected, timeout=4):
        # Separate intentional commands so the device can reject queued batches.
        self.capture(0.15)
        offset = len(self.history)
        if isinstance(payload, str):
            payload = payload.encode("ascii") + b"\n"
        self.log(f"send {payload!r}")
        self.port.write(payload)
        self.port.flush()
        return self.wait(expected, offset, timeout)

    def status(self, armed=False, fault="none"):
        state = "ARMED" if armed else "DISARMED"
        response = self.command("status", rf"PA STATUS: {state};[^\r\n]*shutdown_fault={fault}")
        match = re.search(rf"PA STATUS: {state}; G48=0 G47=0; band=40m "
                          rf"source=7075000 Hz; cooldown_ms=(\d+); shutdown_fault={fault}", response)
        if not match:
            raise RuntimeError("status did not report the expected safe GPIO/band/fault state")
        return int(match.group(1))

    def arm(self):
        self.command("arm 40m dummyload", r"PA ARMED: one use, expires in 30s; RF remains OFF")

    def reject(self, payload, reason="invalid command"):
        return self.command(payload, rf"PA REJECTED: {re.escape(reason)}")

    def assert_no_jobs(self):
        if re.search(r"PA PREP:|PA JOB |key_attempted=yes", self.history):
            raise RuntimeError("a hardware job appeared during disarmed-only validation")


def validate(check, disconnected, expiry):
    check.log("resetting expected PA-test firmware; no RF performance test is authorized")
    HardReset(check.port, uses_usb=True)()
    check.wait(r"PA TEST READY: DISARMED; G48=LOW G47=LOW; fixed 40m; no boot TX", timeout=12)
    if "PA GUARD SELF-CHECK PASS: ISR fired with RF power held off" not in check.history:
        raise RuntimeError("missing independent cutoff self-check evidence")
    check.status()
    check.assert_no_jobs()

    for command in ("pa", "leak"):
        check.reject(command, "not armed")
    check.reject("arm 20m dummyload")
    check.reject("clearfault rebooted")

    check.arm()
    check.status(armed=True)
    check.command("off", r"PA OFF: disarmed")
    check.status()

    check.arm()
    check.reject("pa 1000")
    check.reject("pa", "not armed")
    check.arm()
    check.reject(b"arm 40m dummyload\x00\n")
    check.reject("pa", "not armed")
    check.arm()
    check.reject(b"x" * 80 + b"\n")
    check.reject("pa", "not armed")
    # Use only non-keying commands for the queued-batch check. In particular,
    # never send an arm-then-pa sequence on a potentially connected board: USB
    # fragmentation could make those look like separately entered commands.
    check.reject(b"arm 40m dummyload\nstatus\n")
    check.reject("pa", "not armed")

    if expiry:
        check.arm()
        check.log("waiting 31 seconds for the arm token to expire; RF remains off")
        check.capture(31)
        check.reject("pa", "arm expired")

    check.status()
    check.assert_no_jobs()
    if disconnected:
        check.log("EXPLICIT DISCONNECTED CHECK: accepting one pa 100; missing Si5351 must stop it")
        check.arm()
        check.capture(1.1)
        start = len(check.history)
        check.expect_latched_fault = True
        check.command("pa 100", r"PA DISARMED: job ended; 5000 ms cooldown; fresh arm required", timeout=8)
        job = check.history[start:]
        required = (
            r"PA PREP: 40m CLK0=7075000 Hz; G47=LOW; mode=pa; limit=100 ms",
            r"PA SAFE OFF: G48=0 G47=0; key_attempted=no;",
            r"PA JOB FAILED at Si5351 probe:",
            r"PA SHUTDOWN FAULT LATCHED:",
        )
        for marker in required:
            if not re.search(marker, job):
                raise RuntimeError(f"missing disconnected-job evidence: {marker}")
        if "PA JOB COMPLETE" in job or "key_attempted=yes" in job:
            raise RuntimeError("unexpected clock-enable attempt; RF board may not be disconnected")
        check.reject("arm 40m dummyload", "cooldown active")
        if check.status(fault="LATCHED") == 0:
            raise RuntimeError("expected nonzero cooldown immediately after disconnected job")
        check.capture(5.1)
        if check.status(fault="LATCHED") != 0:
            raise RuntimeError("cooldown did not expire")
        check.reject("arm 40m dummyload", "shutdown fault latched")

    check.command("off", r"PA OFF: disarmed")
    check.status(fault="LATCHED" if disconnected else "none")
    check.capture(0.2)
    expected_jobs = 1 if disconnected else 0
    if check.history.count("PA PREP:") != expected_jobs:
        raise RuntimeError("unexpected number of accepted hardware jobs")
    if "PA JOB COMPLETE" in check.history or "key_attempted=yes" in check.history:
        raise RuntimeError("unexpected successful/keyed hardware job")
    check.log("PASS: console/policy evidence only; left DISARMED, G48/G47 reported LOW; "
              f"shutdown_fault={'LATCHED' if disconnected else 'none'}; "
              f"accepted jobs={expected_jobs}; RF/PA output NOT tested")
    if disconnected:
        check.log("Persistent shutdown fault intentionally left latched; no automatic fault clear. "
                  "Follow full RF-board power-cycle/confirmation procedure before subsequent arming.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tab5", required=True, help="Tab5 USB serial device")
    parser.add_argument("--output", required=True, type=Path,
                        help="New transcript file; refuses to overwrite an existing file")
    parser.add_argument("--disconnected", action="store_true",
                        help="Assert RF board is physically disconnected; permit one missing-board pa 100 job, "
                             "leaving a persistent shutdown fault latched")
    parser.add_argument("--expiry", action="store_true", help="Also wait 31 seconds to verify arm expiry")
    args = parser.parse_args()
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.tab5
    check = None
    try:
        with args.output.open("x", encoding="utf-8") as output:
            check = Validation(port, output)
            try:
                port.open()
                # Opening a USB bridge may reset it. Drain that boot before the
                # explicit reset whose complete safety markers are checked.
                settle_until = time.monotonic() + 3
                while time.monotonic() < settle_until:
                    port.read(port.in_waiting or 1)
                port.reset_input_buffer()
                validate(check, args.disconnected, args.expiry)
                return 0
            except (RuntimeError, serial.SerialException, OSError, KeyboardInterrupt) as error:
                check.log(f"FAIL: {error}")
                # Never send console commands to an unrecognized firmware image.
                if port.is_open and "PA TEST READY: DISARMED;" in check.history:
                    try:
                        check.log("requesting cancellation/off after failure; inspect cleanup evidence")
                        port.write(b"\x03off\n")
                        port.flush()
                        check.capture(2, check=False)
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
