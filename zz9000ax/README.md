# ZZ9000AX ADAU1701 production profile

`zz9000ax-mix1-lowpass-eq.dspproj` is the source-authoritative SigmaStudio
4.7 project for the ADAU1701 program embedded in the firmware.

## Production capture contract

The normal graph routes:

- physical left RCA / ADC0 to `Output5` / `DIG0`;
- physical right RCA / ADC1 to `Output6` / `DIG1`;
- ADC left/right through the existing analogue monitor mixer; and
- FPGA playback through the low-pass, prefactor, equalizer, volume, and DAC
  path.

Firmware configures serial-output register `0x081e` to `0x0c22`: 48 kHz,
16-bit TDM8 master mode with a 256*Fs (12.288 MHz) BCLK. `DIG0` and `DIG1`
therefore occupy slots 0 and 1 on the existing `SDATA_OUT0` wire. The FPGA
extracts those two slots and normalizes them to conventional stereo I2S for
the existing Xilinx receiver, formatter, DMA, firmware, and AHI path.

This fixed transport was selected by the a203 characterization image on real
ZZ9000AX R1 hardware. Its deterministic TDM8 captures preserved both distinct
channels exactly; conventional I2S produced an exact-zero right channel.
Runtime mode switching, pattern generation, raw-frame snapshots, and the
characterization control portal are not part of the production image.

## Generated image identity

| Input | SHA-256 |
|---|---|
| `zz9000ax-mix1-lowpass-eq.dspproj` | `7aab83f544e8721554dd290ab5562a216ac28d7a9d8af353ca115a8a1cec7fac` |
| `Program_Data_Normal_ADC_IC_1` (5120 bytes) | `9b58c07ab19d13665cb4610bf498059dc993ca3499b96d62349dd8e84c31adb0` |
| `Param_Data_Normal_ADC_IC_1` (4096 bytes) | `0fb714aa995d9223f007328b9b98074dbcd92b48c0e4fcebbd2738aae15f6ef0` |

The graph source and its program/parameter arrays are independent of the
serial framing register, which firmware writes and verifies after every cold
boot. Firmware also waits for the ADAU1701 PLL and boot-ROM copy, holds the DSP
core at `0x0018` while loading, reads back every program and parameter word,
and releases the core at `0x001c` only after the complete normal image is
verified.

## Regenerating and verifying

1. Open `zz9000ax-mix1-lowpass-eq.dspproj` in SigmaStudio 4.7.
2. Link, compile, save, and export the system files.
3. Copy the exported `Program_Data_IC_1` and `Param_Data_IC_1` bodies into
   `Program_Data_Normal_ADC_IC_1` and `Param_Data_Normal_ADC_IC_1` in
   `adau.h`.
4. Update the three hashes above and in `test/audio/audio_profile_test.c`.
5. Copy the generated parameter map to `adau_PARAM.h` if the graph changes.
6. Run `make -C test/audio test`.

`adau.h` deliberately supplies its own integration wrapper instead of
including SigmaStudio's generated `SigmaStudioFW.h` and `*_REG.h` files.
