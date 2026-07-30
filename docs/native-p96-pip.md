# Native FPGA P96 PIP design

Status: the native 1:1 foundation is merged on `master`. Local
`feat/zzplay-media` commit `c6341f8` extends it with exact nearest-neighbour
FPGA scaling, sparse source-row fetching, and screen-edge clipping. A
640x480@30 hardware run presented 4,374/4,405 frames after resize versus
4,379/4,405 at exact size; the user reported identical correct playback.

The earlier r7-r9 chronology below records how the 1:1 foundation was
qualified. R9 replaced unsafe row/readiness clock-domain crossings with atomic
XPM handshakes. R9b passed 16/32-bit display depth, scanline-control stability,
and native-video/videocap takeover/return, completing the default-`zorro3`
physical acceptance sequence. Non-default variants were rebuilt from the same
then-current RTL on 2026-07-29. All six passed fresh-project synthesis,
implementation, DRC, bus-skew, and timing gates, but remain
hardware-unqualified until tested on their respective board variants.

## Contract

Picasso96 remains the only public presentation API. Existing applications use
`p96PIP_*` directly or through cgxvideo. The current driver continues sending
`OP_VIDEO_OVERLAY` with the P96-managed source bitmap, real pitch, YUV422
variant, destination rectangle, activation, and color key. No application or
driver API fork is introduced.

Firmware and bitstream are a matched ABI. Packed sources with a positive
destination size and a non-empty screen intersection select the native plane
in the hardware-capable build, including up/down scaling and clipping.
Release packaging must pair that firmware with bitstreams built from the
matching formatter RTL. `ZZ9000OS-legacy-bitstream.elf`, whose hardware
backend is compile-time inert, remains the compatibility fallback for
installations that retain an older bitstream. Unsupported source bounds and
legacy-bitstream operation retain the ARM compositor.

SDK video sessions use the same P96 window/geometry. Their decoder-owned
planar420 source is an internal firmware source mode, not a different player
API.

## Stage 1: packed P96 YUV422 plane

Add a second MM2S VDMA sharing HP0 through a two-input AXI interconnect. It
feeds ping-pong YUV422 line buffers in `video_formatter`.

- 1:1 or scaled/clipped PIP: VDMA reads the P96 packed-YUV source while the
  formatter maps destination pixels and rows to exact source coordinates.
- SDK planar420 at 1:1: core 1 writes the non-scanned ping-pong packed-YUV422
  staging surface; vblank publishes it only after the mandatory full L1+L2
  flush, then VDMA reads it.
- sparse vertical downscaling drains skipped sequential VDMA rows and writes
  only requested rows into explicitly owned ping-pong banks.
- FPGA performs CCIR601 YUV-to-RGB, RGB color-key comparison, scanout-time
  composition, and normal sprite/scanline post-processing.
- ARM no longer copies or flushes full-screen RGB shadows.

The staging path is deliberately packed YUV, not RGB. SDK playback already
avoids the dominant screen-sized RGB copy/conversion. Existing direct P96 PIP
and cgxvideo players benefit without modification.

## Stage 2: planar420 SDK source

Add planar Y/Cb/Cr fetch support and explicit display/decode buffer fences.
The active pl_mpeg display frame cannot be reused until PL has crossed its
last visible overlay line. This removes the SDK's final display-surface copy
while preserving P96 ownership of window geometry and lifetime.

## Formatter controls

Reserve video-control operations 22-27:

| Op | Data |
|---|---|
| 22 `OVERLAY_CTRL` | enable, key enable, source mode, YUV422 variant |
| 23 `OVERLAY_POS` | signed destination Y:X |
| 24 `OVERLAY_SIZE` | destination H:W |
| 25 `OVERLAY_KEY` | converted RGB888 key |
| 26 `OVERLAY_SOURCE_SIZE` | source H:W |
| 27 `OVERLAY_FRAME` | frame generation / fence token |

VDMA address, pitch, HSIZE, and VSIZE remain AXI-Lite registers programmed by
firmware. Geometry/control changes latch at vblank.

