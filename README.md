# Tab5 DXFT8 firmware bring-up

Working directory: `~/tab5_dx` (renamed from `~/tab5_dxft8`; Git history kept).

The [BS170-absent RX leakage loopback](tab5/README.md#rx-leakage-loopback-bs170s-absent)
keeps G47 HIGH/RX selected and compares CLK0 OFF with 1/2 kHz offset carriers
while CLK1 and I2S run. It uses a one-second discard and is not powered-PA
firmware. Never fit the BS170s while this test build is installed.
The [first loopback result](validation/2026-09-18-rx-loopback.md) shows strong
1/2 kHz frequency-following I/Q response with near-90-degree relative phase,
zero ADC rail hits in the measured windows and zero I2S padding errors.

This repository keeps the two sides of the bench setup separate:

- `rf_board_mock/` runs on the YD-ESP32-23 and emulates both the RF daughter
  board's Si5351A at I2C address `0x60` and its PCM1808 I2S output.
- `tab5/` runs on the M5Stack Tab5 and contains reusable Si5351 and PCM1808
  host drivers plus UART-only integration tests.

For live ADC troubleshooting, use the
[persistent I2S diagnostic profile](tab5/README.md#persistent-i2s-diagnostic-for-the-real-rf-board).
It leaves RF power and I2S clocks enabled, with either all Si5351 outputs off
or CLK1/QSD retained for a controlled comparison. The first
diagnostic capture showed varying left/right samples with zero padding errors;
see the [diagnostic record](validation/2026-09-18-i2s.md).
The [CLK1/QSD-on comparison](validation/2026-09-18-i2s-qsd-comparison.md) also
completed three restarts with varying samples and zero padding errors while
keeping the one-second startup wait unchanged.
The [250 ms startup comparison](validation/2026-09-18-i2s-250ms-comparison.md)
reproduced the original constant -1 short capture on all three restarts:
both channels began changing about 108 ms after the discard, then continued
with zero padding errors. This points to premature startup validation, not
a persistently dead I2S link. The current diagnostic profile and short-capture
real-ADC acceptance path now use the previously tested one-second discard
for normal bring-up. The 250 ms record remains a historical comparison;
wider cold-start and analog-path testing is still needed.

For the real RF daughter board's BS170s-absent 14.075 MHz scope test, use the
[separate real-board profile](tab5/README.md#real-rf-board-14075-mhz-clk0-scope-test).
It enables G48 power, holds G47 in RX and programs CLK0. The current scope
profile leaves I2S disabled after a constant-sample failure on the real ADC;
this is recorded separately and is not counted as an I2S pass.

For the mock I2C + I2S setup, power both boards independently over USB.
Connect I2C through the Tab5's internal M5-Bus port:

| Signal | Tab5 M5-Bus | YD-ESP32-23 |
| --- | --- | --- |
| GND | pin 1 or pin 3 | GND |
| SDA | pin 17 / GPIO31 | GPIO8 |
| SCL | pin 18 / GPIO32 | GPIO9 |

The internal bus already has 2.2 kOhm pull-ups to 3.3 V; both boards' weak
internal pull-ups are disabled. Do not connect the boards' power rails.

Port A remains an alternative: connect its black GND, yellow GPIO53/SDA, and
white GPIO54/SCL to the same mock pins, leaving red 5 V disconnected. Set the
Tab5 SDA/SCL configuration back to GPIO53/GPIO54 and enable its weak internal
pull-ups for the previously validated short-wire setup (or provide external
pull-ups to 3.3 V). The mock configuration stays unchanged.

The Tab5 test uses the RF-board v1.3 active 26 MHz reference coupled into the
Si5351 XA input. It validates ordinary register access, safe output
initialization, fractional synthesis, the 7.074 MHz transceiver clock plan,
readback, PLL reset behavior, and the board's clock-enable states:

- CLK0: 7.074 MHz TX/PA clock from PLLA.
- CLK1: 28.296 MHz RX/QSD clock from PLLB.
- Si5351 register 3: `0xFF` for all clocks off, `0xFD` for RX (CLK1 only),
  and `0xFC` for the TX-ready clock state (CLK0 and CLK1).

CLK1 remains running in the mock's TX-ready clock state. Barb's September
assembly notes instead specify G47 as RX/TX selection and G48 as main RF
power, with CLK1 off for TX. The mock's clock-mask exercise is not that full
hardware transmit sequence. The optional TX clock-path exercise is disabled
by default and unavailable in real-board mode. See each
subdirectory's README for build, flash, configuration, and safety details.

The active-XA default writes register 183 as `0x12` (0 pF load setting). Both
the reference frequency and input/load mode are configurable for development
modules that instead use a passive crystal. Do not carry the `0x12` setting
over blindly when changing the reference hardware.

## I2S bench wiring

Alongside the internal I2C wiring, add these M5-Bus I2S connections. The same
common ground serves both interfaces. Do not connect the boards' 5 V or 3.3 V
rails.

| Signal | Tab5 M5-Bus / ESP32-P4 | Direction | YD-ESP32-23 mock |
| --- | --- | --- | --- |
| GND | pin 1, 3, or another GND | — | GND |
| MCLK / PCM1808 SCKI | pin 2 / GPIO16 | Tab5 -> mock | GPIO4 |
| BCLK | pin 8 / GPIO45 | Tab5 -> mock | GPIO5 |
| LRCK / WS | pin 19 / GPIO3 | Tab5 -> mock | GPIO6 |
| PCM1808 DOUT | pin 20 / GPIO4 | mock -> Tab5 | GPIO7 |

The shared wire format is 48 kHz stereo Philips I2S with 24 significant bits
left-aligned in each 32-bit slot: MCLK = 12.288 MHz (256fs), BCLK = 3.072 MHz
(64fs), and LRCK = 48 kHz. The mock emits a continuous self-checking pattern.
The Tab5 verifies channel order, the Philips one-bit delay, 24-bit alignment,
zero padding, and uninterrupted frame sequence. It first discards 4,800
startup frames to clear buffered samples and MCLK qualification transitions
after a host-only reset, then checks 4,800 continuous stereo frames.
The mock separately counts
MCLK on GPIO4 and reports whether it is near 12.288 MHz; by default it withholds
valid pattern data until that clock passes and replaces the pattern with silence
if MCLK is later lost, so the Tab5 cannot pass without the MCLK connection.

On the real daughter board, that format assumes the PCM1808 hardware straps
are `MD1=0`, `MD0=0` (slave) and `FMT=0` (Philips I2S).

For flying-wire tests, keep all wires short and run ground next to the clock
signals. Optional 22–47 ohm series-damping resistors belong at each signal's
source: at the Tab5 end of MCLK/BCLK/LRCK and at the YD GPIO7 end of DOUT.
They are not pull-downs and must not be wired from a signal to ground.

## Bench validation

The internal I2C + I2S setup was built, flashed, and tested on 2026-09-07,
including 1.92 million checked frames in extended captures and repeated
Tab5 resets. See the [validation record and serial logs](validation/2026-09-07.md)
for the startup fixes, test configuration, and final firmware results.

After building and flashing both boards, close any serial monitors and run:

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dx
python tools/bench_validate.py --mock /dev/cu.usbmodem5A7A0113341 --tab5 /dev/cu.usbmodem101 --runs 3
```

The runner checks any startup triggered by opening USB, resets the mock once,
then resets only the Tab5 for each run and streams both serial logs to the
console. Each pass requires Tab5 Si5351 and I2S success messages, the internal
GPIO31/GPIO32 configuration, the mock's independent Si5351 and MCLK checks,
and MCLK-loss detection after the host stops its clocks. Firmware errors or
dropped transaction logs fail the run.

The mock arms I2S before enabling its I2C address so host traffic cannot delay
I2S initialization. The host allows up to two seconds for the I2C device to
become available when both boards boot together.

The earlier Port A I2C flow was built, flashed, and exercised on 2026-09-04 with
the YD-ESP32-23 mock and a Tab5. The Tab5 passed address probing, status reads,
all programmed-register readbacks, PLL-reset handling, and stable-lock checks.
The mock independently decoded PLLA/B at 792.288 MHz, CLK0 at 7.074 MHz, and
CLK1 at 28.296 MHz; it accepted the `0xFD -> 0xFC -> 0xFD` state sequence with
no validation errors or dropped transaction logs. The test ended in RX
(`0xFD`: CLK1 on, CLK0 off).

Register programming follows the manufacturer documentation:

- [Skyworks Si5351 data sheet](https://www.skyworksinc.com/-/media/SkyWorks/SL/documents/public/data-sheets/Si5351-B.pdf)
- [Skyworks AN619 register-calculation guide](https://www.skyworksinc.com/-/media/Skyworks/SL/documents/public/application-notes/AN619.pdf)
