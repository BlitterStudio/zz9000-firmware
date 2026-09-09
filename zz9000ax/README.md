# ZZ9000AX ADAU1701 production profile

`dsp/limiter-postvolume.dspproj` (SigmaStudio 4.7) is the
source-authoritative project for the ADAU1701 program embedded in the
firmware: the original mixer/LPF/prefactor/EQ/volume chain plus the
post-Volume stereo-linked peak limiter that ships engaged at 0.47 FS.
The pre-limiter `zz9000ax/zz9000ax-mix1-lowpass-eq.dspproj` remains in
the tree for history and comparison.

Rate conversion in and out of this fixed 48-kHz domain is handled by the
ARM-side qualified converter kernel; see `docs/audio-conversion.md` for
the measured passband/stop-band/group-delay/cycle-cost table and the
capture register contract.

## Production capture contract

The normal graph routes:

- physical left RCA / ADC0 to `Output5` / `DIG0`;
- physical right RCA / ADC1 to `Output6` / `DIG1`;
- physical ADC/Paula stereo and FPGA playback into `St Mixer1`; and
- the combined mixer output through low-pass, prefactor, equalizer,
  volume/pan, the stereo-linked limiter, and `DAC0` / `DAC1`.

The ADC capture taps remain before the mixer and master scene chain, so
recording observes the physical inputs without internally feeding back
FPGA playback. Every line-output source joins before the scene blocks;
volume zero therefore mutes both Paula and AX.

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
| `dsp/limiter-postvolume.dspproj` | `5d002c6876c37a357cfd8b5f45791167be5d315162c2867b319bdaf683d697be` |
| `Program_Data_Limiter_IC_1` (5120 bytes) | `b703b62adac2ee2d859384fcc92e8369d4273ed6d20f698cc5bd46d1630e9fd8` |
| `Param_Data_Limiter_IC_1` (4096 bytes) | `7565c1b872f2abac46fd4dcd14f388164441ec1a0e0857d42f766f7099b21b8a` |

The graph source and its program/parameter arrays are independent of the
serial framing register, which firmware writes and verifies after every cold
boot. Firmware also waits for the ADAU1701 PLL and boot-ROM copy, holds the DSP
core at `0x0018` while loading, reads back every program and parameter word,
and releases the core at `0x001c` only after the complete normal image is
verified.

## Regenerating and verifying

1. Open `dsp/limiter-postvolume.dspproj` in SigmaStudio 4.7.
2. Link, compile, save, and export the system files.
3. Copy the exported `Program_Data_IC_1` and `Param_Data_IC_1` bodies
   into `adau_limiter_image.h` (the loader symbols in `adau.h` alias
   onto `Program_Data_Limiter_IC_1` / `Param_Data_Limiter_IC_1`).
4. Update the three hashes above and in `test/audio/audio_profile_test.c`.
5. Copy the generated parameter map to `adau_limiter_PARAM.h` if the
   graph changes.
6. Run `make -C test/audio test`.

`adau.h` deliberately supplies its own integration wrapper instead of
including SigmaStudio's generated `SigmaStudioFW.h` and `*_REG.h` files.
