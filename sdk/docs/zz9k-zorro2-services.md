# Zorro II service support

The current matched firmware, FPGA bitstream, RTG driver, and SDK extend a
selected set of ARM services to Zorro II without pretending that its aperture
is equivalent to Zorro III. The FPGA publishes an aperture-relative layout,
the RTG driver validates it against the AutoConfig size and reserves every
region, and the driver then acknowledges it. Firmware exposes the compact host
heap and optional PIP pool only after that two-sided handshake.

Keep all four components from the same release set. The aperture layout has
two generations: generation 1 (the previously supported layout, still
published by older matched sets and older FPGA/firmware pairs) and generation
2 (current matched sets), which shrinks the negotiated host heap to 16 KiB on
the 2 MiB and 4 MiB profiles and reserves the freed 48 KiB for the Z2 audio
direct-ring grant. Descriptor regions, including the optional PIP pool,
require the driver validation and ACK handshake with the descriptor's own
generation token: an invalid or unacknowledged descriptor fails closed, and
`zz9k-info` reports the raw and validated layout. Descriptor-absent legacy
4 MiB stacks intentionally retain the historical fixed 64 KiB `HOST_WINDOW`
path; this compatibility path does not expose negotiated layout or PIP
features. Legacy 2 MiB stacks and unknown aperture sizes reject that path.

## Shipped profiles

| Profile | Host heap (gen 2) | Host heap (gen 1 / legacy) | PIP source pool | Practical result |
| --- | ---: | ---: | ---: | --- |
| 2 MiB Zorro II / A500 | 16 KiB | 64 KiB | none | Compact audio, image, archive, and AmiSSL clients; no ZZPlay video |
| 4 MiB Zorro II / A500 | 16 KiB | 64 KiB | 224 KiB | The same compact clients plus one bounded packed-YUV PIP source |

The source also defines a software-side 8 MiB profile, but there is no shipped
or verified 8 MiB AutoConfig bitstream variant. Do not select or advertise it
as a supported board profile.

The host heap is shared by every CPU-visible SDK client. It is not provided
per application. Card-only compressed, PCM, and decode rings do not consume
it, but two clients whose visible working sets exceed the acknowledged
generation's heap cannot run their accelerated paths concurrently. Close idle
clients before diagnosing an allocation failure; `zz9k-info` reports total,
free, largest-block, and invalid allocation counters.

## Client matrix

| Client or operation | 2 MiB | 4 MiB | Limits and fallback |
| --- | --- | --- | --- |
| `mpega.library` | Yes | Yes | Card-only MP3 ring plus compact host-visible PCM/staging buffers. |
| `mhizz9000.library` | Yes | Yes | Uses the host window for feed staging; requires the ZZ9000AX hardware. |
| ZZPlay standalone MP3 with AHI or MHI | Yes | Yes | Uses a card-only compressed ring and compact PCM/staging windows. |
| Accelerated `amissl.library` | Yes | Yes | X25519, P-256 ECDHE, P-256 ECDSA verify, RSA-2048 PKCS#1/SHA-256 verify, AES-GCM, and ChaCha20-Poly1305 use persistent exact-size host-window scratch. Provider open allocates a 32-byte probe to gate advertisement; if it fails, AmiSSL stays on its software provider. Other 16-byte-aligned slots grow lazily. A later allocation miss follows the operation's failure semantics: P-256 keygen/derive, ECDSA-P256 verify, RSA-2048 verify, and record crypto (AES-GCM and ChaCha20-Poly1305) fall back to software; X25519 is offload-or-fail and returns failure once advertised. |
| `zz9k-view` streaming JPEG/PNG | Yes | Yes | Uses compact host-window staging. Image size is not limited to the heap because compressed data is streamed. |
| `zz9k-picture.datatype` | Yes | Yes | Uses at most 24 KiB of compressed-input staging plus geometry-derived output tiles capped at 32 KiB. A row that cannot fit the tile cap rejects the accelerated DataType path without overrunning the window; fallback then depends on the caller's installed DataType selection. |
| `zz9k-archive` streamed decode/test/extract | Yes | Yes | CPU-visible feed buffers are sized from the acknowledged host window: the combined cap is `min(48 KiB, host heap)`, split evenly between the input and output chunks, so a generation-2 profile (16 KiB heap) streams with 8 KiB chunks. The two chunks are allocated as one retry unit that shrinks in halves down to 4 KiB, so a window contended by another resident client degrades to a balanced smaller pair instead of failing the member; a terminal allocation failure names the window size alongside the shared-heap and host-window free/largest-block counters. Large LHA batch arenas are bypassed and retain the per-member/software fallback chain. |
| ZZPlay MPEG-1 Program Stream video/PIP | No | Limited | Requires the 4 MiB profile and one full aligned YUY2 frame to fit the 224 KiB pool. 352x288 fits; 640x360 does not. P96 owns one fixed source allocation at a time. |
| Arbitrary P96 offscreen bitmaps | No | No | The fixed pool is exposed only for the bounded PIP allocation; general offscreen allocation remains disabled. |
| Zorro III Fast RAM or a Z3-sized shared heap | No | No | Neither facility can be reproduced usefully inside the Zorro II aperture. |

Several low-level diagnostics still call `zz9k_alloc_shared()` with the legacy
default flags and therefore do not map their buffers through the negotiated
Zorro II host window. This includes `zz9k-smoke`, `zz9k-inflate`, `zz9k-mp3`,
`zz9k-hash`, `zz9k-chacha`, `zz9k-aead`, and `zz9k-cryptobench`. Plain
file-output `zz9k-jpeg <file>` belongs in the same diagnostic category: its
streaming input uses `HOST_WINDOW`, but its non-framebuffer tile output uses
the legacy default allocation. On Zorro II, `zz9k-smoke`, `zz9k-inflate`,
`zz9k-hash`, `zz9k-chacha`, `zz9k-aead`, and `zz9k-cryptobench` print an
explanatory note at startup, before their accelerated sections fail;
`zz9k-mp3` explains at the failing step and falls back to streaming decode,
and `zz9k-jpeg` reports allocator diagnostics at the failure. Their failure on
Zorro II does not mean the adapted production client for the same service is
broken. For example, qualify TLS with the accelerated AmiSSL provider, archive
decoding with `zz9k-archive`, and MP3 with `mpega.library`, MHI, or ZZPlay
rather than those generic allocation diagnostics.

## Qualification status

The layout, allocator, client guards, RTG reservation, and 2/4 MiB bitstream
contracts have host-side regression and cross-build coverage. The existing
MHI path has also been confirmed on a 4 MiB Zorro II A2000TX: ZZPlay MP3/MHI
played normally, and AmigaAMP worked after its MHI settings were reset. AHI and
MHI still cannot own the ZZ9000AX simultaneously; that is the existing audio
ownership contract, not an aperture failure.

The wider matrix is not yet physically qualified. Before calling it
hardware-validated, exercise 4 MiB PIP allocation/close cycles, interleave
overlay and normal RTG operations, warm-reset the card, and run image,
DataType, archive, AmiSSL, and audio host-window clients alone and in
contention while watching `zz9k-info` memory diagnostics.