## Gate status (2026-07-29)

1. **PASS:** `video_overlay_pixel` layout/luma/clamp/key simulation for all
   five packed layouts.
2. **PASS:** formatter xsim sweep, 12/12 mode/scale/width configurations;
   overlay-enabled cases cover 32/16/15-bit base modes, odd width, and a
   1920-pixel full-width transfer that cannot fit in horizontal blanking.
3. **PASS for implemented scope:** line-buffer stalls, unexpected-SOF resync,
   bank wrap, disable, source-generation invalidation, independent
   fetch/display selection, and right-edge PIP timing. Resize/move cases
   retain the software fallback.
4. **PASS:** fresh default Vivado implementation, current-source fingerprint,
   WNS +1.631 ns / TNS 0, 0 errors and 0 critical warnings in the formatter
   synthesis and implementation logs.
5. **PASS (complete build matrix):** all six non-default bitstreams were
   regenerated from fresh Vivado projects using the current RTL. Each passes
   synthesis, implementation, DRC, bus-skew, and timing gates. This enables
   the native plane in the complete current bitstream set; the compatibility
   ELF remains available only for genuinely older bitstreams.
6. **FAIL HARDWARE (visual):** the resumed 640x480@30 `zzplay` run opened the
   expected 1:1 window and processed frames, but the PIP contents stayed solid
   cyan with slight flicker. No outside-window corruption was seen and the
   hardware pointer remained topmost. Throughput results are diagnostic only
   until pixels are correct.
7. **PASS/DECISIVE DIAGNOSTIC:** keep the installed artifacts unchanged and
   resize the window. Exact-size, fully visible geometry selects native PL;
   resized or clipped geometry selects the ARM compositor. Capture `[overlay]
   SET ok build ...` UART lines in both states and fingerprint `BOOT.bin`,
   `zzplay`, `zz9k.library`, and `ZZ9000.card`.
8. **PARTIAL FIX, r6 HARDWARE FAIL:** a running AXI VDMA direct-register channel only
   commits a newly written `START_ADDR` after `VSIZE` is written. The native
   flip wrote `START_ADDR` and advanced the formatter generation without that
   commit, so the VDMA could continue scanning the initial cleared P96 source
   instead of the decoder's packed-YUY2 staging surface. The flip now writes
   `START_ADDR`, then `VSIZE`, then advances the generation. r6 changed the
   display from cyan to real decoded pixels, confirming this defect, but the
   image was vertically double/superimposed and flickered until resize.
9. **PARTIAL FIX, r7 HARDWARE FAIL:** the formatter re-armed line zero at the
   start of vblank, before the ARM vblank handler committed the next VDMA
   address after its mandatory cache flush. MM2S could therefore start the
   old frame and lose vertical phase when the later generation change forced
   a resync. Native scanout now stays stalled at the completed frame boundary;
   firmware resets/reprograms and re-arms every frame after the flush, and the
   pixel domain requests line zero only after the line-buffer AXI domain
   acknowledges that generation. The overlay VDMA is connected directly to
   the line fetcher so no decoupling FIFO can retain old-frame beats across
   the reset. Hardware r7 displayed a mostly correct, properly phased image,
   but it still flickered noticeably.
10. **R8 HARDWARE FAIL:** independent displayed-line and fetch-line
    controls now request line N+1 as soon as displayed line N is ready while
    continuing to read N from the other BRAM bank. Display selection follows
    the source row at the next raster boundary. The matched bundle is
    `bench-zzplay-native-p96-r8-line-prefetch`. Exact-size playback flickered
    slightly worse than r7 and showed black horizontal rows at random
    positions. A one-pixel resize remained stable through the ARM fallback.
11. **PASS HARDWARE, DEFAULT `zorro3`:** 318-pixel odd-macroblock playback,
    cgxvideo/RiVA, mode/depth changes, close/reopen, DPMS, all live scanline
    control values, and native-video/videocap takeover and return. Color-key
    occlusion remains opportunistic. **PASS BUILD/REPORT, NON-DEFAULT:** all
    six current variants contain the native plane and meet their implementation
    gates, but still require variant-specific physical acceptance.

