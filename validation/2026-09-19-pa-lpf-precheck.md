# LPF scan/pre-key checks, without runtime monitoring

## Reason and scope

The operator's previous image qualified 40 m at 1059–1060 mV before TX,
then cut RF power after a batch spanning 960–1257 mV (297 mV spread).
That batch violated the 900–1200 mV window and 80 mV spread limit. The
operator subsequently reported that retrofitting 100 nF under the LPF socket
did not resolve the false trip, and explicitly requested removal of runtime
LPF monitoring because the filter is not hot-swappable.

The new profile retains the same voltage, calibration and stability checks,
but runs them only at boot/`scan` and immediately before every PA/leak burst.
This is a bench-workflow change, not evidence that the underlying RF/ADC
interference has been fixed. A changed or removed LPF during a burst is now
undetected. Power down all supplies before changing the filter.

## Implementation

- Boot and explicit `scan` verify Si5351 outputs OFF, then require five new
  consecutive calibrated 40 m batches spanning at least 100 ms. Qualification
  has a 600 ms bound inside the three-second preparation guard.
- Eligibility is latched through idle/cooldown. The status voltage is the
  last checked value, not a live reading. No ADC sampling runs during those
  states, and no LPF freshness timer remains.
- Every accepted `pa`/`leak` command gets another new qualification epoch
  after clock setup, optional I2S baseline capture, and persistent-marker
  writes. CLK0 remains OFF throughout this check. Wrong/missing/invalid
  readings or ADC failure prevent keying and request power OFF.
- Qualification stops the sampler atomically between ADC batches, with an
  explicit `sense_busy` acknowledgement and epoch invalidation. It cannot
  accept while an ADC conversion is still in flight. Old evidence from boot
  or a previous burst cannot substitute for this new acquisition.
- A fresh (<200 ms) result is checked immediately before CLK0 enable.
  Once keyed, no ADC-based invalid/stale check aborts transmission. The
  accepted result does not expire during the 10-second burst.
- Preparation is independently bounded by the existing three-second ISR
  timer. The burst timer is armed before the preparation timer is stopped.
  Neither ADC work nor normal worker progress is required by the cutoff ISR.
- Fixed 40 m / 7.075 MHz, G47 LOW, explicit one-burst commands, 100/250/10000 ms
  PA limits, short leakage limits, cooldowns, operator cancellation, verified
  clock shutdown and persistent shutdown-fault handling remain in place.
- G48 stays HIGH with clocks OFF after a successful scan/burst; this no longer
  means that the LPF is continuously monitored. `off` still cuts power and
  inhibits until an explicit `scan` or a fault-free reboot.

LPF ID does not verify filter response, dummy-load presence, SWR, PA current,
temperature or actual RF cessation. Use the 40 m LPF and a rated 50-ohm dummy
load, never an antenna, for this bench profile.

## Verification

Both the PA and old loopback profiles build with ESP-IDF 5.5.1. The loopback
image remains unsuitable for the populated PA and was not flashed. The
existing five native suites pass
with `-Wall -Wextra -Werror -fsanitize=address,undefined`: command policy,
persistent-shutdown policy, LPF classifier, ADC wrapper, and I/Q capture.
Updated policy regressions cover latched readiness beyond 60 seconds, all
supported duration/cooldown combinations without fresh ADC samples, explicit
second-command requirements, invalidation/recovery and timestamp errors.

A sixth native suite includes the production `pa_test.c` directly, with stub
peripherals and a deterministic cooperative task/timer scheduler. It exercises
successful startup scanning and wrong-band scan rejection, all PA/leak
durations, fresh acquisition after clock/I2S/NVS preparation, ADC
quiescence before keying, no ADC reads during TX or subsequent idle, wrong/invalid
LPF and unavailable/slow/failing ADC rejection, expired pre-key evidence, and
timer/operator cutoff. These are production-code branch/order tests, not proof
of real multicore scheduling, interrupt latency, analog behavior or RF safety.

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/pa_precheck_stubs -Itests/lpf_sense_stubs -Itab5/main/include \
  tests/pa_precheck_test.c tab5/main/pa_policy.c tab5/main/lpf_classify.c \
  -o /tmp/pa_precheck_test
/tmp/pa_precheck_test
```

ELF inspection places `cutoff_isr`, `trip_locked` and `gpio_set_level` in IRAM,
and the cutoff's shared state in internal DRAM. The obsolete LPF-watch ISR
is absent. This does not measure actual RF-envelope or power-switch timing.

This revision has **not been flashed or tested on hardware**. No serial port
was opened, no board reset was requested, and no PA command was sent.

## Built image identity

PA application: 372,400 bytes (`0x5aeb0`); 64% free in the 1 MiB partition.
Built before committing this revision; embedded version `5caf950-dirty`.

```text
Application BIN SHA256:
93192efb92670855a8dc3338b2fb03b36f262f3b56d356258a27d5dbff73ba07
ELF SHA256:
8de2dafe54a17dc36f764acedb4240711df6eb61888c166d8c32e2b085e8cad4
```

## Next hardware check

Flash only after the RF board is powered down/disconnected and serial
monitors are closed. Prefer leaving the Tab5 in its bootloader after flashing,
then power down before reconnecting the LPF/board/dummy load.

The new boot banner is `PA TEST READY: LPF PRECHECK mode`. Confirm successful
`LPF SCAN`, then `LPF PREKEY` for each explicit short test. Expected idle
status includes `sampling=off`, `LPF_last=40m`, and `runtime_monitor=off`.
First use `pa 100` and check normal completion before requesting `pa 10000`.
No automatic transmission or repeat is introduced by this change.
