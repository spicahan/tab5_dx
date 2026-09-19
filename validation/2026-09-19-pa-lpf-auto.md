# LPF-qualified auto-arm and 10-second PA command

## Outcome

Implemented and built the new PA profile with ESP-IDF 5.5.1, then flashed it
to the ESP32-P4 v1.3 Tab5 at `/dev/cu.usbmodem101`. The operator confirmed the
RF board was powered down/disconnected and serial monitors were closed.
Flashing used `--after no_reset`; the tool verified all written images and
ended with **Staying in bootloader**. The application was deliberately not
started afterward. No live ADC/LPF qualification or 10-second RF burst was
performed in this run.

The preceding manually armed firmware's 250 ms test was reported successful
by the operator: its log showed verified clock shutdown, no interruption and
a 240,728 us command interval, and the operator observed RF on the scope.
That evidence applies to the previous image, not the new ADC interlock or
long-burst implementation.

## Hardware basis and behavior

The supplied final schematic and programming notes establish G51 as LPF
sense, with a 100 kOhm pull-up to switched RF-board 3.3 V, and band resistors
of 47 kOhm for 40 m and 100 kOhm for 20 m. Nominal voltages are 1.055 V and
1.650 V respectively. Installed ESP-IDF maps G51 to ADC2/channel 2.

Because this divider is powered from the switched rail, boot performs a
guarded RF power-on inspection with CLK0 disabled, then keeps G48 HIGH for
continuous sensing if the 40 m LPF qualifies. G47 stays LOW. Automatic arming
never requests transmission: a new explicit PA/leak command is always needed.

- Per-channel curve-fitting calibration is mandatory, at 12 dB attenuation.
  Initialization/calibration/read failures fail closed, without a raw fallback.
- Eight calibrated samples per batch; every sample must match one category,
  with spread at most 80 mV and no raw rail code.
- 40 m admission: 900–1200 mV. 20 m: 1450–1850 mV, identified but blocked
  for fixed 7.075 MHz TX. Missing ID: at least 2800 mV without saturation.
  All other readings are invalid and cannot arm.
- Qualification requires five consecutive 40 m batches spanning at least
  100 ms. One invalid batch revokes permission. Old in-flight conversions
  cannot survive a power/session change.
- The ADC task is pinned to CPU1; the independent timer ISR runs on CPU0.
  The freshness watcher checks approximately every 20 ms and cuts G48 for
  data older than 200 ms. These software intervals do not guarantee RF
  cessation within 200 ms, given scheduling and rail-discharge tails.
- `pa 10000` permits a single 10-second guarded test. Normal clock shutdown
  begins around 9.95 seconds; a 10-second cooldown follows. Short PA and
  leakage commands retain 100/250 ms guards and five-second cooldowns.
  `leak 10000` is rejected.
- Successful jobs leave clocks OFF but G48 HIGH for sensing. Cooldown expiry
  can restore readiness, never start another transmission. `off` cuts power
  and inhibits sensing until `scan` or a fault-free reboot.
- Wrong/missing/invalid/stale sensing cuts power, without automatic retry.
  `scan` explicitly resumes inspection. Legacy `arm 40m dummyload` aliases
  `scan` and cannot bypass LPF classification.
- Persistent shutdown faults cannot be cleared by ordinary OFF/cleanup or
  by a reset. Recovery requires the documented full hardware power-cycle
  and `clearfault powercycled`, followed separately by `scan`.

## Verification completed

Five native suites passed with `-Wall -Wextra -Werror` and AddressSanitizer /
UndefinedBehaviorSanitizer:

1. PA policy: fresh-LPF readiness without auto-TX; stale/wrong/error evidence;
   sticky inhibition; fault gates; scan; accepted/rejected durations; cooldown;
   monotonic timestamps and malformed input.
2. LPF classification: voltage boundaries, raw rails, outliers, excessive
   spread, invalid sample counts and arithmetic extremes.
3. ADC wrapper: exact GPIO/unit/channel/calibration setup, dummy conversion,
   mandatory calibration, cleanup and each injected read/calibration failure.
4. Shutdown safety: all 16 cleanup-predicate combinations, inherited fault
   followed by OFF/reset, unverified shutdown and explicit recovery policy.
5. Existing I/Q capture: 50/100/200/250/1000 ms windows, quadrature/DC/noise-free
   synthetic cases, padding/clipping checks, partial reads and failures.

Both the PA profile and the older loopback profile built successfully. Only
the PA image was flashed. The older profile must not run with the populated
PA. ELF inspection placed both ISR callbacks, their cutoff/time/GPIO/critical
section callees and shared state in internal memory; the ISR path makes no
ADC, I2C, logging or NVS calls. Hardware timing and brownout behavior remain
unmeasured.

The serial utility was changed to a status/off-only allowlist. It no longer
resets, scans, arms, keys or clears faults. This avoids interpreting a formerly
rejected, unarmed PA command as harmless now that LPF qualification auto-arms.
It was not run against the new image in this session.

## Flashed image identity

Application size: 372,128 bytes (`0x5ada0`), 65% free in the 1 MiB partition.
Built before this work's commit; embedded version is `2295154-dirty`.

```text
Application BIN SHA256:
59bfa9027074af600a10538e1724a2a1b3c0b2b89c06afefeaa010e59854259f
ELF SHA256:
44c2f349c615925bce22bf44c85198162e9894525653ccd18b193377afe21a71
```

Flash command, run in `tab5/build-pa`:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem101 -b 460800 \
  --before default_reset --after no_reset write_flash @flash_args
```

## First connected-board checks

Power down the Tab5 and all RF supplies before reconnecting the 40 m LPF,
RF board and suitably rated 50-ohm dummy load. No antenna. Boot normally,
inspect `LPF SCAN` and `PA AUTO ARMED`, and compare the measured G51 voltage
with a DMM before relying on the classification. Do not hot-swap LPFs.

The high-resistance divider can be sensitive to ADC loading/noise. Espressif's
[ADC hardware guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html#adc)
recommends a 0.1 uF input capacitor; the supplied main schematic does not show
one on G51. Investigate biased/noisy readings rather than widening thresholds.
The [chip-revision v1.3 datasheet](https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.html)
is the relevant ADC reference for this Tab5. Physical readings and actual
empty/wrong-band rejection still need testing.

Before requesting `pa 10000`, check the supply/load and monitor current and
heating. LPF resistor ID does not verify the RF filter response, dummy load,
SWR, PA current or temperature. See [operating instructions](../tab5/README.md#manual-40-m-pa-test-populated-bs170s).