The geometry diagnostic in item 7 is now **PASS/DECISIVE**: correct video
appeared only after resizing selected the ARM compositor, and close was clean.
Decoder-owned planar output, frame-ready publication, P96 geometry switching,
and software composition are exonerated for the cyan symptom. The resized
path consumes planar420 directly, so it does **not** validate the native-only
planar420-to-packed-YUY2 conversion, staging-buffer handoff, MM2S, line buffer,
or formatter conversion.

### r3 hardware failure

The first r3 hardware run produced corrupt output and flickering below/outside
the PIP. An invalid XPM declaration was found: bank 1 began at address 2048
while the declared memory ended at 2559. That declaration was corrected and
test-guarded, but the implemented bitstream hash did not change because Vivado
had already rounded the physical BRAM allocation. It was real source/simulation
debt, but not a sufficient hardware root cause.

The corrected r5 gate addresses the defects that do change the implementation
and match the observed symptoms:

- primary scanout has AXI priority 15 over overlay priority 0 on shared HP0;
- overlay DDR reads are 64-bit rather than 32-bit;
- the next overlay row is requested at the PIP's final pixel, not the screen's
  final pixel, giving a left-half window roughly half a scanline more prefetch;
- SDK staging uses the P96 pitch programmed into VDMA and allocation covers
  the larger of screen and staging spans;
- hardware/software path transitions invalidate incompatible packed-YUV/RGB
  shadows, and session close restores the live P96 source at flushed vblank;
- formatter register writes are locally IRQ-atomic.

r3 and the discarded hash-identical intermediate r4 must not be reused.

### 2026-07-26 cyan-frame result

`zz9k-services --check-release` passed and `zz9k-info` reported video service
`0x0b00` version 1.0, video-decode capability, core 1 online, zero failed
requests, and no shared allocations left in use. `zzplay` reported
`direct planar overlay` for both runs:

- uncapped: 3160 frames, 70.285 fps playback, 93.588 fps decode-call;
- paced: 995 frames, 26.544 fps playback, 99.282 fps decode-call.

The displayed frames were invalid, so these figures do not qualify playback
performance. The lack of desktop/lower-screen corruption suggests the primary
scanout starvation symptom from r3 is gone. It does not distinguish a stale or
mismatched binary from a native VDMA/formatter defect; artifact fingerprints
and the resize-to-ARM-fallback A/B come first.

The subsequent same-session A/B did distinguish the presentation branch:
resized ARM output was correct. Source diagnosis may therefore proceed in the
native-only staging/VDMA/formatter path even though artifact fingerprints
remain required for the final release record.

Source inspection then found the missing AXI VDMA dynamic-update commit.
AMD's PG020 programming sequence specifies that live video-parameter and start
address writes enter the internal transfer registers only after a `VSIZE`
write. `overlay_hw_set_buffer()` previously changed the start address and PL
generation only. It now writes the active height to `VSIZE` before advancing
the generation, and `overlay_hw_test` verifies the MMIO order and stopped-state
guard. A clean BOOT with SHA-256
`7cfc2ed653c609b4c1ab40fbd8aa69991de3c4ee9833a49a803add4c10d5e1b3`
was staged in `bench-zzplay-native-p96-r6-vdma-commit`; its failed hardware
result is recorded below.

### 2026-07-26 r6 frame-phase result

The fingerprinted r6 bundle did not pass. Correct video was not immediate:
the native 640x480 window showed two top/bottom images superimposed with
flicker. Resizing continued to select the correct ARM path. The absence of
cyan proves that r6 did make the decoded packed-YUY2 staging surface visible,
so the `VSIZE` change remains part of the fix rather than being reverted.

