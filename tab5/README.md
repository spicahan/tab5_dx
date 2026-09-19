# Tab5 RF-board host bring-up

Native ESP-IDF 5.5.1 firmware for exercising the RF-board mock or the
real Si5351A and PCM1808 from the M5Stack Tab5. It is a UART-only bring-up
program with no GUI, M5GFX, M5Unified, Wi-Fi, or other managed-component
dependency.

## Manual 40 m PA test (populated BS170s)

Use `sdkconfig.pa.defaults` for a board with the BS170s installed. This profile
bypasses the old automatic carrier tests. **Boot automatically powers the RF
board to inspect its LPF ID, but never issues a CLK0/TX-enable command.** A stable,
calibrated 40 m ID automatically arms command eligibility. That result is
latched between checks; every `pa`/`leak` command requires a new stable LPF
check with CLK0 OFF before keying. G48 remains HIGH during idle and cooldown,
with the clocks OFF, but LPF ADC sampling is disabled then and during TX.
G47 stays LOW (RX disconnected) during scanning, preparation, every burst,
cleanup, and idle. This is not the earlier G47-HIGH receive-loopback test.

The band/frequency and wiring are fixed: 40 m, CLK0 at **7.075 MHz**, a 26 MHz
external Si5351 reference, internal M5-Bus I2C SDA/G31 and SCL/G32 at 100 kHz,
G51/ADC2 channel 2 LPF sense, and G48/G47 power/RX-switch controls. Optional
leakage capture uses the earlier I2S pins: MCLK/G16, BCLK/G45, LRCK/G3, DOUT/G4.
Disconnect the mock board; it has the same I2C address. RF power still needs
the proper battery-positive lead or DC supply: G48 is an enable, not a power
source. Check polarity, common ground and connector orientation.

### Install and connect safely

