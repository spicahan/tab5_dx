# Tab5 RF-board host bring-up

Native ESP-IDF 5.5.1 firmware for exercising the RF-board mock or the
real Si5351A and PCM1808 from the M5Stack Tab5. It is a UART-only bring-up
program with no GUI, M5GFX, M5Unified, Wi-Fi, or other managed-component
dependency.

## Persistent I2S diagnostic for the real RF board

Use this profile for manual scope measurements, with BS170s still absent.
Unlike the CLK0 scope profile (which disables I2S), it keeps the three I2S
clocks and RF-board power on continuously. G48 and G47 stay HIGH. The current
comparison-1 profile retains CLK1/QSD at 28.296 MHz (the original 7.074 MHz RX
test plan); CLK0 and other outputs stay disabled. The one-second I2S startup
discard and all I2S settings are unchanged from the all-RF-clocks-off baseline.

```sh
source ~/esp/esp-idf/export.sh
cd ~/tab5_dxft8/tab5
idf.py -B build-i2s-diag -D SDKCONFIG=sdkconfig.i2s-diag \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.i2s-diag.defaults' build
idf.py -B build-i2s-diag -p /dev/cu.usbmodem101 flash
cd ..
python tools/i2s_diagnostic.py --tab5 /dev/cu.usbmodem101 --seconds 10 --rf-clocks rx
```

`DXFT8_I2S_DIAGNOSTIC_RX_CLOCK` selects this comparison. To restore the earlier
all-Si5351-outputs-off baseline, disable that option in
`idf.py -B build-i2s-diag menuconfig`, rebuild/flash, and pass `--rf-clocks off`
to the runner. The runner verifies both the requested RF-clock state and the
48,000-frame startup discard. Existing local sdkconfig settings override defaults.

The ADC diagnostic discards one second at startup, then continuously receives
one-second windows. It logs raw 32-bit first/last samples, signed 24-bit
minimum/maximum/mean, nonzero low padding bytes, and consecutive-word changes
for each channel. A short raw dump from the first window helps inspect framing.
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
cd ~/tab5_dxft8/tab5
idf.py -B build-scope -D SDKCONFIG=sdkconfig.scope \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.scope.defaults' build
idf.py -B build-scope -p /dev/cu.usbmodem101 flash
cd ..
python tools/scope_validate.py --tab5 /dev/cu.usbmodem101 --runs 3 --i2c-only
```

The scope build first sets G48 LOW and G47 HIGH, then enables G48 and waits
200 ms for startup. It runs the existing Si5351 readback/lock checks with the
7.074 MHz RX clock plan. The current scope profile explicitly disables I2S:
the first powered PCM1808 test returned constant -1 on both channels, so ADC
validation remains unresolved and is not claimed by this test. On success it
disables Si5351 outputs during reprogramming and leaves **CLK0 only** enabled
at nominal 14,075,000 Hz. G47 stays HIGH throughout; no TX selection occurs.

To investigate the ADC later, use `idf.py -B build-scope menuconfig` to enable
`DXFT8_RUN_I2S_SELF_TEST` with `DXFT8_I2S_CAPTURE_STATS`, rebuild/flash, and run
the runner without `--i2c-only`. That build captures 4,800 frames after 12,000
startup frames and requires a varying, correctly padded sample stream before
enabling the carrier. Existing local sdkconfig settings override defaults;
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
cd ~/tab5_dxft8/tab5
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

After flashing both boards, close serial monitors and run the automated
capture from the repository root:

```sh
cd ~/tab5_dxft8
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
assuming an audio waveform. It first discards 12,000 frames so the PCM1808's
post-clock digital-filter mute interval cannot be mistaken for valid silence.
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
