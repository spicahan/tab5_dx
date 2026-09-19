# BS170-absent RX leakage loopback — 2026-09-18

Result: strong, source-correlated near-quadrature audio is present without
installing the BS170s. Moving CLK0 from 7.075 to 7.076 MHz moves the dominant
measured beat from 1 to 2 kHz with the receiver held at 7.074 MHz. Disabling
CLK0 removes the response, and returning to 7.075 MHz restores it.

This is convincing functional evidence of frequency-following I/Q reception
and digitization. It is not proof of the physical coupling route through TP5,
an RF sensitivity/isolation measurement, or a full analog-quality pass.

## Final schematic and safety decision

The user supplied the final file
`/Users/spica/Downloads/TAB5 DX RF V1.11 CIRCUIT SCHEMATIC.pdf` during this task.
Its title block reads Rev V1.1, dated 8/29/2026. The file, rather than the
earlier v1.3 drawing, was visually checked before the new firmware was flashed.
The September assembly notes separately identify G47 HIGH as RX selected.

The final drawing shows:

- CLK0 -> U8/74ACT244 -> C32 -> TP8 -> Q2/Q3/Q4 gate pads.
- TP7/common drains is a **different net**. With all three BS170s absent,
  there is no intended conductive gate-driver-to-drain connection.
- G47 HIGH turns series Q1 on, connecting the RF node through C30 and Q1
  to TP5/RX_IN. G47 LOW disconnects that path.
- TP5 feeds C25, U9/QSD, U10 I/Q amplifiers, C15/C20 and ADC input filtering,
  then PCM1808. It is not a direct buffered-CLK0-to-ADC connection.

The experiment relies on uncontrolled stray coupling. Keep all BS170s absent,
the antenna disconnected, and external RF drive absent. Do not jumper TP8 to
TP5 or the ADC. Coupling capacitors are not an RF attenuator. TP7 carries
battery DC through its RF choke even without the PA MOSFETs.

Do not fit the BS170s with this firmware installed. A populated PA with
CLK0 ON and G47 HIGH can expose the RX chain to the PA output; an LPF and
dummy load alone do not make that simultaneous connection safe. A future
powered-PA test needs separate sequencing, current/load checks, and a known
attenuated or directional-coupler path.

## Firmware and sequence

The repository is now `/Users/spica/tab5_dx`. Existing Git history, historical
logs, and old build directories were preserved. Their old absolute-path CMake
caches were not reused; a fresh `tab5/build-loopback` was built instead.
Current README commands use the new folder name. The internal project/binary
name remains `tab5_dxft8_bringup` to avoid unrelated renaming.

One-second startup discard is now the default for the continuous real-board
diagnostic and the separate real-ADC short-capture acceptance routine. The
mock pattern's own startup procedure was left unchanged. The ignored local
diagnostic configuration was also changed from 250 to 1000 ms.

The explicit loopback profile keeps G48 and G47 HIGH, I2S at 48 kHz, and
CLK1 at 28.296 MHz (4 x 7.074 MHz). It uses independent PLLA/CLK0 and PLLB/CLK1
settings, checks register readback and both PLL locks, and starts each newly
configured plan with CLK0 disabled. The caller then executes:

| Step | CLK0 | Expected baseband | Measurement |
| --- | --- | --- | --- |
| OFF_BEFORE | Disabled, 7.075 MHz configured | Baseline | 1 s discard + 1 s capture |
| ON_1KHZ | 7.075 MHz enabled | 1 kHz | 1 s discard + 1 s capture |
| OFF_AFTER | Disabled | Baseline again | 1 s discard + 1 s capture |
| ON_2KHZ | 7.076 MHz enabled | 2 kHz | 1 s discard + 1 s capture |
| HOLD_1KHZ | 7.075 MHz enabled | 1 kHz | 1 s discard + 1 s capture, then hold |