Flash with the RF board disconnected and unpowered. The older `build-loopback`
and `build-scope` images automatically enable a carrier and must not be used
with the populated PA. Use this separate build/configuration pair:

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx/tab5
idf.py -B build-pa -D SDKCONFIG=sdkconfig.pa \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.pa.defaults' build
idf.py -B build-pa -p /dev/cu.usbmodem101 flash
idf.py -B build-pa -p /dev/cu.usbmodem101 monitor
```

Existing `sdkconfig.pa` settings override defaults. Confirm the startup log
identifies `PA TEST READY: LPF PRECHECK mode` and the power-off ISR self-check
passes. The initial scan verifies Si5351 clocks off, then qualifies the LPF.
That ISR self-check does not verify the physical RF power switch. With the
board disconnected, the scan cannot contact the Si5351 and can latch a
persistent fault because the initial output-off state is unverified; this is
a fail-closed outcome, not a PA test. Follow the fault-recovery procedure below
after safely reconnecting. The older console validation record describes the
previous manually armed firmware, not this precheck-only behavior. The earlier
LPF auto-arm validation record describes the superseded continuous-monitoring
image and also does not validate this revision.

Power down **all** sources (battery, USB and external supply) before connecting
the RF board, LPF, dummy load, or test probes. Fit the **40 m LPF** and a
suitably rated **50-ohm dummy load** at the antenna output; do not attach an
antenna for this test. Use a current-limited supply where practical and monitor
current and heating. The firmware now measures the LPF's **resistor ID**, but
does not verify the RF filter response, load presence, output power, SWR,
current or temperature. Inspect those independently before transmitting.
Do not hot-plug or swap LPFs while powered, even if transmission is idle.

### LPF prechecks and automatic arming

The supplied `TAB5 DX RF V1.11 CIRCUIT SCHEMATIC.pdf` specifies a 100 kOhm
upper sense resistor. With a nominal 3.3 V rail, the 40 m LPF's 47 kOhm ID
resistor gives about 1.055 V; the 20 m LPF's 100 kOhm ID gives about 1.650 V.
G51 uses ADC2 channel 2 at 12 dB attenuation with per-channel curve-fitting
calibration. Calibration initialization must succeed: there is no raw-code
fallback and no automatic adjustment of thresholds.

| Calibrated G51 reading | Classification | Fixed 40 m TX eligible? |
| --- | --- | --- |
| 900–1200 mV | 40 m | Only after stable qualification |
| 1450–1850 mV | 20 m | No: wrong LPF for 7.075 MHz |
| At least 2800 mV, without raw ADC saturation | Missing | No |
| Other voltage, raw rail code, excessive spread, or ADC error | Invalid/error | No |

Each batch discards one conversion and evaluates eight samples individually.
All eight must be in the same accepted range and the batch spread must be
at most 80 mV; an in-range average cannot hide an outlier. Arming requires
at least five consecutive 40 m batches spanning at least 100 ms. During each
bounded qualification, samples are normally taken about every 25–35 ms;
acquisition must finish within 50 ms.
The scan includes a 200 ms initial power-settling interval before acceptance.
After a successful scan, logs show `LPF SCAN COMPLETE`, `PA AUTO ARMED`, and
`status` reports `ARMED_AUTO` with the measured millivolts.

G48 stays HIGH after qualification, but the accepted result is latched and LPF
ADC sampling stops. `status` shows the last check, not a live voltage reading.
Before every `pa` or `leak` burst, the firmware performs another bounded stable
qualification with CLK0 OFF; `LPF PREKEY` reports that result. A missing,
wrong-band, invalid or failed reading at these checks prevents keying and
requests G48 LOW. Inspect the readings/hardware and explicitly enter `scan`
after a failed check; there is no automatic retry.

**There is no LPF monitoring during idle, cooldown or transmission, and no
LPF-invalid/stale runtime cutoff.** A removal or change after the prekey check
will not abort the burst. The LPF must remain installed and must not be
hot-swapped. The independent three-second preparation guard, bounded burst
timer, operator cancellation and clock/fault shutdown checks remain active;
they do not detect a changed LPF or load.

This precheck-only behavior follows a hardware test in which PA keying
disturbed the band ADC reading enough to trip the continuous monitor, despite
stable pre-TX readings. The user reported that adding 100 nF under the LPF
socket did not resolve it. Disabling in-burst sensing avoids that particular
false-trip path; it does not establish that the underlying interference is
fixed or that the PA is electrically sound.

The divider has roughly 32 kOhm source resistance for 40 m, or 50 kOhm for
20 m. Compare the logged voltage with a meter at G51 before relying on the
classification. A **100 nF capacitor from G51 to ground, close to the ADC input,
is recommended** for sampling stability; it is not shown on the supplied main
schematic and must not be assumed present. Allow at least 50 ms input settling
after a supply change. If readings are biased or noisy, investigate the sense
connection, filtering, source impedance and calibration; do not widen the
windows or substitute raw thresholds to force the test to arm. An ID resistor
can identify a board but cannot prove its RF inductors/capacitors are correct.

### One manually requested burst

Wait for `PA AUTO ARMED` and check `status` while idle. Confirm the 40 m LPF
and rated 50-ohm dummy load are connected, then explicitly request one burst:

```text
pa 100
```

Do not paste or queue commands. `pa` defaults to `pa 100`; `pa 250` permits
the longer short test after inspecting the first result. A successful burst
is followed by a five-second cooldown. Latched eligibility automatically
returns after cooldown, but another explicit command and a fresh prekey check
are always required: no automatic TX or repeat is provided. There is no longer a separate
30-second manual arm token. `scan` starts/restarts powered LPF qualification;
the legacy `arm 40m dummyload` command is only an alias for `scan`, not a way
to bypass the voltage checks.

Only after the short tests and supply/load/temperature checks are satisfactory,
`pa 10000` explicitly requests the longer test with a **10-second guard**.
Normal clock shutdown begins around 9.95 seconds, with I2C/RTOS overhead;
actual RF pulse width is not measured by this firmware. This command has a
10-second cooldown, then latched auto-arm eligibility can return; the next
command still requires a fresh prekey check. It must never be treated as a
repeating carrier mode. The LPF precheck does not prevent overheating or
excessive current, and does not monitor the LPF during that longer burst.
There is no `leak 10000` mode.

For ordinary `pa`, CLK1 is off during the burst. The firmware programs and
checks the clock plan with CLK0 off, arms an independent timer, then enables
CLK0. It normally requests clock shutdown after about 80/230 ms for the
100/250 ms selections, leaving time for the I2C shutdown before the timer
requests G48 LOW. Those are software timing targets, **not measured RF pulse
widths**. The logged command interval is also not an RF-envelope measurement.
No claim of RF output power, PA efficiency, or hardware protection follows
from `PA JOB COMPLETE`.

`off` requests power off and inhibits automatic arming until `scan`
or a fault-free reboot. Invalid/rejected commands also inhibit and request
power off; do not request another burst before cooldown finishes. During an
active job, any received USB input other than CR/LF-only terminators requests
immediate power cutoff;
do not type `status` while a burst/preparation is running. The console also
recognizes Ctrl-C/Escape, although a terminal program may intercept those keys.
The independent burst guard does not depend on receiving keyboard input;
initial power-on/LPF scanning and burst preparation have a three-second guard.
Closing the monitor is not an emergency power disconnect and does not power
down the idle RF board.

### Optional TX-state leakage capture

After a satisfactory ordinary PA check and cooldown, wait for `ARMED_AUTO`
and enter `leak 100` (or later `leak 250`). G47 remains LOW. CLK1 runs at
28.296 MHz, corresponding to 7.074 MHz receive mixing; CLK0 remains 7.075 MHz, so a coupled
response may appear at 1 kHz. This intentionally retains CLK1 during TX only
for the diagnostic, unlike ordinary `pa`.

Before keying, I2S starts at 48 kHz and discards **one second** with CLK0 OFF,
then records a 100 ms background measurement. A baseline with nonzero padding
or exact ADC rail hits prevents keying. During the burst, the test drains
35 ms to clear stale DMA samples, then captures 50/200 ms for the 100/250 ms
guard settings. Capture statistics and 1/2 kHz tone estimates print only after
the normal RF-off request; the underlying driver can still report transfer
errors. Any capture or clock error requests shutdown, without automatic retry.

Compare `PA_OFF_BASELINE` and `PA_KEYED_LEAK`, checking padding, rail hits and
whether the 1 kHz tone is above background. This is a **TX-state leakage
observation, not normal RX sensitivity or input-path validation**: the RX switch
is open, the physical coupling route is unknown, and absence of a tone may
mean good isolation. Short windows do not guarantee analog settling or sample
continuity, and no rail hits does not exclude clipping elsewhere. Leakage
amplitude is not a universal board pass/fail threshold.

### Shutdown and faults

Successful bursts verify Si5351 register 3 is `0xFF`, leave CLK0/CLK1 OFF, and
keep G48 HIGH through cooldown and idle, without LPF ADC sampling. G47 remains LOW.
After leakage capture, cleanup stops I2S, resets its pins and disables pulls
to reduce unnecessary drive/back-powering. An error, failed LPF precheck,
guard expiry or operator `off` requests G48 LOW before potentially blocking
cleanup. There is no LPF-triggered cutoff after a successful prekey check.

A persistent pending marker is stored before initial RF power and before each
attempted transmission. The scan clears it only after verifying clocks off;
the burst clears it only after confirmed clock shutdown. No flash/NVS writes
take place while CLK0 is intentionally enabled. Resetting during an unverified
scan or burst therefore leaves a persistent fault that refuses scanning and
arming, including after a serial reset or ordinary reboot. Failure to contact
the Si5351 after applying power can also latch a fault because the initial
output-off state could not be verified.

After that fault, disconnect **all hardware power sources**, including RF
battery/DC and Tab5 USB/battery power, and allow the rails to discharge before
reconnecting and booting. Disconnect signal/back-power connections as needed
to make the RF board genuinely unpowered. Diagnose the logged failure first.
After that full power cycle and safe reassembly, boot with the fault latched
and enter `clearfault powercycled` to acknowledge recovery manually; the firmware
does not sense that a power cycle occurred. This command leaves RF power off;
then enter `scan` separately to resume qualification. A Tab5 reset or USB
reconnection alone neither clears the stored fault nor proves RF power was lost.
G48 LOW requests a switch action; it does not prove zero rail voltage or
immediate RF cessation because of capacitance,
back-power paths, GPIO reset states or hardware faults. Suitable hardware
default biasing and physical power removal remain necessary.

## RX leakage loopback (BS170s absent)

The final `TAB5 DX RF V1.11 CIRCUIT SCHEMATIC.pdf` supplied by Barb separates
the 74ACT244 output/TP8/BS170 gate net from TP7/common drains. With all three
BS170s absent, the intended PA connection is missing; only uncontrolled stray
coupling is available. G47 HIGH connects the RF node through C30 and Q1 to
TP5/RX_IN, then C25, the QSD, I/Q amplifiers and AC-coupled ADC inputs.
G47 LOW disconnects that RF input path. It is not a direct CLK0-to-ADC wire.

**Keep every BS170 absent and the antenna disconnected. Do not jumper TP8
to TP5 or the ADC.** This firmware is not safe for a populated PA: it keeps
RX selected while a test clock is active, has no LPF interlock, and restarts
the sequence automatically on boot. Power down and replace it before fitting
PA transistors. An LPF and dummy load alone do not make simultaneous PA/RX
operation safe or validate this test's coupling level.

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx/tab5
idf.py -B build-loopback -D SDKCONFIG=sdkconfig.loopback \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.loopback.defaults' build
idf.py -B build-loopback -p /dev/cu.usbmodem101 flash
cd ..
python tools/i2s_diagnostic.py --tab5 /dev/cu.usbmodem101 --seconds 25 --rf-clocks loopback --startup-ms 1000
```

