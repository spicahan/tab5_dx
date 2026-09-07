#!/usr/bin/env python3
"""Reset and observe both USB-connected boards; run in the ESP-IDF Python env."""

import argparse
import re
import sys
import time

import serial
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mock", required=True)
    parser.add_argument("--tab5", required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=15)
    args = parser.parse_args()
    if args.runs < 1 or args.timeout <= 0:
        parser.error("runs and timeout must be positive")

    ports = {}
    buffers = {"mock": "", "tab5": ""}
    history = {"mock": "", "tab5": ""}
    started = time.monotonic()

    def check_errors(stage):
        combined = history["tab5"] + history["mock"]
        if re.search(r"SELF-TEST FAILED|Guru Meditation|abort\(\)|"
                     r"(?m:^E \([0-9]+\))|transaction logs dropped=", combined):
            raise RuntimeError(f"firmware failure during {stage}")

    def pump(duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            for name, port in ports.items():
                data = port.read(port.in_waiting or 1).decode("utf-8", "replace")
                if not data:
                    continue
                data = re.sub(r"\x1b\[[0-9;]*m", "", data)
                history[name] += data
                buffers[name] += data
                while "\n" in buffers[name]:
                    line, buffers[name] = buffers[name].split("\n", 1)
                    print(f"[{time.monotonic() - started:8.3f} {name}] {line.rstrip()}",
                          flush=True)
            time.sleep(0.005)

    try:
        for name, path in (("mock", args.mock), ("tab5", args.tab5)):
            port = serial.Serial(port=None, baudrate=115200, timeout=0)
            port.dtr = False
            port.rts = False
            port.port = path
            port.open()
            ports[name] = port
        # Opening the USB bridges can itself reset a board. Let that startup
        # finish before resetting the mock so we do not interrupt host I2C.
        pump(2.0)
        deadline = time.monotonic() + args.timeout
        while ("Calling app_main()" in history["tab5"] and
               "ALL ENABLED SELF-TESTS PASS" not in history["tab5"]):
            check_errors("initial USB-open startup")
            if time.monotonic() > deadline:
                raise RuntimeError("initial USB-open startup did not finish")
            pump(0.1)
        check_errors("initial USB-open startup")
        print("BENCH: resetting mock and waiting for both emulators", flush=True)
        history["mock"] = ""
        HardReset(ports["mock"])()
        deadline = time.monotonic() + args.timeout
        while "Ready for the Tab5 I2C and I2S host tests" not in history["mock"]:
            pump(0.1)
            if time.monotonic() > deadline:
                raise RuntimeError("mock did not become ready")

        for run in range(1, args.runs + 1):
            pump(1.0)
            history["mock"] = ""
            history["tab5"] = ""
            print(f"BENCH: run {run}/{args.runs}: resetting Tab5 only", flush=True)
            HardReset(ports["tab5"], uses_usb=True)()
            deadline = time.monotonic() + args.timeout
            while "ALL ENABLED SELF-TESTS PASS" not in history["tab5"]:
                pump(0.1)
                check_errors(f"run {run}")
                if time.monotonic() > deadline:
                    raise RuntimeError(f"Tab5 completion timeout in run {run}")
            # The mock's asynchronous I2C log queue can trail host completion.
            pump(1.0)
            check_errors(f"run {run} completion")
            for marker in ("SI5351 SELF-TEST PASS", "I2S SELF-TEST PASS",
                           "SDA=GPIO31, SCL=GPIO32"):
                if marker not in history["tab5"]:
                    raise RuntimeError(f"missing Tab5 evidence: {marker}")
            if "MCLK PASS:" not in history["mock"]:
                raise RuntimeError("mock did not independently qualify MCLK")
            for marker in ("PASS: Si5351 programming order, clock plan and state are valid",
                           "valid I2S pattern revoked until MCLK qualifies again"):
                if marker not in history["mock"]:
                    raise RuntimeError(f"missing mock evidence: {marker}")
            print(f"BENCH: run {run} PASS", flush=True)
        print(f"BENCH PASS: {args.runs} combined internal-I2C/I2S runs", flush=True)
        return 0
    except (RuntimeError, serial.SerialException) as error:
        print(f"BENCH FAIL: {error}", flush=True)
        return 1
    finally:
        for port in ports.values():
            port.close()


if __name__ == "__main__":
    sys.exit(main())
