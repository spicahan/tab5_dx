#!/usr/bin/env python3
"""Reset the real Tab5 RF-board scope build and check UART evidence.

Use only with BS170s absent. Leaves the last successful CLK0 carrier running.
This checks firmware evidence, not frequency/amplitude at the physical pin.
"""

import argparse
import re
import sys
import time

import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tab5", required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    if args.runs < 1 or args.timeout <= 0:
        parser.error("runs and timeout must be positive")

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.tab5
    try:
        port.open()
        # Opening the USB bridge may reset the board. Let that run finish
        # before performing the explicit, fully captured resets below.
        # Drain while waiting: USB Serial/JTAG can retain device-side log
        # bytes that reset_input_buffer() alone cannot discard on the host.
        settle_until = time.monotonic() + 3
        while time.monotonic() < settle_until:
            port.read(port.in_waiting or 1)
        for run in range(1, args.runs + 1):
            port.reset_input_buffer()
            print(f"SCOPE: reset {run}/{args.runs}", flush=True)
            HardReset(port, uses_usb=True)()
            history = ""
            deadline = time.monotonic() + args.timeout
            ready_at = None
            failed_at = None
            while time.monotonic() < deadline:
                data = port.read(port.in_waiting or 1).decode("utf-8", "replace")
                data = re.sub(r"\x1b\[[0-9;]*m", "", data)
                history += data
                print(data, end="", flush=True)
                if re.search(r"SELF-TEST FAILED|Guru Meditation|abort\(\)|"
                             r"(?m:^E \([0-9]+\))", history):
                    if failed_at is None:
                        failed_at = time.monotonic()
                    # Keep capturing the power-off cleanup before closing USB.
                    if time.monotonic() - failed_at >= 1:
                        raise RuntimeError(f"firmware failure in run {run}")
                if "CLK0 SCOPE READY:" in history and ready_at is None:
                    ready_at = time.monotonic()
                if (failed_at is None and ready_at is not None and
                        time.monotonic() - ready_at >= 1):
                    break
            if failed_at is not None:
                raise RuntimeError(f"firmware failure in run {run}")
            for marker in (
                "SDA=GPIO31, SCL=GPIO32",
                "G48=HIGH (power enabled), G47=HIGH (RX / TX off)",
                "SI5351 SELF-TEST PASS",
                "I2S CAPTURE SANITY PASS: 4800 PCM1808 frames received",
                "ALL ENABLED SELF-TESTS PASS",
                "CLK0 SCOPE READY: nominal 14075000 Hz; CLK0 only;",
            ):
                if marker not in history:
                    raise RuntimeError(f"run {run} missing evidence: {marker}")
            print(f"SCOPE: run {run} PASS", flush=True)
        print(f"SCOPE PASS: {args.runs} real-board runs; CLK0 left running", flush=True)
        return 0
    except (RuntimeError, serial.SerialException) as error:
        print(f"SCOPE FAIL: {error}", flush=True)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