RTL inspection then exposed the next ordering defect. The formatter requested
line zero at vblank pixel zero, while `control_vblank` interrupts the ARM at
the end of the visible-width position on that line. The ISR also performs the
mandatory full L1/L2 flush before committing the overlay address and
generation. Thus MM2S could already be consuming the old frame when the
generation invalidated its line tags. The matched fix replaces this timing
assumption with an acknowledged generation handoff. Review also removed the
overlay's 32-beat AXI FIFO and made the post-flush handoff reset/reprogram
MM2S, preventing either the FIFO or VDMA internals from retaining the old
staging surface. It requires a rebuilt bitstream and matching firmware.

The matched r7 bundle is
`bench-zzplay-native-p96-r7-frame-handoff`. Its BOOT SHA-256 is
`a253618c5d1ad950809df73c629a1d7678f8cd361556e09f8bfc717bef9b4daa`;
the embedded default bitstream SHA-256 is
`94bc2f59d0af1b022279b1bc040a69131486ed055f92b2de6b15b109cb35d8a6`.
Payload hashes are verified.

### 2026-07-26 r7 line-readiness result

r7 is another partial fix, not a release candidate. The native 640x480 image
was mostly correct and no longer solid cyan or vertically superimposed, which
confirms the post-flush generation handoff and MM2S reset removed the stale
frame/vertical-phase failure. It still flickered noticeably. Resizing the
window by even one pixel immediately produced stable, correct ARM-composited
output.

The remaining native-only failure matches an intermittent line-buffer
readiness miss. The current formatter cannot request line N+1 until the final
visible PIP pixel of line N because the same line number selects both the BRAM
display bank and the MM2S fetch target. For a full-width PIP, that leaves only
horizontal blanking to fetch an entire packed-YUY2 row. When the line is not
ready, the formatter deliberately shows the underlying desktop for that row,
which appears as flicker without corrupting pixels outside the window.

R8 splits those roles: it keeps line N selected for display, requests N+1 as
soon as N is ready, fills the opposite BRAM bank throughout the current raster
row, and derives display selection from the current source row at the
following raster boundary.

### 2026-07-26 r8 line-prefetch candidate

The dedicated line-buffer xsim proves that advancing the fetch target does
not disturb the currently displayed line or its readiness. The full formatter
xsim passes 12/12 configurations with zero pixel mismatches, including
row-distinct luma, odd/right-edge cases, and full-width stress through 1920
pixels. A fresh Vivado project imported byte-identical current sources and
meets timing at +1.631 ns WNS / 0 TNS. All RTG, video, config, scheduler, and
video-codec host tests pass, followed by a clean ARM/newlib build.

The matched hardware bundle is
`bench-zzplay-native-p96-r8-line-prefetch`. Its BOOT SHA-256 is
`22bd0171f49e7e489822b4f3386b289b72c069e13620bf5fcd5b8fdc60049679`;
the embedded default bitstream SHA-256 is
`293320172cfd42eb6752b3961b69a32a3bb900063caf00ae04dbdca932ab4ed2`.
Its first gate is untouched exact-size 640x480 playback for at least ten
seconds, followed by a one-pixel resize only as a control.

Hardware failed that gate. Native playback was about 26 fps, but flicker was
slightly worse than r7 and black horizontal lines appeared intermittently at
random vertical positions. Resizing selected correct, stable ARM output at
about 19–21 fps. R8 is diagnostic only; the result points to native
line-readiness or bank-valid timing that the deterministic xsim source did not
exercise.

### 2026-07-26 r9 atomic-CDC candidate

The implemented r8 CDC report identified the missing hardware behavior:
binary fetch rows, completed-row tags, their valid flags, and the accepted
generation crossed independently between the 100 MHz AXI and DVI pixel
clocks. Several were reported as raw multi-bit synchronizers. A transition
could therefore be sampled as a row number that never existed, or a valid
flag could arrive with the wrong tag. That explains random missing rows while
the deterministic same-clock-value RTL source still passed.

