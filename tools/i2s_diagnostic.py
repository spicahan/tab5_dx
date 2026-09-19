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
    parser.add_argument("--rf-clocks", choices=("off", "rx", "loopback"), default="off",
                        help="Expected RF state: off, CLK1 only, or BS170-absent loopback")
    parser.add_argument("--startup-ms", type=int, default=1000,
                        help="Expected diagnostic startup discard at 48 kHz")
    args = parser.parse_args()
    if not 6 <= args.seconds <= 60:
        parser.error("capture duration must be between 6 and 60 seconds")
    if not 0 <= args.startup_ms <= 5000:
        parser.error("startup discard must be between 0 and 5000 milliseconds")
    if args.rf_clocks == "loopback" and (args.seconds < 20 or args.startup_ms != 1000):
        parser.error("loopback requires at least 20 seconds and --startup-ms 1000")

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
        expected_rf_state = {
            "off": "Si5351 outputs OFF; G48=HIGH, G47=HIGH",
            "rx": "CLK1/QSD=28296000 Hz; CLK0 OFF; G48=HIGH, G47=HIGH",
            "loopback": "RX LOOPBACK HOLD: CLK0=7075000 Hz; CLK1=28296000 Hz; "
                        "expected beat=1000 Hz; G48=HIGH, G47=HIGH",
        }[args.rf_clocks]
        for marker in (
            "SDA=GPIO31, SCL=GPIO32",
            "SI5351 SELF-TEST PASS",
            expected_rf_state,
            "I2S DIAGNOSTIC ACTIVE: clocks remain on for probing",
            "MCLK=GPIO16, BCLK=GPIO45, LRCK=GPIO3, DIN=GPIO4",
            "clocks: Fs=48000 Hz",
            f"diagnostic startup discard: {48 * args.startup_ms} frames",
            "I2S INITIAL 100MS frames=4800 ",
            "I2S FIRST CHANGE frame_offset ",
        ):
            if marker not in history:
                raise RuntimeError(f"missing evidence: {marker}")
        if args.rf_clocks == "loopback":
            labels = ("OFF_BEFORE", "ON_1KHZ", "OFF_AFTER", "ON_2KHZ", "HOLD_1KHZ")
            captures = re.findall(
                r"RF LOOPBACK label=(\w+) frames=48000 discard_frames=48000 "
                r"padding_nonzero=(\d+)", history)
            if tuple(label for label, _ in captures) != labels:
                raise RuntimeError(f"missing/reordered/repeated loopback captures: {captures}")
            if any(int(padding) for _, padding in captures):
                raise RuntimeError("loopback capture contains nonzero I2S padding")
            for label, frequency, state in (
                ("OFF_BEFORE", 7075000, "OFF"), ("ON_1KHZ", 7075000, "ON"),
                ("OFF_AFTER", 7075000, "OFF"), ("ON_2KHZ", 7076000, "ON"),
                ("HOLD_1KHZ", 7075000, "ON"),
            ):
                marker = f"LOOPBACK STATE {label}: CLK0={frequency} Hz {state}; "
                if marker not in history:
                    raise RuntimeError(f"missing evidence: {marker}")
            for frequency in (1000, 2000):
                if len(re.findall(rf"RF LOOPBACK tone={frequency} Hz ", history)) != 5:
                    raise RuntimeError(f"missing/repeated {frequency} Hz tone measurements")
            # Do not call source leakage or analog quality a PASS: amplitudes,
            # phases and rail hits must be interpreted against the OFF captures.
        windows = re.findall(r"I2S DIAG window=(\d+)\b", history)
        if len(windows) < 3:
            raise RuntimeError(f"only {len(windows)} diagnostic windows; expected at least 3")
        if len(windows) != len(set(windows)):
            raise RuntimeError("repeated window numbers suggest an unexpected restart")
        print(f"DIAGNOSTIC HARNESS OK: {len(windows)} windows; RF clocks={args.rf_clocks}; "
              f"startup={args.startup_ms} ms; "
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
