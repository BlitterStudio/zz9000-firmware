# ZZ9000 SDK v2 Release Smoke Checklist

Use this procedure to qualify a packaged SDK v2 release with its matching
SDK-service firmware on AmigaOS. Run the commands from the installed package
and replace the example paths with files that load on the test machine.

The checklist qualifies the full service set on Zorro III. For Zorro II, read
the [Zorro II service matrix](zz9k-zorro2-services.md) first and run only the
steps marked for its negotiated profile. Low-level tools that use the legacy
default shared heap are Zorro III diagnostics.

## Procedure

1. Check the service contract.

   ```text
   zz9k-services --check-release
   zz9k-services --all
   zz9k-info
   ```

   **Expected pass signal:** `zz9k-services --check-release` exits with status
   `0` and ends with `release check ok`. `zz9k-services --all` and `zz9k-info`
   complete without mailbox or query errors and report the available services,
   capabilities, and memory information. A missing required capability, service
   flag, opcode range, or major-version match blocks the release pair. On
   Zorro II, `APERTURE_LAYOUT` must be present; Zorro III does not advertise
   that Zorro II-only capability.

2. Run the core surface checks on Zorro III.

   ```text
   zz9k-smoke
   zz9k-surface-info
   zz9k-surfaceops --hold-ticks 0 --loops 1000 --stats --stats-interval 100
   ```

   **Expected pass signal:** each command completes without a timeout or SDK
   status error. `zz9k-surfaceops` reports loop progress with no failed
   operations, and the RTG display remains usable afterwards. On Zorro II, use
   `zz9k-info` and the adapted client checks below instead; `zz9k-smoke`
   requests legacy default shared buffers.

3. Run the Zorro III crypto vectors.

   ```text
   zz9k-hash --alg sha256
   zz9k-hash --alg sha1
   zz9k-hash --alg poly1305
   zz9k-chacha
   zz9k-aead
   ```

   **Expected pass signal:** every built-in hash, ChaCha20, and AEAD vector
   passes. A digest or vector failure blocks archive and TLS qualification. On
   Zorro II, use the accelerated AmiSSL provider self-test or an HTTPS GET
   through an AmiSSL TLS application instead. With the drop-in
   `amissl.library` installed, follow the hardware check in
   [`zz9k-amissl-provider.md`](zz9k-amissl-provider.md); its self-test reports
   `encrypt via 'zz9000'` and `ALL PASS` when offload is active.

4. Test compression and archives.

   ```text
   zz9k-inflate --selftest
   zz9k-archive l Work:Archives/test.zip
   zz9k-archive t Work:Archives/test.zip
   zz9k-archive x --dry-run -o RAM:zz9k-smoke Work:Archives/test.zip
   zz9k-archive l Work:Archives/test.lha
   zz9k-archive t Work:Archives/test.tar.gz
   zz9k-archive l Archives/split-deflate.7z
   zz9k-archive t Archives/split-deflate.7z
   zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-deflate.7z
   zz9k-archive l Archives/split-lzma.7z
   zz9k-archive t Archives/split-lzma.7z
   zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-lzma.7z
   zz9k-archive l Archives/split-lzma2.7z
   zz9k-archive t Archives/split-lzma2.7z
   zz9k-archive x --dry-run -o RAM:zz9k-split Archives/split-lzma2.7z
   ```

   **Expected pass signal:** `zz9k-inflate --selftest` completes its built-in
   payload checks. Each archive command finishes without CRC, path-safety, or
   unexpected unsupported-method errors. The packaged `split-*.7z` fixtures
   list two files. `--dry-run` must not create or overwrite output files.
   Run the archive cases on Zorro II; `zz9k-inflate --selftest` is a Zorro III
   diagnostic.

