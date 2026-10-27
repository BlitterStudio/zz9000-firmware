# ZZ9000 configuration and compatibility

`ZZ9000.CFG` controls the firmware settings that are read from the FAT32
microSD card. Put it beside `BOOT.bin` in the card root, then power-cycle the
Amiga after editing it. Release ZIPs include the commented
[`ZZ9000.CFG`](../ZZ9000.CFG) sample.

The parser accepts one `key = value` assignment per line. Keys and keyword
values are case-insensitive. `#` and `;` begin comments. Unknown keys and
invalid values are skipped, except that a malformed `fast_ram` assignment
keeps Fast RAM disabled. The file is limited to 8 KiB; data after that limit
is ignored.

ZZTop can write this file through its Settings, Scandoubler, and Audio save
commands. Settings and Scandoubler saves replace the settings they manage and
keep the previous version as `ZZ9000.BAK`. Audio Save preserves every
non-audio line, including hand-written comments and unknown keys, and replaces
only the audio keys.

## Native video capture

Use `videocap_profile` in new files. The three legacy keys in the second row
remain available for existing configurations.

| Key | Accepted values | Effect |
|---|---|---|
| `videocap_profile` | `full_60`, `full_exact`, `filtered_60`, `filtered_pal`, `filtered_pal_exact`, `filtered_ntsc_exact`, `centered_1080p_60`, `centered_1080p_50`, `centered_1080p_match` | Selects a complete native-video profile. The sample default is `filtered_60`. |
| `videocap_mode` | `800x600`, `pal`, `720x576` | Legacy output-mode selector. |
| `videocap_shres` | `filter`, `full` | Legacy filtered or full-detail selector. |
| `nonstandard_vsync` | `off`, `on`, `pal`, `ntsc` | Legacy refresh selector. `on` is the same as `pal`. |
| `videocap_sample` | `average`, `even`, `odd` | Chooses 28 MHz sampling. The sample default is `average`. |
| `videocap_crop_h` | `0`–`4095` | Horizontal capture crop in samples. Omit for automatic crop. |
| `videocap_crop_v` | `0`–`4095` | Vertical capture crop in lines. Omit for automatic crop. |
| `videocap_phase` | `-255`–`255` | Legacy E7M capture-clock phase. Omit unless calibrated. |
| `videocap_c28_phase` | `-896`–`895` | AGA video-slot capture-clock phase. It is independent of `videocap_phase`. |
| `videocap_width` | `256`–`1280`, divisible by 16 | Manual capture width in stored words. Omit for automatic width. |
| `videocap_height` | `100`–`1024` | Manual capture height in source lines. Omit for automatic height. |

A present `videocap_width` or `videocap_height` value of `0` is invalid. The
runtime geometry interface uses zero to mean automatic, but the persistent
configuration file requires the valid ranges in the table.

A minimal configuration is:

```ini
videocap_profile = filtered_60
scanline_mode = 2
scanline_parity = 0
```

`filtered_60` uses 800x600 for PAL input and 720x480 for NTSC input.
`full_60` and `full_exact` preserve full SuperHires detail in a 1280x1024
output. `full_exact` uses fixed PAL/NTSC timing approximations of about
49.93/59.95 Hz; it does not phase-lock to the source.

Fullscan uses integer vertical scaling on the classic power-of-two paths. PAL
uses x4 for 256 progressive rows and x2 for 512 interlaced rows to fill the
1024-line raster. A 15 kHz NTSC source uses the same factors for 200 or 400
rows in an 800-line viewport, with 112-line black bars above and below.
Short-line NTSC sources such as Euro72 and DblNTSC retain the captured row
class rather than using that letterbox, so the viewport does not clip them.

On full-rate doubled sources such as DblPAL and Euro72, the 640-pixel line is
repeated horizontally and the existing row class sets vertical scaling. A
640x512 class-2 source fills the 1280x1024 capture raster. Filtered capture
and short-line, non-doubled Super72 retain their existing horizontal
sampling. `SCALEX` changes displayed pixels only; 32-bit VDMA rows retain
their content pitch.