G48/G47 remain HIGH throughout. The QSD runs at 28.296 MHz for an effective
7.074 MHz RX frequency. The source sequence is CLK0 OFF, 7.075 MHz ON, OFF,
7.076 MHz ON, then 7.075 MHz ON held for scope probing. Each step drains
one second of ADC samples before measuring a one-second window. It logs
AC RMS, mean, extrema, exact full-scale rail hits and coherent 1/2 kHz
amplitudes/relative phase. The final source is a nominal 1 kHz beat. Regular
continuous I2S statistics then resume with a one-second startup discard.

Compare the target bin against both OFF measurements and check whether the
response shifts from 1 to 2 kHz with CLK0. Phase is meaningful only when a
tone is clearly above noise. No measured leakage level, quadrature accuracy,
RF sensitivity or coupling route is guaranteed by merely completing the test;
internal clock/digital feedthrough can also produce a response. Absence of
full-scale ADC codes does not rule out earlier analog clipping.

Scope TP5 for leaked 7.075 MHz RF; TP10/TP11 are the schematic's I/Q amplifier
outputs where a 1 kHz tone may be easier to see (they carry DC bias, so start
with a high-impedance x10 probe and use AC coupling to inspect small AC).
Use a short ground connection. TP8 is the much stronger buffer/gate-drive
node, not the RX input. Do not interpret a large TP8 signal as successful RX.

