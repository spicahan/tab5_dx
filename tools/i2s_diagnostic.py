#!/usr/bin/env python3
"""Capture persistent I2S diagnostic logs; verify the harness, not ADC quality.

Use only with BS170s absent. Leaves RF power and I2S clocks running for probing.
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
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--rf-clocks", choices=("off", "rx"), default="off",
                        help="Expected firmware clock state: all off or CLK1/QSD only")
    args = parser.parse_args()
    if not 6 <= args.seconds <= 60:
        parser.error("capture duration must be between 6 and 60 seconds")

    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.tab5
    try:
        port.open()
        settle_until = time.monotonic() + 3
        while time.monotonic() < settle_until:
            port.read(port.in_waiting or 1)
        port.reset_input_buffer()
        print("I2S DIAGNOSTIC: resetting Tab5; sample-quality warnings are expected", flush=True)
        HardReset(port, uses_usb=True)()
        history = ""
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = port.read(port.in_waiting or 1).decode("utf-8", "replace")
            data = re.sub(r"\x1b\[[0-9;]*m", "", data)
            history += data
            print(data, end="", flush=True)

        if re.search(r"SELF-TEST FAILED|Guru Meditation|abort\(\)|"
                     r"(?m:^E \([0-9]+\))", history):
            raise RuntimeError("firmware/transport error; inspect power-off cleanup above")
        expected_rf_state = (
            "CLK1/QSD=28296000 Hz; CLK0 OFF; G48=HIGH, G47=HIGH"
            if args.rf_clocks == "rx" else
            "Si5351 outputs OFF; G48=HIGH, G47=HIGH"
        )
        for marker in (
            "SDA=GPIO31, SCL=GPIO32",
            "SI5351 SELF-TEST PASS",
            expected_rf_state,
            "I2S DIAGNOSTIC ACTIVE: clocks remain on for probing",
            "MCLK=GPIO16, BCLK=GPIO45, LRCK=GPIO3, DIN=GPIO4",
            "diagnostic startup discard: 48000 frames",
        ):
            if marker not in history:
                raise RuntimeError(f"missing evidence: {marker}")
        windows = re.findall(r"I2S DIAG window=(\d+)\b", history)
        if len(windows) < 3:
            raise RuntimeError(f"only {len(windows)} diagnostic windows; expected at least 3")
        if len(windows) != len(set(windows)):
            raise RuntimeError("repeated window numbers suggest an unexpected restart")
        print(f"DIAGNOSTIC HARNESS OK: {len(windows)} windows; RF clocks={args.rf_clocks}; "
              "ADC quality NOT certified; "
              "power and I2S clocks left running", flush=True)
        return 0
    except (RuntimeError, serial.SerialException) as error:
        print(f"DIAGNOSTIC HARNESS FAIL: {error}", flush=True)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