5. Activate the JPEG, PNG, and MP3 DataType descriptors when they are not
   already active.

   ```text
   copy Storage/DataTypes/ZZ9000-JPEG#? TO DEVS:DataTypes/
   copy Storage/DataTypes/ZZ9000-PNG#? TO DEVS:DataTypes/
   copy Storage/DataTypes/ZZ9000-MP3#? TO DEVS:DataTypes/
   AddDataTypes DEVS:DataTypes/ZZ9000-JPEG
   AddDataTypes DEVS:DataTypes/ZZ9000-PNG
   AddDataTypes DEVS:DataTypes/ZZ9000-MP3
   AddDataTypes LIST
   ```

   **Expected pass signal:** DataType descriptors are activated from
   `Storage/DataTypes`, and `AddDataTypes LIST` shows `ZZ9000-JPEG`,
   `ZZ9000-PNG`, and `ZZ9000-MP3`.

6. Check direct image decoding, the viewer, and picture DataTypes.

   ```text
   zz9k-jpeg Work:Pictures/test.jpg
   zz9k-jpeg --fb --hold 200 Work:Pictures/test.jpg
   zz9k-png Work:Pictures/test.png
   zz9k-png --fb --hold 200 Work:Pictures/test.png
   zz9k-view Work:Pictures/test.jpg Work:Pictures/test.png Work:Pictures/test.webp
   zz9k-dtprobe --client Work:Pictures/test.jpg
   zz9k-dtprobe --client Work:Pictures/test.png
   MultiView Work:Pictures/test.jpg
   MultiView Work:Pictures/test.png
   ```

   **Expected pass signal:** the listed decoders complete without SDK status
   errors. `zz9k-view` opens one resizable window and its next/previous keys
   navigate between the images. Check JPEG, PNG, and WebP in RGB555 (15-bit),
   RGB565 (16-bit), and BGRA8888 (32-bit) RTG modes. Check that resize and
   occlusion redraw through visible clips leave surrounding RTG contents
   intact. MultiView and `zz9k-dtprobe --client` display JPEG and PNG through
   `zz9k-picture.datatype`.

   On Zorro II, skip the plain `zz9k-jpeg` command because it uses a
   legacy-default CPU-visible output tile. Run `zz9k-jpeg --fb`, both PNG
   forms, `zz9k-view`, and the DataType clients. While those clients run,
   `zz9k-info` must continue to report an active acknowledged layout and
   host-window free space must return after close.

7. Check the optional MP3 sound DataType.

   ```text
   AddDataTypes LIST
   MultiView Work:Audio/test-bare.mp3
   MultiView Work:Audio/test.mp3
   ```

   **Expected pass signal:** `AddDataTypes LIST` routes `ZZ9000-MP3` to
   `zz9k-sound.datatype`, and both an untagged Layer III file and a file with a
   leading ID3v2 tag open through the system `sound.datatype`. On version 47,
   stereo output uses 16-bit left and right planes at the source rate; earlier
   versions use planar 8-bit mono. Repeated open and dispose cycles leave free
   memory stable. Layer II files and bare ID3 data must not be claimed.

   If a Layer III file does not open, confirm that its first frame immediately
   follows its ID3v2 tag. The recognition hook does not scan for a later frame.
   See [zz9k-sound-datatype.md](zz9k-sound-datatype.md).

8. Check optional FLAC and Ogg Vorbis descriptors when the matched firmware
   advertises their audio-stream flags.

   ```text
   copy Storage/DataTypes/ZZ9000-FLAC#? TO DEVS:DataTypes/
   copy Storage/DataTypes/ZZ9000-OggVorbis#? TO DEVS:DataTypes/
   AddDataTypes DEVS:DataTypes/ZZ9000-FLAC DEVS:DataTypes/ZZ9000-OggVorbis
   MultiView Work:Audio/test.flac
   MultiView Work:Audio/test.ogg
   ZZPlay --audio=ahi Work:Audio/test.flac
   ZZPlay --audio=ahi Work:Audio/test.ogg
   ```

   **Expected pass signal:** `AddDataTypes LIST` routes `ZZ9000-FLAC` and
   `ZZ9000-OggVorbis` to `zz9k-sound.datatype`; Ogg Opus and Ogg-FLAC are not
   claimed. Firmware without the required `FLAC_STREAM` or `VORBIS_STREAM`
   capability refuses the accelerated path with `ERROR_NOT_IMPLEMENTED`.
   Current firmware leaves these capabilities unadvertised pending physical
   qualification, so refusal is the expected observation for that firmware.

