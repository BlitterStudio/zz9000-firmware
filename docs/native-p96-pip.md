# Native FPGA P96 PIP design

Status: corrected r5 direct/SDK-staged default-bitstream gate built on
`feature/video-session-prototype`; hardware validation and non-default
variants remain.

## Contract

Picasso96 remains the only public presentation API. Existing applications use
`p96PIP_*` directly or through cgxvideo. The current driver continues sending
`OP_VIDEO_OVERLAY` with the P96-managed source bitmap, real pitch, YUV422
variant, destination rectangle, activation, and color key. No application or
driver API fork is introduced.

This firmware and bitstream are a matched ABI: firmware assumes the second
VDMA and overlay controls exist. Fully visible 1:1 packed sources select the
native plane. Resized/clipped legacy sources retain the ARM compositor until
packed resize staging is complete.

SDK video sessions use the same P96 window/geometry. Their decoder-owned
planar420 source is an internal firmware source mode, not a different player
API.

## Stage 1: packed P96 YUV422 plane

Add a second MM2S VDMA sharing HP0 through a two-input AXI interconnect. It
feeds ping-pong YUV422 line buffers in `video_formatter`.

- 1:1, fully visible PIP: VDMA reads the P96 source bitmap directly.
- SDK planar420 at 1:1: core 1 writes the non-scanned ping-pong packed-YUV422
  staging surface; vblank publishes it only after the mandatory full L1+L2
  flush, then VDMA reads it.
- resized or clipped legacy PIP: planned destination-sized packed-YUV422
  staging; the current hardware gate uses the proven ARM fallback.
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

## Gate status (2026-07-11)

1. **PASS:** `video_overlay_pixel` layout/luma/clamp/key simulation for all
   five packed layouts.
2. **PASS:** formatter xsim sweep, 12/12 mode/scale/width configurations;
   overlay-enabled cases cover 32/16/15-bit base modes and odd width.
3. **PASS for implemented scope:** line-buffer stalls, unexpected-SOF resync,
   bank wrap, disable, source-generation invalidation, and right-edge PIP
   timing. Resize/move cases retain the software fallback.
4. **PASS:** fresh default Vivado implementation, current-source fingerprint,
   WNS +1.738 ns / TNS 0, 0 errors and 0 critical warnings.
5. **PENDING RELEASE GATE:** rebuild the six non-default committed bitstreams.
6. **PENDING HARDWARE (r5):** P96 direct and cgxvideo players, 1:1 and resized
   PIP, color key, 318-pixel odd-macroblock regression, 640x480@30 FPS, mode
   changes, close, DPMS, scanlines, and videocap takeover.

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
