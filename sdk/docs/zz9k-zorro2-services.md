# Zorro II service support

The current generation-2 Zorro II layout gives both the 2 MiB and 4 MiB
apertures a 16 KiB CPU-visible host window. `zz9k-archive` and the adaptive
AmiSSL provider can use that window. Several older client paths still request
larger buffers and therefore cannot run their accelerated path on a
current-generation layout.

Keep the FPGA bitstream, firmware, RTG driver, and SDK from one release set.
The FPGA reports the aperture layout and its generation. The RTG driver
validates that layout against the AutoConfig aperture and acknowledges it
before firmware publishes the generation-2 host window or mailbox. An invalid
or unacknowledged layout leaves those regions unavailable.

## Layout profiles

| Aperture | Generation-2 host window | PIP source pool | Firmware layout |
| --- | ---: | ---: | --- |
| 2 MiB | 16 KiB | none | Audio direct-ring reservation and compact host window |
| 4 MiB | 16 KiB | 224 KiB | Audio direct-ring reservation, compact host window, and PIP source pool |
| 8 MiB | 80 KiB | 256 KiB | Defined by the firmware layout code |

The host window is shared by all CPU-visible SDK allocations. Close idle
clients before retrying an allocation. `zz9k-info` reports the aperture state,
raw layout, host-window total and free space, largest free block, allocation
count, and invalid-slot count.

Generation 2 reserves 48 KiB from the former 64 KiB host window for one Zorro
II audio direct-ring grant. Clients obtain the grant geometry from
`SDK_OP_AUDIO_RING_ACQUIRE`; the reservation is unavailable to the shared
allocator.

## Current client matrix

| Client or operation | 2 MiB | 4 MiB | Current behavior |
| --- | --- | --- | --- |
| `mpega.library` | No | No | Its compact PCM ring is 32 KiB, followed by 16 KiB feed staging. Neither layout has a 32 KiB host window. |
| ZZPlay with AHI | No | No | The compact AHI path allocates a 32 KiB PCM ring and 16 KiB staging buffer. |
| ZZPlay card-local streaming | Limited | Limited | The card-local pass keeps PCM and compressed rings card-only and uses up to 16 KiB of host-window staging. It requires an otherwise free host window. |
| Accelerated `amissl.library` | Yes | Yes | The provider advertises hardware algorithms only after a 32-byte host-window probe succeeds. Scratch allocations are 16-byte aligned and grow to the requested size. X25519 is offload-or-fail after advertisement. P-256 and AEAD operations can use the default provider after an offload miss. RSA-2048 PKCS#1/SHA-256 verification uses the default provider by default; set `ENV:ZZ9K_RSA_VERIFY_OFFLOAD` only to test the board path. |
| `zz9k-view` JPEG, PNG, and WebP | No | No | Each viewer path requests a 32 KiB host-window staging buffer. |
| `zz9k-picture.datatype` | Limited | Limited | Input staging is capped at 24 KiB and output tiles at 32 KiB. The accelerated path can run only when its chosen input and tile allocations fit the remaining host window. |
| `zz9k-archive` streamed decode, test, and extract | Yes | Yes | Decode and extract use a combined CPU-visible budget of `min(48 KiB, host window)`, split between input and output. Verify-only output can be card-only. The 16 KiB profile therefore starts with 8 KiB chunks. A failed pair allocation retries at half size down to 4 KiB. |
| ZZPlay MPEG Program Stream video/PIP | No | No | Current code accepts only an acknowledged generation-1 PIP layout. On that legacy 4 MiB layout, the 224 KiB pool fits a 352x288 YUY2 frame (202,752 bytes) and rejects 640x360 (460,800 bytes). |

## External integration notes

The following compatibility facts belong to the RTG and audio integrations,
which are outside this source tree:

| Integration | 2 MiB | 4 MiB | Constraint |
| --- | --- | --- | --- |
| `mhizz9000.library` | Yes | Yes | Uses the host window for feed staging and requires ZZ9000AX hardware. |
| ZZPlay MP3 with MHI | Yes | Yes | The external MHI path has separate audio ownership from AHI. |
| General P96 offscreen bitmaps | No | No | The fixed PIP pool is for one bounded source allocation; general offscreen allocation remains disabled. |
| Zorro III Fast RAM or a Z3-sized shared heap | No | No | The Zorro II aperture does not provide either facility. |

## Checking a board

1. Run `zz9k-info` after the matched stack has started.
2. Confirm that the aperture state is `active` and that the reported layout
   generation, aperture size, host window, and PIP pool match the board.
3. Run one host-window client at a time first. Use the memory counters in
   `zz9k-info` when combining clients.

Generation 1 used a 64 KiB host window on the 2 MiB and 4 MiB layouts.
Descriptor-absent legacy 4 MiB stacks retain that fixed window. Legacy 2 MiB
stacks and unknown aperture sizes do not use it. Generation 2 is the current
layout and requires the acknowledgement above.

## Diagnostic scope

`zz9k-smoke`, `zz9k-inflate`, `zz9k-mp3`, `zz9k-hash`, `zz9k-chacha`,
`zz9k-aead`, and `zz9k-cryptobench` allocate their accelerated buffers from
the default shared heap. That heap is not CPU-visible on Zorro II, so these
tools do not qualify the adapted clients. `zz9k-jpeg <file>` also uses a
default-heap tile output path.

Use the accelerated AmiSSL provider for TLS and `zz9k-archive` for archive
work when qualifying the generation-2 Zorro II paths. Test MHI and RTG
integration against their own components.

## Status

Recorded hardware testing on a 4 MiB Zorro II A2000TX found that ZZPlay MP3
with MHI played normally and that AmigaAMP worked after its MHI settings were
reset. AHI and MHI cannot own the ZZ9000AX at the same time.

The wider hardware matrix remains unqualified. It still needs 4 MiB PIP
allocation and close cycles, interleaved overlay and RTG operations, a warm
reset, and image, DataType, archive, AmiSSL, and audio host-window clients run
alone and under contention while monitoring `zz9k-info` memory diagnostics.