`centered_1080p_60` and `centered_1080p_50` place the full-rate native picture
inside a 1920x1080 signal with 320-pixel side borders and 28-line top and
bottom borders. Their nominal rates are approximately 60.03 Hz and 50.02 Hz.
They use the closest legal 100 MHz integer-PLL setting to 148.5 MHz,
52/5/7 = 148.5714286 MHz, with standard blanking unchanged. They are
free-running rather than input-genlocked. Centered 60 Hz requires firmware
capability bit 3; centered 50 Hz requires capability bits 3 and 4. Those
capabilities are advertised only by matching viewport/full-rate bitstreams.

Use matching firmware, bitstream, `ZZ9000.card`, and ZZTop for centered
profiles. Current ZZTop hides unsupported centered choices and substitutes
`full_60` when it loads or saves an unsupported stored profile. Older firmware
skips an unknown profile token, so hand-editing it into an older stack does not
guarantee that fallback.

On a filtered profile, a manual capture window is centered in its active
800x600, 720x576, or 720x480 canvas, rather than in the 1280x1024 fullscan
canvas.

`centered_1080p_match` is experimental. With the matching bitstream, firmware,
`ZZ9000.card`, and ZZTop, it adjusts vertical blanking to the captured source
cadence. Interlaced input uses bounded field-pair correction; progressive
input uses direct anchor tracking. During interlaced acquisition or
reacquisition, pixels remain hidden until capture and read phases are safe.
On a bitstream without source-sync support, firmware degrades this selection
to `centered_1080p_60`.

Native-pixel testing on an A4000 with a default-Z3 ZZ9000 reported correct
display for all tested interlace modes and no visual anomalies. This result
does not establish monitor compatibility or tear-free operation on other
systems.

RTG/native changes keep the HDMI signal active when the complete output timing
is unchanged. A centered native 60 Hz mode and a 1920x1080 RTG mode using the
same timing preset do not require transmitter or pixel-clock retraining.
Different refresh rates or sync timings still require an output-mode change.

The runtime geometry request is distinct from the persistent file keys.
Firmware capability bit 8 accepts `CARD_FEATURE_VIDEOCAP_GEOMETRY`, and bit 10
adds its acknowledgement contract. `REG_ZZ_CONFIG_KEY` queries 28–34 report
the requested and applied dimensions, serials, and `APPLIED_VALID`, `PENDING`,
and `REJECTED` status bits. An accepted request remains pending until stable
native-vblank VDMA programming succeeds. RTG vblanks and failed VDMA setup do
not acknowledge it.

`VCAP_LIVE_EFFECTIVE_CROP` at host offset `0x140c` reports the resolved
capture-domain automatic crop. `REG_ZZ_VIDEOCAP_STATS` (`0x4e`) reports the
ten-bit frame line count in both word halves, including the upper half used by
Zorro II reads.

## Scanlines and card options

| Key | Accepted values | Effect |
|---|---|---|
| `scanline_mode` | `0`–`3` | `0` off, `1` black, `2` dimmed, `3` gradient. |
| `scanline_parity` | `0`, `1` | Selects the darkened line parity. |
| `gamma` | `0.20`–`5.00` | Output gamma filter; `1.00` or absent is identity. Above `1.00` brightens, below `1.00` darkens. Ignored by bitstreams without the FPGA gamma LUT; the `ZZGamma` tool also sets it live. |
| `int2` | `on`, `off`, `1`, `0` | Selects INT2 instead of INT6. |
| `offscreen_bitmaps` | `on`, `off`, `1`, `0` | Enables or disables Picasso96 off-screen bitmaps. |
| `video_overlay` | `on`, `off`, `1`, `0` | Enables or disables the Picasso96 video window. |

Scanline mode and parity can change live. Save them to preserve the setting
across a reset. Existing `ENV:` variables and `ZZ9000.card` tooltypes take
precedence over equivalent file settings; remove old overrides when migrating.

In ZZTop, open Scandoubler from the button above Audio or from
`Project > Scandoubler`. Its Capture control opens sampling, framing, and
native-picture calibration. `Project > Settings` edits INT2, MAC, HDF,
off-screen bitmap, and overlay settings; `Project > Audio` edits audio
settings. General Settings save leaves absent native keys inactive, while
Scandoubler save writes the displayed native settings.

See the driver's [Output/Refresh profile table](https://github.com/BlitterStudio/zz9000-drivers#scandoubler-output-and-refresh)
for supported selector combinations.

## Fast RAM, network, and SD boot