R9 preserves r8's early N+1 prefetch but carries all dynamic metadata through
three bundled-data `xpm_cdc_handshake` instances: fetch request/row,
completed-row descriptor, and generation acknowledgement. A qualified pixel
request invalidates its destination bank before AXI can overwrite it. The
generation startup path also waits for an explicit line-zero request; the
new regression covers firmware programming a generation while the overlay is
disabled and then enabling it.

Validation is complete:

- dedicated line-buffer xsim: PASS, including the disabled-start sequence;
- full current formatter xsim: PASS, 12/12 with zero mismatches through
  1920-pixel stress;
- pinned pre-64-bit reference formatter: PASS, 10/10 applicable cases;
- implemented overlay line-buffer CDC paths: six total, all recognized
  three-stage XPM handshake controls (`CDC-3` informational), with no r8 raw
  row/tag/valid crossing remaining;
- fresh imported RTL fingerprints match the built sources;
- implementation timing: PASS, +1.011 ns WNS / 0 TNS;
- all RTG/video/config/scheduler/video-codec host suites and a clean
  ARM/newlib BOOT build: PASS.

The matched bundle is
`bench-zzplay-native-p96-r9-atomic-cdc`. Its BOOT SHA-256 is
`10f074a51a17cbeaccfa35f965ec6491b1cca6741603d14d91f9beb0bf7d0aca`;
the embedded default bitstream SHA-256 is
`c4a8715798a717db1facbf17357e6a25567fcf134b5f16e5c54c5b1a0c140fc7`.

The first launch did not reach that gate: core 1 was offline, so `zzplay`
opened its P96 window and then reported
`session begin failed: unsupported`. A full power cycle restored core 1
without changing any artifact. The repeated untouched native run then showed
correct video with no flicker or artifacts at about 27–28 fps. This is the
first physical pass of the atomic-CDC line fetcher and clears the r8 random
black-row defect. Retain the one-off offline event as a recurrence watch; it
does not justify changing the scheduler without a failure from a known cold
boot.

The one-pixel resize selected correct ARM output and returning to exact size
immediately restored correct native output. The bundled 318x240
odd-macroblock-width clip measured 23 fps playback and 209 fps decode-call;
its picture was also confirmed correct. The next gates are uncapped 640x480
throughput, move/clip/depth transitions, close/reopen cleanup, and the rest of
the P96 acceptance matrix.

Those interaction/performance gates also pass. Uncapped 640x480 processed
4,405 frames at 69.095 fps playback and 93.113 fps decode-call. Moving,
partially clipping/obscuring, changing depth, restoring, closing, and reopening
were artifact-free. Post-close diagnostics reported core 1 online, zero failed
requests, zero shared buffers/surfaces in use, and the full 4,128,768-byte
shared heap free. The corrected RiVA native-P96PIP binary and stock RiVA
through cgxvideo then produced identical correct results, including movement,
resize, and close. Refreshed diagnostics remained clean: core 1 online,
8,912 completed requests, zero failures, no live buffers/surfaces, and the
full heap free. DPMS is accepted from its existing hardware pass (ON,
Standby, Suspend, Off, wake, and selector-cache follow-up). The subsequent
scanline-control and videocap continuation passed on 2026-07-27.

The first depth-change gate exposed a driver allocation bug before
presentation: on a 1280x1024x16 Workbench, `zzplay` could not open the PIP
window. P96 derives the managed source bitmap's storage from the visible
friend. `ZZ_CreateFeature` always requested half width, which yields the
required packed-YUV pitch for a 32-bit friend but only half the required pitch
for a 16-bit friend; firmware correctly rejected that SET.

The driver-only r9b fix chooses the nominal allocation width from the active
friend's bytes-per-pixel and reserves a complete final YUV pair for odd source
widths. The full RTG host suite passes, including 98 overlay checks, and the
m68k card builds cleanly. The matched test bundle is
`bench-zzplay-native-p96-r9b-16bit-pip`; its `ZZ9000.card` SHA-256 is
`02351bbf229fe32a25816d1d024fd1c5a923528e54d278d29820ba4f591faa6f`.
Hardware validation confirms correct PIP operation in both
1280x1024x16 and 1280x1024x32 modes. The r9b depth-compatibility gate is
therefore PASS.