After the project-directory rename, old CMake caches still reference
`tab5_dxft8`. This profile deliberately uses a fresh `build-loopback` directory;
old builds and historical evidence are preserved. For other profiles, use
a fresh `-B` directory if the existing cache still points to the old path.

## Persistent I2S diagnostic for the real RF board

Use this profile for manual scope measurements, with BS170s still absent.
Unlike the CLK0 scope profile (which disables I2S), it keeps the three I2S
clocks and RF-board power on continuously. G48 and G47 stay HIGH. The current
profile retains CLK1/QSD at 28.296 MHz (the original 7.074 MHz RX test plan);
CLK0 and other outputs stay disabled. Startup discard is 1,000 ms (48,000
frames), restoring the successful comparison-1 setting after the
[250 ms comparison](../validation/2026-09-18-i2s-250ms-comparison.md)
reproduced the original premature short-capture failure. I2S clocking,
format and one-second statistics windows are unchanged.

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx/tab5
idf.py -B build-i2s-diag -D SDKCONFIG=sdkconfig.i2s-diag \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.i2s-diag.defaults' build
idf.py -B build-i2s-diag -p /dev/cu.usbmodem101 flash
cd ..
python tools/i2s_diagnostic.py --tab5 /dev/cu.usbmodem101 --seconds 10 --rf-clocks rx --startup-ms 1000
```

`DXFT8_I2S_DIAGNOSTIC_RX_CLOCK` controls CLK1. The separate
`DXFT8_I2S_DIAGNOSTIC_STARTUP_MS` selects startup discard (0..5000 ms).
If a local configuration still has the comparison-2 setting, set startup to
1000 in `idf.py -B build-i2s-diag menuconfig` before rebuilding/flashing.
To reproduce the historical comparison only, set 250 and pass `--startup-ms 250`
to the runner. For the original all-Si5351-outputs-off baseline, disable
the RX-clock option and pass `--rf-clocks off`. The runner verifies the
requested RF-clock state and startup frame count at 48 kHz. Existing local
sdkconfig settings override defaults.

The ADC diagnostic drains and discards the configured startup frames while
clocks run, then continuously receives one-second windows. It logs raw 32-bit
first/last samples, signed 24-bit
minimum/maximum/mean, nonzero low padding bytes, and consecutive-word changes
for each channel. It separately summarizes the first 100 ms after discard,
matching the original short capture duration, and reports the first change
from the initial sample on each channel (zero-based post-discard frame offset;
-1 means no change in the first one-second window). Extra prefix logs are
deferred until the first window is complete. A short raw dump from that window
helps inspect framing. Inspect the prefix as well as the full windows: a
flat prefix can be hidden by later variation within a one-second window.
Flat samples and padding anomalies produce warnings without shutting down
power or clocks. Actual API/transport failures still stop the test and request
power off. A running harness is **not** a passing ADC or analog-path test.
No clock frequency or amplitude is physically measured by these UART logs.

Probe with x10, high-impedance probes and a short board-ground connection:

| Signal | Tab5 | PCM1808 pin | Nominal frequency | Suggested starting timebase |
| --- | --- | --- | --- | --- |
| SCKI/MCLK | G16 | 6 | 12.288 MHz | 100 ns/div |
| BCK | G45 | 8 | 3.072 MHz | 500 ns/div |
| LRCK | G3 | 7 | 48 kHz | 5 us/div |
| DOUT | G4 | 9 | Data, not a periodic clock | Compare with BCK/LRCK |

Begin with one clock at a time; DC coupling, about 1 V/div and a rising-edge
trigger near 1.5 V are useful starting settings. Verify the signal at the
ADC-side pad as well as the M5-Bus. Two scope channels suffice for pairwise
comparisons. The scope's sample-rate rating is not its analog bandwidth.

The diagnostic repeats after reset and leaves power on when the serial
capture closes. Power off to stop it, or flash another profile. This is a
temporary bench mode, not a production shutdown or LPF/TX safety system.

## Real RF board: 14.075 MHz CLK0 scope test

**Use this profile only with the BS170s uninstalled.** It is not transmit
firmware and does not implement the LPF/band interlock. The carrier starts
again on every boot; replace this build before fitting the PA transistors.
Disconnect the mock before connecting the real board (both use address 0x60).

Barb's September 2026 *TAB5 DX Building and Programming port Info.pdf* maps
G48 HIGH to main RF-board power enabled, and G47 HIGH to RX on / TX off.
G48 is not the power source: the board still requires its battery-positive
lead or an appropriate DC-input supply. Verify the supply, polarity, common
ground, and M5-Bus header orientation before applying power.

This separate configuration leaves the default mock build intact:

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx/tab5
idf.py -B build-scope -D SDKCONFIG=sdkconfig.scope \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.scope.defaults' build
idf.py -B build-scope -p /dev/cu.usbmodem101 flash
cd ..
python tools/scope_validate.py --tab5 /dev/cu.usbmodem101 --runs 3 --i2c-only
```