| Key | Accepted values | Effect |
|---|---|---|
| `fast_ram` | `on`, `off`, `1`, `0` | Requests Zorro III Fast RAM on a supporting image. |
| `mac` | Six hexadecimal octets separated by `:` or `-` | Overrides the Ethernet MAC address. |
| `hdf` | `off`, or a root-level filename up to 63 printable ASCII characters | Selects the SD-boot HDF. `off` disables SD boot. The default HDF name is `zz9000.hdf`. |

`hdf` names cannot start with `.` or include a path separator, drive prefix,
`#`, `;`, whitespace, or non-printable ASCII.

Fast RAM is disabled unless a valid `fast_ram = on` decision is available.
The fast-RAM loader runs before the advertised Zorro slot is exposed, and it
runs again on a later warm reset. A missing, malformed, unreadable, truncated,
or too-slow configuration leaves Fast RAM disabled for that pass. Firmware
also requires the bitstream's `MNTZORRO_REG7` Fast-RAM-base capability; a
new firmware therefore withholds Fast RAM from an older bitstream even when
the configuration requests it. On an unstable 68040/68060 system, remove
`fast_ram` or set it to `off`, then reboot.

For firmware 2.9.0, install the matching 2.9.0 bitstream and RTG drivers
before enabling `fast_ram`. The Fast-RAM location and its RTL guard changed
together in this release.

## ZZ9000AX audio

The audio control plane stores eight scenes, an active scene, a baseline, and
per-card clean ceilings. Values are decimal.

| Key or family | Accepted values | Effect |
|---|---|---|
| `audio_active` | `0`–`7` | Scene selected at boot and after each Amiga warm reset. |
| `audio_baseline` | Unsigned 16-bit decimal | Paula/AX balance packed as `paula*256+ax`, with each leg `0`–`255`; 127 is 0 dB. |
| `audio_ceiling_paula` | `1`–`4095` | Measured clean Paula-only ceiling. |
| `audio_ceiling_ax` | `1`–`4095` | Measured clean AX-only ceiling. |
| `audio_scene<N>_lpf` | `N` = `0`–`7`; `1`–`23900` | Low-pass cutoff in Hz. `23900` is the DSP default. |
| `audio_scene<N>_eq01`, `_eq23`, `_eq45`, `_eq67`, `_eq89` | `N` = `0`–`7`; packed pair with each field `0`–`100` | EQ bands, packed as `hi*128+lo`; 50 is 0 dB. |
| `audio_scene<N>_out` | `N` = `0`–`7`; packed pair with each field `0`–`100` | Prefactor and volume, packed as `prefactor*128+volume`. |
| `audio_scene<N>_pan` | `N` = `0`–`7`; `0`–`100` | Stereo pan: 0 left, 50 center, 100 right. |
| `audio_scene<N>_nm1` through `_nm8` | `N` = `0`–`7`; packed printable-ASCII pair | Scene name chunks, packed as `c1*256+c2`. `0` ends the name. |

The firmware weights Paula by `audio_ceiling_ax/audio_ceiling_paula`, caps
each leg at its configured ceiling, then limits the combined output. The
fallback ceilings are Paula 48 and AX 80, measured on one R1 card. They are
not a clean-limit guarantee for every board. With no saved baseline, the
fallback mix is Paula 36 and AX 72, for weighted level 132/160. Saved ceiling
and baseline values take precedence, including older 256/256 or 192/255
values. Recalibrate instead of increasing a ceiling to conceal distortion.

LPF, EQ, prefactor, and volume process the shared Paula/AX output. Absent
scene keys retain the firmware defaults. The active saved scene is applied at
cold boot and after each Amiga warm reset before an application can allocate
the audio device.

## AGA video-slot capture

The `zorro3-aga` bitstream captures AGA pixels from the 28 MHz video-slot
signal. Its capture MMCM operates at approximately 908–916 MHz with one
capture clock per source pixel. E7M bitstreams remain the route for A3000,
other machines, and an A4000 fallback. Configuration cannot change the clock
source.

Build the release AGA bitstream on a Vivado 2018.3 host:

```bash
./build_variant_bitstreams.sh zorro3-aga
```

For a separate Windows diagnostic image, run:

```powershell
.\build_bitstream.ps1 -CaptureC28
```

The diagnostic output is
`bootimage_work/capture-c28/zz9000_ps_wrapper.bit`, separate from packaged
release bitstreams. Package it with matching ARM firmware by passing both
`--bitstream` and a separate `--output` path to `build_bootimage.sh`.