The final held source is followed by continuous one-second I2S diagnostic
windows. There is no G47-LOW/TX selection in the sequence. API/read failures
request all Si5351 outputs off, stop I2S, and request G48 LOW/G47 HIGH.
This is a bench-only mode, not a complete LPF/TX protection system.

## Hardware results

One explicit USB reset was captured for 25 seconds after successful flash
verification. Five 48,000-frame measurement windows and thirteen 48,000-frame
continuous diagnostic windows were reported: **864,000 stereo frames** total.
Startup drains and the diagnostic's nested 100 ms prefix are not added to
that total. No read errors, firmware errors, restart loops or nonzero I2S
padding appeared. All five finite measurements reported zero exact ADC rail
hits; their peak codes remained far below signed-24 full scale.

| Step | 1 kHz L/R dBFS | 2 kHz L/R dBFS | Phase R-L at intended tone |
| --- | --- | --- | --- |
| OFF_BEFORE | -122.11 / -126.71 | -116.04 / -108.82 | Not meaningful |
| ON_1KHZ | -16.30 / -16.34 | -78.45 / -71.16 | +91.77 degrees |
| OFF_AFTER | -123.23 / -110.67 | -111.43 / -121.55 | Not meaningful |
| ON_2KHZ | -109.68 / -98.69 | -16.50 / -16.39 | +90.75 degrees |
| HOLD_1KHZ | -16.30 / -16.34 | -78.72 / -71.09 | +91.77 degrees |

dBFS here is the coherent single-bin **peak** amplitude relative to 2^23.
These are narrow-bin measurements, not broadband SNR or RX/TX isolation.
OFF-phase values are intentionally not interpreted. The two 1 kHz ON captures
agree closely in amplitude and relative phase.

The 1 kHz ON-channel AC RMS is about 1.00 million codes, greater than the
measured fundamental peak divided by sqrt(2), about 0.91 million codes. Do not
call the waveform a clean sine or derive THD from these two bins. Other
spectral energy, waveform distortion, slight frequency mismatch, and capture
continuity are not fully characterized. No exact ADC rail hits also does not
exclude clipping or nonlinearity earlier in the analog path.

The response may include internal capacitive, supply or clock feedthrough;
TP5 probing or a controlled external stimulus is needed to establish the
coupling route. Absolute sensitivity, front-end filtering, I/Q calibration
and lossless DMA continuity are not certified by this result.

Raw evidence: [serial capture](2026-09-18-rx-loopback-1.log).

## Build and software verification

- ESP-IDF v5.5.1; ESP32-P4 rev 1.3 at `/dev/cu.usbmodem101`.
- Installed loopback ELF SHA256 prefix: `4331f373f`; flash hash verified.
- Fresh default mock-host build also succeeded, without flashing it.
- Native tone-measurement tests passed with `-Wall -Wextra -Werror` and
  AddressSanitizer/UndefinedBehaviorSanitizer: known 1/2 kHz amplitude and
  quadrature, DC rejection, silence, rail hits, padding, partial reads,
  invalid arguments and read failures.
- `git diff --check` passed.

See [profile/build instructions](../tab5/README.md#rx-leakage-loopback-bs170s-absent)
and `tests/rf_loopback_host_test.c` for the host-test command.

## Handoff for scope measurements

Left running without another reset after the recorded run:

- G48 HIGH, G47 HIGH; all three BS170s must remain absent.
- CLK0 7.075 MHz, CLK1 28.296 MHz; expected audio beat 1 kHz.
- MCLK 12.288 MHz, BCLK 3.072 MHz, LRCK 48 kHz remain enabled.

Probe TP5 for the leaked RF. TP10/TP11 are I/Q amplifier outputs, where the
1 kHz response may be easier to inspect. Start with high-impedance x10 probes,
short board-ground connections, and AC coupling to examine AC on the biased
I/Q outputs. Compare both I/Q outputs at about 200 us/div. TP8 is the stronger
buffer/gate signal, not evidence of reception. No additional PA gain is needed
for the response already observed in the ADC data.
