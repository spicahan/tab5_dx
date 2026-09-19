# I2S comparison 1: retain CLK1/QSD, keep one-second startup — 2026-09-18

Result: all three explicit Tab5 restarts completed with live data on both
channels, zero padding errors, and no return of the constant -1 stream.
Twenty-four 48,000-frame windows were inspected: 1,152,000 stereo frames.
This supports I2S operation with CLK1/QSD active at the one-second startup
wait. It does not yet establish the cause of the earlier startup failure.

## Controlled change

Baseline: [all RF clocks off](2026-09-18-i2s.md). Before requesting this
comparison, the user reported all three I2S clocks measured correctly and
DOUT appeared correct on the scope.

The only acquisition-condition change for comparison 1 was keeping Si5351
CLK1/QSD enabled at nominal 28.296 MHz, the original 7.074 MHz RX test plan.
CLK0 and all other Si5351 outputs remain disabled. Register 3 = 0xFD was
read-back verified. G48 and G47 remain HIGH.

Unchanged from baseline:

- ADC receive code and headers (no source diff).
- One-second startup discard: 48,000 frames, explicitly checked by runner.
- 48 kHz stereo Philips I2S, 32-bit slots carrying 24-bit samples.
- MCLK G16 = nominal 12.288 MHz; BCLK G45 = nominal 3.072 MHz;
  LRCK G3 = nominal 48 kHz; DOUT into G4.
- One-second statistics windows, initial raw dump, padding/flat diagnostics.
- Nonfatal handling of sample-quality anomalies and fail-closed handling of
  actual API/transport errors.

`DXFT8_I2S_DIAGNOSTIC_RX_CLOCK=y` selects the comparison and is enabled in the
current tracked diagnostic profile and installed firmware. Setting it to `n`
restores the baseline. The serial runner now requires `--rf-clocks rx` for this
state; it still supports `--rf-clocks off` for the earlier baseline.

## Build and hardware evidence

ESP-IDF v5.5.1 build succeeded and flash hash verification passed. Tab5 is the
ESP32-P4 revision 1.3 at `/dev/cu.usbmodem101`. Installed ELF SHA256 prefix:
`50a2e418f`.

Each run captured ten seconds of UART output following an explicit USB reset,
including eight complete 48,000-frame diagnostic windows. Every window in
every run varied on both channels and had padding_nonzero=0. There were no
sample-quality anomaly messages, firmware errors or unexpected restart loops.

| Run | Stereo frames | Padding errors | Flat channels | Final-window L range | Final-window R range |
| --- | ---: | ---: | --- | --- | --- |
| 1 | 384,000 | 0 | None | -17,167..16,023 | -20,459..18,931 |
| 2 | 384,000 | 0 | None | -15,732..15,039 | -19,589..22,136 |
| 3 | 384,000 | 0 | None | -15,855..16,239 | -18,290..19,021 |

The original all-clocks-off baseline also varied on both channels with zero
padding errors. Its later right-channel range was much smaller, and its early
windows included transients. Without a controlled analog stimulus or identical
input conditions, changes in sample amplitude do not establish analog gain,
noise performance, sensitivity or I/Q balance. This logging diagnostic also
does not certify lossless DMA continuity or measure physical clock frequency.

- [Run 1](2026-09-18-i2s-qsd-on-1.log)
- [Run 2](2026-09-18-i2s-qsd-on-2.log)
- [Run 3](2026-09-18-i2s-qsd-on-3.log)

Repeat with:

```sh
python tools/i2s_diagnostic.py --tab5 /dev/cu.usbmodem101 --seconds 10 --rf-clocks rx
```

## Handoff

No additional resets after run 3. The firmware remains in continuous RX-clock
diagnostic mode: CLK1/QSD on, CLK0 off, I2S active, G48/G47 HIGH. BS170s must
remain absent for this bench setup.

Comparison 2 (shortening startup discard to 250 ms while retaining CLK1) has
not been implemented or run. That is the next controlled test, rather than
concluding that the longer startup wait alone resolved the earlier failure.