Use the matching `ZZCapture` tool from
[zz9000-drivers](https://github.com/BlitterStudio/zz9000-drivers) to inspect
clocks and calibrate a still SuperHires pattern. It checks raw RGB values and
frame stability, sweeps the phase circle, refines clean boundaries, and
retests the midpoint. On success it prints `videocap_c28_phase`; saving is
explicit. Run `Stack 32768` in the Amiga Shell before calibration or applying
a phase.

`videocap_c28_phase` and `videocap_phase` use different phase units. Preserve
both when saving unrelated settings. Calibrate on affected PAL and NTSC
machines after cold and warm boots, then check lores, hires, SuperHires,
progressive, interlaced, filtered, and full-detail output. Calibration checks
raw input pixels only. It does not qualify vertical geometry or the complete
HDMI path. Keep a working `BOOT.bin` for recovery.

## Custom Picasso96 modelines

Matched `ZZ9000.card` 2.11 and firmware advertising `ZZ_FW_CAP_CUSTOM_MODE`
(capability bit 7) accept custom progressive RTG timings, including 960x720.
The driver sends active dimensions, sync positions, totals, common sync
polarity, and a legal pixel-clock PLL tuple through the custom-mode register
window.

| Property | Limit |
|---|---|
| Active size | Width 320–2560 pixels, aligned to 8; height at least 200 |
| Porch and sync | Positive front porch, sync width, and back porch on both axes |
| Total dimensions | At most 4095 |
| Pixel clock | 25–165 MHz; achieved PLL clock must be within 0.5% of the request |
| Scan and sync polarity | Progressive only; HSync and VSync use the same polarity |

Firmware stages a full modeline before applying it. Invalid or incomplete
requests leave the live mode unchanged. A PLL lock failure restores the
previous output. Video interrupts are deferred during the transaction; audio
and other interrupts remain enabled. Packaged presets retain their output
timings and historical P96 polarity flags.

Use P96's temporary test display before saving a custom mode. Firmware
acceptance does not establish monitor support. Back up `BOOT.bin`,
`ZZ9000.card`, and P96 settings first. See the
[driver instructions](https://github.com/BlitterStudio/zz9000-drivers#custom-picasso96-modelines).

## USB host stack

Firmware 2.8.0 RC3 and `zzusbhw.device` 2.2 form one USB proxy release. The
matched pair negotiates protocol v2 before advertising persistent interrupt,
simple ISO, or realtime ISO support. A driver without the required capability
set leaves those public capabilities disabled.

The transport supports control, bulk, persistent interrupt, and high-speed or
split full-speed ISO IN/OUT. Full-speed ISO needs a high-speed hub transaction
translator. Direct low-speed devices remain unadvertised. MIDIStreaming uses
its descriptor-defined bulk endpoints; enabling USB-audio ISO does not reroute
MIDI through ISO.

Host-side models cover mailbox ownership, EHCI retirement, error mapping,
periodic cadence, ISO descriptors and batches, and stop/reset races. They do
not qualify physical devices. Consult the drivers repository's
[`docs/usb-qualification-matrix.md`](https://github.com/BlitterStudio/zz9000-drivers/blob/master/docs/usb-qualification-matrix.md)
for hardware evidence and unavailable topologies. Run the matched `ZZDiag`
binary on AmigaOS to collect driver and firmware capabilities, epochs,
counters, queue state, schedule bits, and recent events.

## Amiga MMU and cache settings

On 68040/68060 systems, configure every pure ZZ9000 RAM window in the
Amiga-side MMU tool. For optional Zorro III Fast RAM, `Writethrough` has shown
the best recorded performance because reads can use cache while writes reach
the board immediately. In MuLibs/MMULib terms:

```text
For 28014 5 SetCacheMode {base} {size} Valid Writethrough
```

Avoid `CopyBack` for ZZ9000 RAM unless the driver and workload are known to be
cache-coherent. Keep MMIO, register, boot, USB, Ethernet, and other FPGA/ARM
shared windows cache inhibited or data no-cache. If `Writethrough` is unstable,
use `Data NoCache` or `CacheInhibit` for the configured Zorro RAM range. Leave
instruction cache enabled. 68030 systems do not need this workaround.
