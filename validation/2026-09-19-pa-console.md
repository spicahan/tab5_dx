# Manual 40 m PA firmware: build, flash and disconnected-console validation

## Result and scope

The new `sdkconfig.pa.defaults` image was built and flashed to the Tab5 on
`/dev/cu.usbmodem101` with ESP-IDF 5.5.1. The RF daughter board was disconnected,
as reported by the operator. The Tab5 was left DISARMED, reporting G48=0,
G47=0 and `shutdown_fault=none`. No accepted PA/leak job was sent and no RF
output, PA current, pulse width, efficiency, temperature or populated-board
leakage was measured.

This profile hardcodes 40 m: CLK0 is nominally 7.075 MHz from the 26 MHz
external reference. G47 remains LOW. Ordinary PA bursts keep CLK1 off; the
optional leakage mode uses 28.296 MHz CLK1 and 48 kHz stereo I2S.

## Evidence

- [USB console transcript](2026-09-19-pa-console.log).
- PA build passed; application image is 350,400 bytes (`0x558c0`), with 67%
  of the 1 MiB application partition remaining.
- Flash completed with successful bootloader, application and partition-table
  hash verification. Target identified as ESP32-P4 revision 1.3.
- Boot log reported `PA GUARD SELF-CHECK PASS`, then `PA TEST READY: DISARMED`.
  The self-check fires the cutoff ISR while G48 is already LOW. It is not a
  measured HIGH-to-LOW transition or physical power-switch test.
- Status while armed still reported G48=0 and G47=0. `off` disarmed it.
- Unarmed PA/leak requests, wrong-band arming, unsupported duration,
  embedded NUL, an overlong line and an already-queued command batch were
  rejected. Invalid commands removed arming permission.
- An arm token was allowed to expire for 31 seconds; the subsequent PA
  command was rejected as `arm expired`.
- Final status was DISARMED, G48=0, G47=0, cooldown=0, fault=none.
- Pure C policy and I/Q measurement host tests passed with
  `-Wall -Wextra -Werror -fsanitize=address,undefined`. Policy tests include
  one-use arming, accepted durations, cooldown, exact fault-clear syntax,
  busy rejection, expiry, malformed commands and monotonic-clock handling.
  Measurement tests cover 50/100/200/250/1000 ms captures, quadrature 1/2 kHz,
  DC, silence, clipping, padding, partial reads and errors.
- The old BS170-absent loopback profile still builds after the capture/report
  refactor. It was NOT flashed and must not be used with a populated PA.
- ELF inspection placed the cutoff ISR, its critical-section/GPIO calls and
  its shared state in internal instruction/data memory. No flash or logging
  calls occur in that callback. This is structural evidence, not a measured
  worst-case interrupt latency guarantee.

Validation command (no accepted hardware jobs):

```sh
python tools/pa_validate.py --tab5 /dev/cu.usbmodem101 \
  --output validation/2026-09-19-pa-console.log --expiry
```

The runner refuses to overwrite a transcript. Use a new output filename for
another run. Its optional `--disconnected` missing-board job was not used:
that test deliberately leaves an unverified-clock-state fault latched.

## Image identity

The image was compiled from the changes recorded alongside this report,
before their commit. Its embedded Git version is therefore `1fc8a41-dirty`;
the following hashes identify the actual tested image rather than the old
commit alone:

```text
Application BIN SHA256:
7c1d28f660ec0637c96f95dbde7f8e659f9ac613ce7ec5414c7cf2d13dc2a0f1
ELF SHA256 (matches boot log prefix 43d84f21f):
43d84f21ff93084012a82c758f7a099a68b2d8285599e96e764415a9223fd3d3
```

## Next physical test and limitations

Power off all sources before reconnecting. Fit the 40 m LPF and a suitably
rated 50-ohm dummy load, with no antenna. Check the supply, polarity, load,
current and heating manually. Then send `arm 40m dummyload`, wait for its
reply and separately send `pa 100`. The longer `pa 250` and optional
`leak 100`/`leak 250` require fresh arming and the five-second cooldown.

The 100/250 ms selection is a software guard deadline, not a guaranteed RF
envelope duration. Ordinary shutdown is requested roughly 20 ms earlier to
allow I2C completion. G48 removal can leave a rail-discharge tail; external
pull-ups and other connections can backpower the board. Hardware default
biasing and physical power removal remain important.

Persistent shutdown-fault code was reviewed but reset/brownout persistence,
active cancellation, powered cutoff timing and successful connected-board
jobs were not experimentally exercised in this run. The LPF, load, current
and temperature are not sensed. See the [PA operating instructions](../tab5/README.md#manual-40-m-pa-test-populated-bs170s)
for the manual full-power-cycle/fault-clear procedure and leakage-test limits.