9. Check audio decoding and output.

   ```text
   zz9k-mp3 --stats Work:Audio/test.mp3
   zz9k-mpega-smoke --trace --null-api-check
   zz9k-mpega-smoke --trace --stream-info --stats --freq-max 0 --frames 100 Work:Audio/test.mp3
   ZZPlay --audio=ahi Work:Audio/test.mp3
   ZZPlay --audio=mhi Work:Audio/test.mp3
   ZZPlay --audio=auto Work:Audio/test.mp3
   ZZPlay --audio=none --benchmark Work:Audio/test.mp3
   ZZPlay --audio=ahi Work:Video/test.mpg
   ```

   **Expected pass signal:** `zz9k-mp3 --stats` reports decoded frames and
   samples without stream-status errors. `zz9k-mpega-smoke --trace
   --null-api-check` prints `null-api check ok`; the stream-info run reports
   the installed `mpega.library` version and revision and finishes without a
   decode error. AHI plays the complete MP3 without a truncated tail. On
   ZZ9000AX with `mhizz9000.library`, explicit MHI and `AUTO` use MHI without
   AHI ownership; `AUTO` may fall back before playback if MHI is unavailable.
   The `--audio=none --benchmark` run finishes without opening an audio backend.

   On Zorro II, omit `zz9k-mp3`; qualify accelerated MP3 through
   `mpega.library`, MHI, or ZZPlay.

10. Check MPEG-1 Program Stream video and P96 PIP.

    ```text
    ZZPlay --fps Work:Video/test.mpg
    ZZPlay --benchmark Work:Video/test.mpg
    ```

    **Expected pass signal:** ZZPlay identifies the stream and reports `direct
    planar overlay`. Inspect the pixels before treating FPS output as a pass.
    At 1:1 and after resizing, colors, horizontal placement, and all rows are
    correct. Moving, resizing, obscuring, and closing the PIP leave the desktop
    clean, the pointer remains topmost, and paced and benchmark playback finish
    without SDK errors.

    Resize the PIP away from source dimensions and return it to 1:1. During
    playback, cycle `ZZScanlines` modes, bring a native PAL/NTSC screen forward,
    then return to the P96 RTG screen. Neither transition may expose stale PIP
    or RTG pixels. The 2 MiB Zorro II profile has no PIP pool and skips this
    step. On the matched 4 MiB Zorro II profile, use an aligned source that
    fits the 224 KiB pool, such as 352x288 YUY2; 640x360 must fail without
    reserving memory or changing overlay state.

## Failure routing

- Stop on service discovery, mailbox, capability, opcode, or major-version
  failures. Reflash a firmware image from the matching release pair before
  investigating higher-level clients.
- Route allocation, shared-buffer, or surface errors to the memory and surface
  services. A slower `zz9k-surfaceops` run while audio is active is contention
  evidence unless a command times out or returns an error.
- Route crypto-vector and TLS-offload failures to the crypto service and the
  AmiSSL provider. Route archive CRC, path-safety, overwrite-policy, or
  streamed-extraction failures to `zz9k-archive`.
- Route direct image decode failures to the image service or shared-buffer
  path; route MultiView-only failures to `zz9k-picture.datatype`.
- Route AHI failures to accelerated decode or AHI output, MHI-only failures to
  `mhizz9000.library` and ZZ9000AX ownership, and MPEGA-only failures to the
  resident compatibility shim.
- Treat corruption outside the PIP as a stop condition. Preserve the UART log
  and the exact artifact hashes.