The user then completed the remaining default-`zorro3` continuation and
reported all visual checks good. Live scanline modes off/classic/soft/gradient
did not disturb exact-size native PIP; the resized ARM fallback and return
remained clean. Native-video/videocap takeover, return to RTG,
exact/resized/exact transitions, pointer ordering, and close cleanup all
passed. High-resolution RTG intentionally suppresses visible scanline shading,
and scaled low-resolution P96 modes make the overlay non-presentable, so this
validates control stability rather than simultaneous visible shading with
PIP. Final `Z:\results.txt` diagnostics reported core 1 online, 4,554
completed requests, zero failures, no live buffers/surfaces, and all
4,128,768 shared-heap bytes free. `ZZDiag` showed the final live scanline state
as classic/even-dark rather than off.

### FPGA scaling extension

Local `feat/zzplay-media` commit `c6341f8` replaces the resized/clipped ARM
shadow path with scanout-time nearest-neighbour scaling for supported
packed-YUV sources. It keeps the public P96/cgxvideo ABI unchanged, preserves
the 1:1 pixel path, computes exact quotient/remainder scale steps outside the
pixel critical path, and assigns line-buffer banks independently of source-row
parity so vertical downscaling can drain skipped rows safely.

The full calibrated formatter sweep passes 12/12 configurations, including
non-integer up/down scaling and negative-edge clipping. A fresh Vivado 2018.3
implementation meets timing: the formatter pixel clock has +0.917 ns setup WNS
and +0.017 ns hold WHS. The default bitstream was rebuilt from the regenerated
project and packaged with the matched firmware.

Default-Z3 hardware validation used the same 640x480@30 MPEG-1 Program Stream
with direct AX audio in separate exact-size and enlarged-window runs:

| Metric | Exact size | Resized |
| --- | ---: | ---: |
| Decoded frames | 4,405 | 4,405 |
| Presented frames | 4,379 | 4,374 |
| Discarded/late frames | 26 | 31 |
| Average playback | 29.916 fps | 29.925 fps |
| Audio frames | 6,476,544 | 6,476,544 |
| Audio underruns | 13 | 10 |

The presented-ratio difference is 0.001135, below the locked 0.002 noise
threshold. The user reported that playback looked exactly the same in both
scenarios, with no resize slowdown or visual regression. Wider clipping,
downscale-ratio, and non-default-variant qualification remains part of the
release matrix; the software compositor remains the compatibility fallback.

### Z3660 cross-check

The sibling [Z3660 repository](https://github.com/shanshe/Z3660) was reviewed
at `main` `1dddf7978fe4ec0203ab2b88f035525083e10ac3` (2026-07-22). Its PL uses a
second VDMA and a single overlay line buffer, but still requests the next row
at the visible-line boundary. Its formatter source explicitly notes a VDMA
switch timing window that produces a flickering line. It has no generation
acknowledgement or ready tags to transplant; the independent fetch/display
split above is the stronger solution.

Its MPEG-1 implementation is also useful context but not a drop-in
optimization. Z3660 buffers up to 1 MiB, copies available FIFO data through a
temporary allocation, converts planar output to RGB32 on ARM, and flushes the
full RGB surface. ZZ9000 already streams directly into the demuxer, exposes
decoder-owned planar frames, writes only packed YUY2 staging, and performs
YUV-to-RGB/composition in PL.

Z3660's vendored `pl_mpeg.h` contains Cortex-A9 NEON motion-compensation and
IDCT experiments. They must not be copied as-is: nested rounded averages do
not match MPEG's exact four-sample expression for all inputs, and several
IDCT expressions add an explicit rounding bias and then use a rounding shift,
applying the bias twice. The ZZ9000 MPEG translation unit is already compiled
at `-O3` with Cortex-A9/NEON flags. A corrected, pixel-exact NEON decoder path
is a worthwhile post-PIP performance follow-up, gated by scalar-vs-NEON
equivalence tests running on an ARM target.