The scope build first sets G48 LOW and G47 HIGH, then enables G48 and waits
200 ms for startup. It runs the existing Si5351 readback/lock checks with the
7.074 MHz RX clock plan. The current scope profile explicitly disables I2S:
the first powered PCM1808 test returned constant -1 on both channels with its
old 250 ms discard. Later I2S diagnostics reproduced that early flat interval;
this independent scope profile still does not claim an ADC pass. On success it
disables Si5351 outputs during reprogramming and leaves **CLK0 only** enabled
at nominal 14,075,000 Hz. G47 stays HIGH throughout; no TX selection occurs.

To investigate the ADC later, use `idf.py -B build-scope menuconfig` to enable
`DXFT8_RUN_I2S_SELF_TEST` with `DXFT8_I2S_CAPTURE_STATS`, rebuild/flash, and run
the runner without `--i2c-only`. That build captures 4,800 frames after one
second of startup discard (48,000 frames at 48 kHz) and requires a varying,
correctly padded sample stream before enabling the carrier. Existing local
sdkconfig settings override defaults;
use menuconfig to disable I2S when switching back to the independent CLK0 test.

The carrier helper uses PLLA = 788.2 MHz and integer MS0 = 56 with a nominal
26 MHz reference; spread spectrum is off, and register 3 ends at `0xFE`.
All unused output drivers are powered down. Probe **CLK0 / TP1** relative to
board GND with a high-impedance ×10 probe (not a 50-ohm load). Expected nominal
period: about 71.05 ns. Register readback and PLL lock do not independently
measure the physical output frequency or its amplitude.

