# Tab5 RF-board host bring-up

Native ESP-IDF 5.5.1 firmware for exercising the RF-board mock—and later the
real Si5351A and PCM1808—from the M5Stack Tab5. It is a UART-only bring-up
program with no GUI, M5GFX, M5Unified, Wi-Fi, or other managed-component
dependency.

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
clock states, not complete RF-path states. The daughter board's separate G47
and G48 controls perform the actual complementary RX/TX switching; exercising
those GPIOs is outside these I2C/I2S tests.

For a mock-board acceptance run, enable
`CONFIG_DXFT8_RUN_TX_PATH_SELF_TEST` with `idf.py menuconfig`. Its default is
off: never enable this option with a powered PA or antenna connected. The test
only changes Si5351 clock enables; it does not assert G47/G48. The normal final
state is RX, never TX-ready.

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
