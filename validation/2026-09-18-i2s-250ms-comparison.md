# I2S comparison 2: 250 ms startup with CLK1/QSD on — 2026-09-18

Result: the original short-capture failure is reproducible. On all three
explicit Tab5 USB resets, both channels remained at signed 24-bit **-1**
through the entire first 100 ms after a 250 ms discard. Both then began
changing about 108 ms after the discard. The link continued running with
zero padding errors and no transport failures.

The 250 ms startup + 100 ms acceptance test is too early for this observed
startup sequence. Looking only at one-second windows would hide the failure:
every full window varies, including the first one. The earlier
[one-second startup comparison](2026-09-18-i2s-qsd-comparison.md) passed all
three restarts with the same CLK1/QSD state.

## Controlled conditions and instrumentation

The requested acquisition change was reducing startup discard from 48,000
frames (1000 ms) to 12,000 frames (250 ms). The driver drains the receiver
throughout discard; this is not a delay with clocks stopped.

Unchanged:

- CLK1/QSD = nominal 28.296 MHz, the original 7.074 MHz RX test plan;
  CLK0 and other Si5351 outputs disabled, register 3 readback = 0xFD.
- G48 HIGH for RF power, G47 HIGH for RX / TX off; BS170s remain absent.
- Internal I2C: GPIO31 SDA / GPIO32 SCL.
- I2S1 master RX: GPIO16 MCLK, GPIO45 BCLK, GPIO3 LRCK, GPIO4 DOUT input.
- 48 kHz stereo Philips I2S, 32-bit slots with 24-bit significant samples;
  nominal MCLK 12.288 MHz, BCLK 3.072 MHz.
- One-second capture windows and persistent clocks after capture closes.
- Startup power sequencing and the existing post-discard UART log.

Added instrumentation snapshots the first 4,800 post-discard frames within
the first one-second window and records each channel's first change from
the initial value. It does not perform a separate capture. These extra logs
are emitted only after that first window completes, so the new UART output
does not interrupt the initial 100 ms. The runner verifies the startup
count, 48-kHz setting, clock state and presence of these new markers.

## Measured results

| Reset run | First 100 ms, L/R | First-change offset L/R | Time after discard L/R | Complete stereo frames | Padding errors |
| --- | --- | --- | --- | ---: | ---: |
| 1 | Both constant -1 | 5175 / 5176 | 107.813 / 107.833 ms | 432,000 | 0 |
| 2 | Both constant -1 | 5173 / 5175 | 107.771 / 107.813 ms | 432,000 | 0 |
| 3 | Both constant -1 | 5175 / 5176 | 107.813 / 107.833 ms | 432,000 | 0 |

Offsets are zero-based within delivered post-discard samples. Each initial
100 ms has L/R min=max=mean=-1, zero changes, and raw first/last words
`FFFFFF00/FFFFFF00`; the eight least-significant padding bits are zero.

Each run captured ten seconds of UART after reset and yielded nine complete
48,000-frame windows, totaling **1,296,000 stereo frames**. The 4,800-frame
prefixes are subsets of those windows, not additional frames. All full
windows varied on both channels. No firmware error, read timeout, unexpected
restart or nonzero padding word appeared. The runner's `HARNESS OK` is not
an ADC-quality pass: the prefix explicitly fails the original variation test.

Adding the configured 250 ms discard to the first-change offsets gives
about **357.77–357.83 ms in the delivered sample timeline**. This is not a
scope measurement of delay from ADC rail rise or first physical clock edge,
nor proof that analog settling is complete at the first changing sample.
The first full-window means are still roughly +3200 (L) / +3500 (R), then
drop substantially, reinforcing that first variation alone is not a
settling criterion.

## Interpretation and next step

The repeatable flat-prefix-then-live-data behavior, together with the
successful one-second comparison, strongly supports premature startup
acceptance as the explanation for the earlier constant -1 failure. It
does not support treating that short capture as evidence of a persistently
broken I2S connection or CLK1/QSD incompatibility.

There is a plausible device-level explanation. TI specifies 1024 SCKI
cycles of reset release, followed by 8960/Fs initialization. Its fade-in
logic can wait 8192/Fs without a zero crossing before forcing a 48/Fs fade.
At our clocks these are approximately 0.083 + 186.667 + 170.667 ms, then
a 1 ms forced fade. That timing is close to the observed transition.
This is a hypothesis, not a measured internal state: we have no synchronized
rail/clock/analog capture, and the observed -1 code differs from TI's stated
zero during reset. It does not establish why that exact code appears.
See [TI PCM1808 datasheet, sections 7.3.4, 7.4.1 and 7.4.2](https://www.ti.com/lit/ds/symlink/pcm1808.pdf).

For normal bring-up acceptance, retain the already-tested one-second
discard while clocks run and the receiver drains. A 400 or 500 ms minimum
is not established by this experiment, and one second is not a universal
guarantee across power, temperature and supply conditions. These are USB
resets with the existing brief RF-power-off sequence, not verified fully
discharged cold-power starts. A future cold-start check and known analog
or RF test signal are still needed to validate startup margin and the
analog path. These statistics do not certify DMA continuity, sensitivity,
I/Q balance, gain, phase, or channel mapping under a known stimulus.

## Build and reproduction

ESP-IDF v5.5.1 diagnostic build succeeded; flash hash verification passed on
the ESP32-P4 revision 1.3 at `/dev/cu.usbmodem101`. Installed ELF SHA256 prefix:
`f7e10cf9a`. Default mock-host and CLK0-scope profiles also rebuilt successfully
as compile regressions; neither was flashed. `git diff --check` passed.

Current tracked diagnostic defaults:

```text
CONFIG_DXFT8_I2S_CONTINUOUS_DIAGNOSTIC=y
CONFIG_DXFT8_I2S_DIAGNOSTIC_RX_CLOCK=y
CONFIG_DXFT8_I2S_DIAGNOSTIC_STARTUP_MS=250
```

Repeat after building/flashing this profile:

```sh
python tools/i2s_diagnostic.py --tab5 /dev/cu.usbmodem101 --seconds 10 --rf-clocks rx --startup-ms 250
```

- [Run 1 raw log](2026-09-18-i2s-250ms-1.log)
- [Run 2 raw log](2026-09-18-i2s-250ms-2.log)
- [Run 3 raw log](2026-09-18-i2s-250ms-3.log)

## Handoff

The 250 ms diagnostic remains installed to preserve the comparison state.
No additional reset or flash was performed after run 3. RF power and I2S
clocks remain active, CLK1/QSD on, CLK0 off, G48/G47 HIGH. This is bench
firmware without LPF/TX interlocks; keep BS170s absent. The separate legacy
short-capture acceptance path has not yet been changed to use a longer
discard, so enabling it would still use its original 250 ms behavior.