Any failed check requests all Si5351 outputs off and G48 LOW. If address 0x60
does not answer, an address-only scan logs other responding internal-bus
devices before shutdown. Restore the missing supply/connection and reset to
retry. Software output initialization cannot guarantee safe GPIO levels
during reset; the final hardware still needs suitable default biasing.

## Wiring for this milestone

Power the Tab5 and YD-ESP32-23 separately over USB. Connect the internal I2C
bus through M5-Bus:

| Tab5 M5-Bus | RF-board mock |
| --- | --- |
| Pin 1 or pin 3 / GND | GND |
| Pin 17 / GPIO31 / SDA | GPIO8 / SDA |
| Pin 18 / GPIO32 / SCL | GPIO9 / SCL |

The Tab5 is the I2C master on its internal system bus at 100 kHz. Onboard
R76/R93 already pull SCL/SDA up to 3.3 V through 2.2 kOhm, so the weak internal
pull-ups are disabled on both boards. Address `0x60` is separate from the
Tab5's onboard peripherals. This standalone firmware owns the bus; future
GUI integration must share the existing bus handle with the board drivers.
ESP-IDF may print a generic pull-up warning when internal pull-ups are
disabled; that warning does not measure or detect the onboard resistors.

For the I2S test, add the following M5-Bus wiring:

| Signal | Tab5 M5-Bus / GPIO | Direction | YD mock |
| --- | --- | --- | --- |
| GND | any M5-Bus GND | — | GND |
| MCLK / SCKI | pin 2 / GPIO16 | -> | GPIO4 |
| BCLK | pin 8 / GPIO45 | -> | GPIO5 |
| LRCK / WS | pin 19 / GPIO3 | -> | GPIO6 |
| ADC DOUT / Tab5 DIN | pin 20 / GPIO4 | <- | GPIO7 |

Power each board only from its own USB connection. Do not join 5 V or 3.3 V.
Keep jumpers short; optional 22–47 ohm source-series resistors can be fitted at
the Tab5 end of its three clock outputs and at YD GPIO7 for DOUT.

For the previous Port A setup, move SDA to yellow GPIO53, SCL to white GPIO54,
and GND to black; leave red 5 V disconnected. In `idf.py menuconfig`, set
`CONFIG_DXFT8_TAB5_I2C_SDA_GPIO=53`,
`CONFIG_DXFT8_TAB5_I2C_SCL_GPIO=54`, and enable
`CONFIG_DXFT8_TAB5_I2C_INTERNAL_PULLUPS` for short bench jumpers unless external
pull-ups to 3.3 V are fitted. The YD remains on GPIO8/GPIO9 with its weak
pull-ups disabled.

## Build and run

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx/tab5
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

After flashing both boards, close serial monitors and run the automated
capture from the repository root:

```sh
cd ~/tab5_dx
python tools/bench_validate.py --mock /dev/cu.usbmodem5A7A0113341 --tab5 /dev/cu.usbmodem101 --runs 3
```

The runner resets the mock once and the Tab5 for each run, streaming both
logs to the console. It requires Si5351 and I2S success messages on the
internal I2C pins, independent Si5351 and MCLK checks by the mock, and
clock-loss detection after shutdown. Initial USB-open startup failures and
delayed mock validation errors also fail the runner.

The deterministic startup test:

1. Probes address `0x60` for up to two seconds to allow board startup, then
   reads status registers 0 and 1.
2. Disables every output with Si5351 register 3 = `0xFF`, powers down the
   output drivers, and applies the configured XA reference mode.
3. Programs the actual 40 m plan for RF 7.074 MHz: PLLA/B at 792.288 MHz,
   TX CLK0 at 7.074 MHz from PLLA, and RX/QSD CLK1 at 28.296 MHz from PLLB.
4. Reads every persistent register back in separate one-byte transactions and
   checks PLL reset behavior, then requires three consecutive clean lock-status
   reads before enabling a clock output.
5. Enters RX with register 3 = `0xFD`: CLK1 on and CLK0 off.
6. When `CONFIG_DXFT8_RUN_TX_PATH_SELF_TEST` is enabled, briefly exercises
   `RX (0xFD) -> TX-ready clocks (0xFC) -> RX (0xFD)`. The QSD CLK1 remains
   running while CLK0 is added for the PA clock.
