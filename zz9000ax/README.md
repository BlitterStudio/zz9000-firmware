# ZZ9000AX ADAU1701 production profile

The firmware loads the ADAU1701 program from
`ZZ9000_proto.sdk/ZZ9000OS/src/adau_limiter_image.h`. Its authoritative
SigmaStudio 4.7 project is `dsp/limiter-postvolume.dspproj`. The profile runs
at 48 kHz and enables a post-volume, stereo-linked peak limiter at 0.47 FS on
boot.

Firmware uses the fixed TDM8 capture transport. It writes and verifies
`0x0c22` at codec register `0x081e`; ADC left and right use slots 0 and 1.
The FPGA converts those slots to the stereo I2S stream used by the existing
receiver, formatter, DMA, firmware, and AHI path. Rate conversion to and from
this 48 kHz domain is described in
[`docs/audio-conversion.md`](../docs/audio-conversion.md).

## Signal path

The production graph routes physical left RCA / ADC0 to `Output5` / `DIG0`
and physical right RCA / ADC1 to `Output6` / `DIG1`. It combines ADC/Paula
stereo and FPGA playback in `St Mixer1`, then applies low-pass filtering,
prefactor, equalizer, volume/pan, the limiter, and `DAC0` / `DAC1`.

The capture taps precede the mixer and master scene chain. Recording therefore
receives the physical inputs without FPGA playback feeding back into capture.
All line-output sources join before the scene controls, so volume zero mutes
both Paula and AX.

## Regenerate the image

1. Open `dsp/limiter-postvolume.dspproj` in SigmaStudio 4.7.
2. Link, compile, save, and export the system files.
3. Copy the exported `Program_Data_IC_1` and `Param_Data_IC_1` bodies into
   `ZZ9000_proto.sdk/ZZ9000OS/src/adau_limiter_image.h`. `adau.h` aliases
   them as `Program_Data_Limiter_IC_1` and `Param_Data_Limiter_IC_1`.
4. Copy the generated parameter map to
   `ZZ9000_proto.sdk/ZZ9000OS/src/adau_limiter_PARAM.h` if the graph changed.
5. Update the image identity values in `test/audio/audio_profile_test.c`.
6. Run the profile test:

   ```sh
   make -C test/audio test
   ```

Firmware holds the DSP core at `0x0018` while it loads and reads back the
program and parameter data. It releases the core at `0x001c` only after the
load succeeds.

## Recorded hardware result

The a203 characterization image on a ZZ9000AX R1 card captured two distinct
channels through TDM8 slots 0 and 1. The conventional I2S setup produced an
exact-zero right channel. The production image excludes the characterization
mode, pattern generator, raw-frame snapshots, and its control portal.

The pre-limiter project
`zz9000ax/zz9000ax-mix1-lowpass-eq.dspproj` remains for comparison.