7. Starts I2S1 as a 48 kHz master receiver, generating 12.288 MHz MCLK,
   3.072 MHz BCLK, and 48 kHz LRCK.
8. In the default mock mode, discards 4,800 startup frames to clear both DMA
   rings, an in-flight mock write, and MCLK qualification transitions across
   host-only resets. It then validates 4,800 continuous stereo frames for
   channel order, Philips-I2S timing/alignment, zero padding, and sequence.
9. Releases I2S (including MCLK/APLL) and finishes in RX with CLK1 enabled and
   CLK0 disabled.

Any failure requests all Si5351 outputs off and leaves the error visible in the
UART log. Register 3 output enables are active-low: `0xFF` disables all
outputs, `0xFD` enables CLK1 only, and `0xFC` enables CLK0 and CLK1. These are
clock states, not complete RF-path states. Per Barb's September assembly
notes, G47 selects RX/TX and G48 controls main RF power; they are not
complementary T/R outputs. The default mock profile does not drive these pins.

For a mock-board acceptance run, enable
`CONFIG_DXFT8_RUN_TX_PATH_SELF_TEST` with `idf.py menuconfig`. Its default is
off: never enable this option with a powered PA or antenna connected. The test
only changes Si5351 clock enables; it does not assert G47/G48. The default
mock profile finishes in RX, never TX-ready. The real-board scope profile
above is a separate, explicit CLK0-only exception with BS170s absent.

## I2S modes and production reuse

The default `CONFIG_DXFT8_I2S_MOCK_PATTERN_TEST` is for the YD board and will
correctly reject ordinary audio. For a real RF board, select
`CONFIG_DXFT8_I2S_CAPTURE_STATS` in `idf.py menuconfig`. That mode receives
normal PCM1808 data, converts the left-aligned words to signed 24-bit samples,
and reports left/right minimum, maximum, mean, and padding errors without
assuming an audio waveform. It first drains and discards one second of frames
(48,000 at 48 kHz) to avoid the startup interval observed in the real-board
comparison. This bench-tested margin is not a universal analog-settling guarantee.
It rejects constant channels and nonzero padding, but it is only a digital-link
sanity check—not a measurement of RX gain, noise, or analog performance.

The reusable API is in `main/include/pcm1808_i2s.h`. It configures the Tab5 as
I2S master RX and the PCM1808 as the external slave. Its production wire
format is fixed deliberately at stereo Philips I2S, 32-bit slots, and 64 BCLK
per frame. Only the top 24 bits are meaningful; the low byte is padding. I2S1
is selected to avoid claiming I2S0 used by common Tab5 internal-audio code.

To run only the already-validated I2C path while I2S is unwired, disable
`CONFIG_DXFT8_RUN_I2S_SELF_TEST`. An I2S timeout otherwise intentionally ends
the bring-up test with the Si5351 outputs requested off.

The real daughter board must strap the PCM1808 before power-up as assumed by
this driver: `MD1=0`, `MD0=0` for slave mode and `FMT=0` for 24-bit Philips
I2S. Firmware cannot read those three hardware-only inputs, so the first-board
test still needs their logic levels checked at the IC.

## Reference input configuration

The RF-board v1.3 uses a 26 MHz active oscillator, AC-coupled into the Si5351
XA pin with XB left unused. Its default configuration is:

- `CONFIG_DXFT8_SI5351_REFERENCE_HZ=26000000`
- `CONFIG_DXFT8_SI5351_EXTERNAL_REFERENCE=y`
- register 183 = `0x12` (0 pF load field plus the required low reserved bits)

The `0x12` value is specific to the active-XA arrangement; it replaces the
common `0xD2` 10 pF passive-crystal setting. The reference frequency and
6/8/10 pF passive-crystal modes remain selectable with `idf.py menuconfig` for
other Si5351 modules. Match those settings to the hardware rather than merely
copying the v1.3 default. Because the active-XA case is less common than a
passive crystal, verify the oscillator amplitude, startup, and frequency on
the first assembled RF board.

## Manufacturer references

- [Skyworks Si5351 data sheet](https://www.skyworksinc.com/-/media/SkyWorks/SL/documents/public/data-sheets/Si5351-B.pdf)
- [Skyworks AN619 register-calculation guide](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/AN619.pdf)
