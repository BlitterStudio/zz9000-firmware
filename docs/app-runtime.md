# ZZ9000 app runtime

This is the firmware and launcher contract for the app kernel, which
implements AAR API 0.5 (BlitterStudio/aar, pinned by `sdk/AAR_REF`). The public
app interface is [aar-api.md](https://github.com/BlitterStudio/aar/blob/main/docs/aar-api.md); application
build and usage guidance is in
[aar-developer-guide.md](https://github.com/BlitterStudio/aar/blob/main/docs/aar-developer-guide.md). The shared
wire definitions in `ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h` are the
source of truth for byte offsets and constants. [App interface](#app-interface)
below lists what this firmware does where the standard leaves a choice.

## Kernel architecture

Core 1 runs a small kernel. It has one highest-priority system thread and one
app thread per running app. The number of apps is not a constant: it is the
record capacity chosen at boot from card memory (231 on this board, see
[Record arena](#record-arena)). An app process is a record that owns its
address space, memory commit, resource ledger, launcher window binding, and
handle. Its thread owns the saved user context, kernel stack, scheduler state,
and CPU accounting. There is one app thread per process today.

An exception frame is the saved context. It includes the interrupted user
registers, SYS `sp` and `lr`, SPSR, VFP registers, and FPSCR. A new app starts
from a synthetic frame with the ELF entry point, API table, `argc`, `argv`,
and user stack. The exception return restores the selected frame without a
nested call into app code.

### Process records and handles

An app is a process record. A record is named by its index, the pid, and the
index always selects the same record, core-1 kernel stack, L1 table, and
user-table pages: they are parallel arrays carved once at boot from one
card-pool block. A record is never returned to an allocator, so core 0
reading a record that core 1 has just retired sees a generation mismatch, not
freed memory (`zzapp_rec.h:15-20`, `zzapp_rec.c:201-219`).

A handle is `(generation << 16) | pid`, built with `aar_handle_make`: a
16-bit index and a 16-bit generation from 1 to `0xFFFF`, so a handle is never
0 (`zzapp_rec.h:9-13`, `zzapp_rec.c:311`). Freeing a record bumps its
generation first, and the generation wraps from `0xFFFF` to 1, so a handle
from an earlier life of the index fails the lookup
(`zzapp_rec.c:382-425`). Free indices wait in a FIFO queue, so an index is
reused as late as possible (`zzapp_rec.c:297-323`). The table holds at least 8
and at most 1024 records (`ZZAPP_REC_MIN_APPS`, `ZZAPP_REC_MAX_APPS`,
`zzapp_rec.h:42-43`).

A record is one 12 KiB slot (`ZZAPP_REC_RECORD_BYTES`) holding the kernel
thread, the words core 0 polls, the per-app scheduler words, the claim
words (limits, load progress, terminal and refusal reasons), the process
memory ledger and its address space, the host state, the 64-entry
host-visible block table, and the services, loader, SVC, and decode state
(`zzapp_rec.h:274-295`). Its states are `CLAIMED`, `LIVE`, `RETIRING`,
`RECLAIMED` (resources returned, shell held for a quarantined window), and
`FREE`; they only move forward until the free (`zzapp_rec.h:62-72`).

The launcher wire has not changed: a request names a slot and a 32-bit
generation. The wire slot names a launcher window, a wire-layer object that is
bound to a record while a claim is live (`zzapp_services.c:233-269`,
`zzapp_services.c:4868-4897`). Handles are internal until the transport swap
(slice 6) lets requests name them.

### Record arena

At boot one block of the card pool holds the L1 tables, the core-1 stacks, the
records, the user API and GL table pages, and the shared L2 table pool. It is
taken in class `CARD_POOL_CLASS_APP_ARENA`, 1 MiB aligned, after
`card_pool_setup_card()` and before `zzapp_as_init()` and before any app
extent exists (`main.c:1343-1361`, `zzapp_rec.c:155-179`). The class is
permanent like `FIRMWARE`: the block is never placed in the revocable
Fast-RAM tail, survives an Amiga reset, is never freed, and has no class cap
(`card_pool.c:181-184`, `card_pool.h:63-68,87`). The old fixed 208 KiB
translation-table slab, its linker section, and the per-slot static arrays are
gone; nothing in the image is sized by an app count.

The arena is sized by memory (`zzapp_rec.c:55-91`):

- Records: `(permanent card-pool bytes - 128 MiB system reserve) / 2 MiB`,
  bounded to 8 at the least and 1024 at the most. 2 MiB is the commit floor of
  one app, an image frame plus a stack frame.
- Per record: a 16 KiB L1 table, a 32 KiB core-1 stack, the 12 KiB record,
  and 8 KiB of user table pages (`zzapp_rec.h:52-57`), 68 KiB in all, plus the
  16 L2 tables it is guaranteed.
- L2 pool: 16 tables for the kernel template, 16 per record, and a shared
  remainder of 2 tables per MiB of host-visible heap, rounded up to a multiple
  of 32. A table is 1 KiB plus 6 bytes of bookkeeping (`zzapp_as.h:37-54`).
- The total is rounded up to 1 MiB. If the block does not fit, the carve
  retries with half the records down to 8; if even 8 do not fit, no app can
  run and the boot log says `record arena: no memory, apps unavailable`.

This board: the permanent prefix of the card pool is `0x0B000000` to
`0x2FE00000`, 590 MiB. That gives 231 records, 3872 L2 tables (16 template,
3696 reserved, 160 shared for the 78.0625 MiB host-visible heap), and a 20 MiB
arena, 3.4 percent of the prefix (`test/zzapp/h/rec_test.c:94-114`). Boot
prints `[zzapp] record arena N KiB at 0x...: N records, N L2 tables (N
shared)`. The commit ledger sees the arena automatically, because the block
is allocated from the pool before any app extent.

The arena uses the BSP's Normal, shareable write-back mapping, including the
record words polled by core 0. The former per-slot block in the task-queue
region also used write-back memory; it was not a non-cacheable page. The
global heartbeat stays in that region. SCU snooping makes these words visible
while both cores participate in coherency. Reset-time cache handling is
described below.

### Scheduling and doorbell

The core-1 private timer gives apps a 5 ms quantum. The scheduler first runs
due system work: SDK tasks, MiniGL service, launcher work, and audio feeding.
It then selects runnable app threads round-robin. In an overload window,
service work is limited so runnable apps retain a 5% floor.
Deferred launcher operations are selected round-robin across launcher windows.
Each service pass advances at most one window by one bounded step, preserving
per-window request order without letting a long `LOAD_END` starve another
window's `LOAD_BEGIN`, `LOAD_CHUNK`, or `START`
(`zzapp_services.c:4621-4633`). The scheduler walks a dense run list of the
records whose thread is not stopped, not the record capacity
(`zzapp_kernel.h:186-196`).

A system-service pass runs to completion, and no app runs on core 1 meanwhile.
A CPU-bound app therefore gets about one service turn per 5 ms timer quantum;
there is no universal 1 ms guarantee. The app loader zeroes at most 32 KiB,
maps at most 96 KiB, copies at most 48 KiB, extracts at most 512 relocation
entries, or applies at most 256 per pass. Loading swquake on the card, the
longest pass of each kind measured, in XTime ticks at 333 MHz: zero 138667
(0.42 ms), copy 59571 (0.18 ms), map 159258 (0.48 ms), relocation
extraction 295814 (0.89 ms), relocation application 110622 (0.33 ms),
allocation 13392 (0.04 ms), other worker work 227020 (0.68 ms). The
progressive-JPEG coefficient step is bounded to about 1 ms.
An on-demand v3 module load reads and hashes at most 12 KiB, then zeroes,
flushes, invalidates, or failure-poisons at most 64 KiB. A FLAC, Vorbis, or
MP3 decode pass produces at most 24 KiB of PCM plus one frame. A measured
24 KiB pass took 1172743 XTime ticks, about 3.5 ms at 333 MHz.
At 44.1 kHz stereo S16, 24 KiB is about 139 ms of playback and needs about
7.2 refill passes per second, well below the roughly 200 service turns per
second available to a CPU-bound app when the pump refills below low water.

Those pass bounds do not make 64 KiB uploads, MiniGL batches, or whole image
and JPEG operations universally 1 ms. Boot-time `IMAGE_SESSION_FEED` reached
14.9 ms in the same measurement and is not covered by these app-runtime pass
bounds. The mailbox JPEG task decodes two rows per invocation and invalidates
or flushes at most 32 KiB of cache per invocation; host bound tests do not
cover those limits. Image setup, scaling, and non-coefficient work remain
outside the progressive-JPEG step bound. TLS key exchange and signature
checks offloaded by the AmiSSL provider cannot be split: one X25519, P-256 or
RSA-2048 operation holds core 1 for about 5 to 25 ms, so a handshake stalls
running apps for that long.

An API body remains unpreemptible while it owns the API lock. Its internal
explicit `aar_yield` safe point releases that lock, lets one due system-service
pass run, then reacquires the lock before returning to the body. A standalone
`aar_yield` rotates to another runnable app and runs a system-service pass only
when service work is due. This bounds audio-service delay at cooperative yield
points to the current API segment plus one scheduler handoff; a body that does
not yield remains subject to the API watchdog rather than a scheduler preemption.

File and directory operations, console read and backpressure, display open and
flip, non-host audio open and close, `aar_msg_wait`, and sleep park the caller
at the SVC handoff. `aar_gl_swap` parks through its flip. Completion wakes the
slot READY; file and directory waits wake TIMEOUT after 30 seconds and return
`AAR_E_TIMEDOUT`. Stop wakes a parked waiter (an EVENT waiter wakes READY and
then sees the stop); waiting service calls return `AAR_E_IO`. `aar_event_poll`
and `aar_display_vsync` do not park; the
latter only yields once.

`aar_msg_wait` parks as an `EVENT` wait with the caller's timeout as its
deadline. The Amiga writes the host-to-app ring in the launcher window and
rings no doorbell, so the wait is a level test. The service pass reads the
ring head and tail for every parked `EVENT` waiter and wakes it when they
differ. The waiter reads the ring again after every resume and before it
parks, so a message that lands in that gap is found by the next service pass.
A queued message ends the wait before a pending stop does. Like every
declared wait it keeps the slot out of the 30-second stall and 10-second
stuck-call watchdogs.

Core 0 does not wait for core 1. It publishes work and raises doorbell SGI 15
only when `service_pending` changes from zero to one. Core 1 drains a bounded
batch, rechecks the word before clearing it, and therefore cannot lose a wake
or require one SGI per task. Timer IRQ, doorbell SGI, and SVC handoff save the
current frame, choose the next thread, switch the address space when needed,
and return through that frame.

### Performance counters and probes

`ZZAPP_PERF_COUNTERS=1` enables the kernel measurements. They are sampled on
the SVC cadence and on exception-driven switches; `ZZAPP_PERF_COUNTERS=0`
removes their code and storage. The PMU runs only in privileged core-1 code:
it supplies context-switch cycles and per-slot instruction/data TLB-refill
counts, and is also initialized when `ZZAPP_SVC_PROBES=1` with counters off.
The other scheduler latencies and every `*_span` are XTime global-timer ticks:
`COUNTS_PER_SECOND` is CPU/2, or about 333 MHz on the 666 MHz CPU.

While an app runs, periodic report requests are coalesced and emitted at most
once every ten seconds across all slots; terminal paths record a `forced=1`
report that is distinguished in the emitted line. The system-thread service
path builds the report while it holds the kernel lock, then releases that lock
before writing the resulting `[app] perf` line; neither exception handling nor
a context switch prints or touches the UART. Each `p50/p99/max` triplet is
reported as `p50/p99/max`. `switch` starts at the vector timestamp before the
full scheduler frame is saved and ends after the selected frame is fully
restored, immediately before the final exception-return instruction. It
includes vector frame save, IRQ dispatch, scheduler/context copies, address
space switching, and frame restoration; it excludes hardware exception entry
and the final return instruction. `sgi` is one doorbell to the start of the
next worker service pass, and `sdk` is each task's enqueue publication to its
start. Sampling the pass boundary preserves the hot-path publication protocol;
first-service attribution would require coupling every cross-core work claim
to the publisher.
`sgi_burst` is `SGIs/work publications`; publications include SDK tasks,
launcher deferred operations, audio refill triggers, and quiesce requests, so
a value below one shows doorbell coalescing. `sdk_span`, `minigl_span`,
`launcher_span`, `audio_span`, and `svc_span` are the 99th-percentile/maximum
non-preemptible spans for SDK tasks, MiniGL, launcher deferred work, audio
pumping, and sampled API-lock-held SVC work.
`launcher_steps=zero:N/copy:N/map:N/reloc:N/apply:N/alloc:N/other:N`
reports the exact longest tick span observed for each launcher-step kind.
Each field is written only when that kind sets a new maximum; the entire
attribution is compiled out with `ZZAPP_PERF_COUNTERS=0`.
`audio` is `low_water/starved/underruns`: low-water crossings on both AHI and
card-fabric routes, retryable fabric fill failures, and played underruns.
The zero-underrun gate uses only the final `underruns` value. `tlb` is
instruction/data refills, sampled in the one-second windows that open when a
perf report is requested (reports are ten seconds apart): the counts cover
those windows only, not the full run, and are per-slot totals of
micro-TLB refills observed while the slot held the core.
Histogram percentiles are power-of-two buckets, so the exact maximum is the
final number in each pair or triplet.

`ZZAPP_SVC_PROBES=1` adds the `svcprobe` veneers used to characterize bare SVC
paths. It defaults to off and is measurement tooling, not an application ABI.
Build and run the dedicated probe firmware when enabling it; ordinary firmware
does not expose those probe numbers.

### Address spaces and SVC

Each record has a private 16 KiB L1 table at its index in the arena, copied
from a privileged kernel template when the address space is created
(`zzapp_as_pool_l1`, `zzapp_as_create`, `zzapp_as.h:112-141`). The private
virtual window is `0xC0000000`–`0xDFFFFFFF`. Image, heap, and stack are mapped
there as non-global sections, 64 KiB large pages, or 4 KiB small pages as
appropriate. The app sees only its private pages plus grants for its launcher
window, host-visible blocks, and owned display surfaces. The rest of DDR,
firmware, devices, and other apps remain privileged-only.

**L2 tables.** One pool of L2 tables serves every address space and the kernel
template. Admission reserves 16 tables for each record: image sections that
mix permissions, the first alias section, and slack (`zzapp_as.h:41-43`). A
record's allocations use its own reserve first and then draw from the shared
remainder, the free tables not promised to another record
(`zzapp_as.c:459-526`). A table allocation that cannot be given makes the map
call fail with `ZZAPP_AS_ENOMEM`, leaves the address space intact, and is
counted in `tables_refused`. The one place that cannot report the failure to an
app is the BSP translation-change hook; there the record is stopped with a
forced fault, never the kernel (`zzapp_as.c:1641-1735`,
`zzapp_host.c:2761-2768`). Destroying an address space returns every table it
owns. An address space tracks its alias ranges as at most 96 extents
(`zzapp_as.h:48-49`).

**ASIDs.** Hardware ASIDs 1 to 255 are handed out lazily: a record gets one at
its first dispatch, not at creation (`zzapp_as_switch`, `zzapp_as.c:1738-1743`).
Assigned ASIDs sit on a least-recently-dispatched list. When none is free, the
least recently dispatched ASID whose owner is not the running record is
stolen: its translations are flushed by ASID, its generation is bumped, the
branch predictors are invalidated, and the old owner finds the generation
mismatch at its next dispatch and takes another ASID
(`zzapp_as.c:1161-1205`). A retiring record flushes its ASID by ASID before
the ASID goes back to the free stack, so a free ASID never has live
translations and assigning one needs no flush (`zzapp_as.c:1207-1226`). On
this board 231 records fit in 255 ASIDs, so the steal path does not run. The
allocator runs on core 1 with local IRQs masked and takes no lock; core 0
touches it only with core 1 halted, which resets every ASID and flushes the
whole TLB (`zzapp_as.c:1229-1238`).

A host-visible block uses the same mapping as the rest of DDR, Normal
write-back shareable (descriptor `0x15de6`). Zorro III accesses reach DDR
through the Zynq ACP (`m00_axi` of the Zorro master, ARCACHE `0xF` for reads
and AWCACHE `0x3` for writes). The ACP snoops the ARM L1 caches, so neither
side needs ARM cache maintenance. The 68k's own caches are outside the ACP:
a 68k client reads through a cache-inhibited mapping, as `zz9k.library` does
for the launcher window, or maintains its caches itself. The Zorro
master's second port, `m01_axi`, goes to HP1 but only writes video-capture
frames; it carries no Zorro access to the host-visible heap.

The app-facing `aar_api`, `aar_gl` and `aar_ext_zz9000` tables keep the layouts
and `size` semantics of `aar/app.h`, `aar/gl.h` and `aar/ext_zz9000.h`, but
their entries, including `gl_bind_flags` in the extension table, point to
user-RX `svc; bx lr` veneers. The extension table's strings live in the app's
read-only table page. On a user veneer call,
the vector copies the exception frame to that slot's kernel stack, switches to
SYS mode, and enables IRQs before invoking the handler. Every SVC handler
checks user pointers against the caller's mappings and permissions before
touching them. Invalid, overflowed, read-only-for-output, device, and
other-slot pointers return `AAR_E_FAULT`. Firmware code never becomes a
user-call target.

GL client-array state is per slot. `DrawArrays` validates every enabled vertex,
colour, and client texture-coordinate element in its requested range.
`DrawElements` also validates its index buffer and every indexed element before
MiniGL is called. An unmapped or changed range returns `AAR_E_FAULT` and records
`GL_INVALID_VALUE` for `glGetError`.

### Frames, commit, and teardown

Private memory is allocated as zeroed 1 MiB frames from aligned 8 MiB
card-pool extents. Admission is atomic: aggregate commit plus the request
must fit in pool free memory after the system reserve and any per-app launcher
limit. An app commits its image, its stack, and the `min_heap_mib` its note
asks for; there is no fixed per-app memory block. `heap_grow` and
`heap_shrink` add or return whole frames while keeping the heap VA range
fixed. The commit is taken at the first `LOAD_END` step, all or nothing and
before any zeroing, because the image size is not known earlier
(`zzapp_loader.c:535-575`).

All terminal paths end in one idempotent retire function, `zzapp_rec_retire`,
the only place the resources of an app are returned. Callers that end a
running app first latch the end reason and run the end-of-app services: stop
new calls, wake and cancel waits, quiesce work and DMA holding app pointers,
hand off or unbind display, detach audio, release GL and decoder sessions.
The retire then runs in stages and records the last completed one in the
record, so a restarted core 1 resumes after it (`zzapp_host.c:1261-1325`,
`zzapp_rec.h:74-90`): detach the thread from the scheduler, return the
host-visible blocks, grants, staging, and back buffer, destroy the address
space (alias extents, L2 tables, L1, ASID) and return the L2 reserve, return
the frames to the commit ledger, and finally detach the window and free the
record with a new handle generation. It runs on core 1, or on core 0 while
core 1 is halted. The end reason is the firmware-latched one, or `EXIT` when
none was latched. A window remains `STOPPING` until audio teardown is
acknowledged and any running deferred load cancellation completes; only then
can it become reusable. `LIST` retains the reason after the window returns to
idle. The reasons are none, normal exit, forced stop, user fault, stall
watchdog, stuck API call, lease loss, Amiga reset, and core-1 restart.

A fault retires the app at once. The record is freed in the same pass that
stops the thread, so the window reads `IDLE` and the next claim can start;
there is no `FAULTED` latch to wait out. The fault words (fault code, `pc`,
`sp`, `lr`, `far`, `fsr`, `cpsr`) and the terminal reason `USER_FAULT` stay in
the window's lifecycle block, and `STATUS` and `LIST` keep returning them,
until the next `LOAD_BEGIN` of that window clears them
(`zzapp_host.c:1069-1143`, `zzapp_loader.c:1052`,
`test/zzapp/b/reset_lifecycle_test.c:246-256`).

Only user-mode faults stop a single app. A fault in firmware code restarts
core 1 and ends every app. Per-app 30-second stall and 10-second stuck-call
watchdogs stop only their app. A separate core-0 backstop cold-restarts core 1
after two seconds without both progress and timer ticks; that ends every
live record with the restart reason.

## App interface

The firmware implements the AAR 0.5 app table (`aar_api`, `aar/app.h`), the GL
table (`aar_gl`, `aar/gl.h`) and the ZZ9000 extension table
(`aar_ext_zz9000`, `aar/ext_zz9000.h`) on the process records and the launcher
window wire described below. Calls are `aar_*`, constants `AAR_*`, status
codes `AAR_E_*`. The `zz_*`, `ZZ_*` and `zzapp_*` names of the 0.4 header are
gone with no compatibility layer; `docs/zzapp-to-aar.md` in the aar repository
maps them. The firmware's hardware names (`ZZ_*` audio, Ethernet, RasterIX and
MiniGL registers) are unchanged. `QUERY` reports 0.5 in its ABI major and
minor words (`ZZAPP_ABI_MAJOR`, `ZZAPP_ABI_MINOR`).

**Groups.** `aar_api.groups` and `aar_info.groups` report the same mask:
`AAR_GROUP_INPUT` and `AAR_GROUP_SERVICES` always, and `AAR_GROUP_DISPLAY`,
`AAR_GROUP_AUDIO`, `AAR_GROUP_GL` when the launcher capability word has the
matching bit. `audio_stats` is `NULL`: the runtime keeps no underrun count.

**Note v1.** An image must carry exactly one AAR note (owner `AAR`, type 1, a
40-byte descriptor of ten little-endian words), found in a `PT_NOTE` segment or,
when no segment holds one, in a note section. The loader checks, in the
descriptor's order: magic `0x4E524141` and version 1 (`AAR_E_INVAL`); profile
A32 (`AAR_E_PROFILE`); `api_major` 0 and `api_minor` at most 5
(`AAR_E_INVAL`); `stack_bytes` 0 for the default 1 MiB, else 64 KiB to 1 MiB
and a multiple of 16 (`AAR_E_INVAL`); the reserved word 0; every
`groups_required` bit present in the runtime mask (`AAR_E_NOGROUP`); and
`min_heap_mib` within the 512 MiB private window (`AAR_E_NOMEM`). No note, a
second AAR note, or a descriptor of the wrong size is `AAR_E_INVAL`. The load
status the launcher sees maps `AAR_E_INVAL` and `AAR_E_PROFILE` to
`BAD_REQUEST`, `AAR_E_NOGROUP` to `UNSUPPORTED`, and `AAR_E_NOMEM` to
`NO_MEMORY`; a profile or group refusal prints its reason in the `[app] reject
op` line. The groups a note names (required and optional) set the display,
audio and GL bits of the lifecycle flags that the launcher reads. An image
without a v1 note, including one that carries a 0.4 note, does not load.

**Events.** `aar_event_poll` always delivers `AAR_EV_STOP` and
`AAR_EV_LOW_MEMORY`, and delivers the launcher's key, mouse, joystick and
break events only when the runtime has `AAR_GROUP_INPUT`. One stop request
yields one stop event: the firmware queues it itself, and the launcher's own
stop event for the same request counts as that delivery, so the second is
dropped. The low-memory event carries no fields other than its type.

**Files.** The open flags travel unchanged from the app to the launcher
window (`zzapp_file_wire.flags` at `FILE_OFF+12`): `AAR_FILE_READ` 1, `WRITE` 2,
`CREATE` 4 (create if missing, keep an existing file), `TRUNCATE` 8, `EXCL` 16
(with `CREATE`, fail when the file exists). The firmware refuses an invalid
combination with `AAR_E_INVAL` before it reaches the launcher: an unknown bit,
neither `READ` nor `WRITE`, `CREATE` or `TRUNCATE` without `WRITE`, or `EXCL`
without `CREATE`. Create-or-replace is `CREATE | TRUNCATE`.

**Shared buffers.** `aar_shared_alloc` takes flags 0 and returns a block from
the host-visible heap, rounded to the 4096-byte granule, with at most 64 live
blocks per app and the `shared_limit` described under `LOAD_BEGIN`. The
result's `haddr` is the board-relative Zorro III offset of the block (the card
address minus the board's window base), `ptr` is the app mapping, and `flags`
always has `AAR_SHAREDF_COHERENT`. `aar_publish` and `aar_acquire` check that
the range lies inside one live block and add a memory barrier only, because
the Zorro master reaches the heap through the ACP. A byte count whose high
word is not zero is `AAR_E_INVAL`. `aar_get_info` reports `msg_capacity` 64
(the ring holds 64 messages), a 4096-byte `shared_granule`, `heap_granule` 1
MiB, `stall_limit_ms` 30000, `stop_grace_ms` 500, `sleep_granule_us` 5000,
`max_files` 16, and the flags `AAR_INFOF_ISOLATED` and
`AAR_INFOF_SHARED_COHERENT`.

**Extension table.** `aar_ext(AAR_PLATFORM_ZZ9000, AAR_EXT_ZZ9000_MAIN)`
returns the app's own copy of `aar_ext_zz9000`: `gl_bind_flags` (a veneer),
`build_id` (the firmware build string) and `gl_build_id`. `gl_build_id` is
always `NULL`, because the resident firmware has no source for the GL module's
revision; the module prints it on the UART when it loads (`[ZZM] MINIGL loaded
v1 build <id>`, see [minigl-module.md](minigl-module.md)). Any other platform
or extension id returns `NULL`.

## Slot protocol

The app service is `0x0c00`; its catalog version is `0x00030000`. Integers
in mailbox payloads and windows are big-endian. `ZZAPP_CAP_SLOTS` advertises
the extended request forms; the slot on the wire names a launcher window, and
`QUERY` reports the window count, 4 (`ZZAPP_SLOT_COUNT`). The count is a
platform ceiling on windows, not on apps (see
[Launcher windows](#launcher-windows)). `ZZAPP_CAP_LOW_MEMORY`,
`ZZAPP_CAP_LOAD_PROGRESS`, and `ZZAPP_CAP_HOST_LIMIT` are
append-only bits in the extended `QUERY` capability word; a client with
`ZZAPP_CAP_SLOTS` uses the extended request form for all operations after
allocation and carries the returned slot and generation.

An extended request is exactly 48 payload bytes. The selector is at byte 40
(`slot`) and byte 44 (`generation`). Existing fields retain their old offsets;
unused bytes are zero. A selector with an out-of-range slot or a generation
that no longer owns the slot fails with the stale ownership error and changes
nothing.

Extended `ALLOC_WINDOW` atomically claims the lowest free window, in the
admission order described under [Admission](#admission-and-refusal-reasons),
and returns the old 12-byte window reply followed by slot and generation at
bytes 12 and 16. A refusal returns a 4-byte reason instead.
A `CLAIMED` or `LOADING` slot has a 30-second lease. `LOAD_BEGIN`, every
completed `LOAD_CHUNK`, and each changed launcher heartbeat renew it. This
permits slow Zorro transfers and long asynchronous file reads while reclaiming
a launcher that has disappeared before or during a load.

Once the app runs, the launcher must change the slot heartbeat at least every
two seconds. A launcher that sets lifecycle flag bit 18
(`ZZAPP_F_LAUNCHER_HEARTBEAT`) gets ten seconds instead, so a briefly starved
Amiga task does not lose its app; it still renews only from its own main
loop, so a launcher that has died is still detected.

The client side of that lease is in `zz9k.library`: from revision 40 its
service task releases the slot of an owner task that is gone, see
[zz9k-library.md](../sdk/docs/zz9k-library.md#arm-app-client).

An expired load becomes `STOPPING` with terminal reason `QUARANTINED` (9).
Core 1 retires the app's resources (frames, tables, ASID, host-visible blocks,
staging), but its card window remains
reserved: a late `Read` from the old Amiga process must never write into a
replacement launcher's window. The record shell and its handle stay allocated
until the window is released, so a quarantined window holds one record
(`zzapp_services.c:1402-1452`). No allocator can choose a quarantined slot.
The old launcher releases that reservation by making its next stale load
request, which returns the stale-ownership error. A stale request arriving
while core 1 is still tearing down records that release intent and still
receives the terminal error; the window is released as soon as teardown
finishes. If it will never return, the user may send extended `RELEASE`
(`0x0c0a`) with the slot and current generation; it succeeds only after loader
teardown and only for a quarantined slot. Card reset clears all quarantine.

Extended `LOAD_BEGIN` carries the launcher memory limit in MiB at byte 0;
zero means no limit beyond the pool. `LOAD_CHUNK`, `LOAD_END`, `START`,
`STATUS`, and `DISPLAY_BIND` retain their old fields and append the selector.
The limit applies to the initial image, stack, and
minimum heap commit as well as later heap growth. `DISPLAY_BIND` always
carries the spare-buffer field at byte 28; zero means none. A second bind while
a surface is already bound returns `BUSY` and leaves the existing mappings alone.

Extended `LOAD_BEGIN` also carries the app's host-visible limit in MiB at
byte 4 (`ZZAPP_LOAD_HOST_VISIBLE_MIB`); firmware that advertises
`ZZAPP_CAP_HOST_LIMIT` reads it. Zero, and every request form that has no such
word, keeps the default of 16 MiB. A nonzero value replaces the default for
that app, so it can raise the limit or lower it, and `aar_get_info` reports the
value in force as `shared_limit`. A value above 4095 in either limit
word returns `BAD_REQUEST`. The limit is a cap, not a
reservation: nothing is set aside when the load begins, and an allocation can
still fail below the cap when the shared host-visible heap, which also serves
P96 surfaces and SDK buffers, or the app's alias range is full. Only a request
that could never be satisfied is refused. `LOAD_BEGIN` then returns
`SDK_STATUS_NO_MEMORY` (6) at once and leaves the claim as it was. That
ceiling is the smaller of the surface heap and one app's alias window of 62
MiB; the heap on the card is larger, so the window sets it, and a request of
up to 62 MiB is accepted. The limit lasts until the slot is claimed again.
Both limits (the memory cap at byte 0 and this one) take effect only when the
request is
admitted to the slot's queue and the slot is idle or claimed; a `LOAD_BEGIN`
answered `BUSY`, or sent after the load has begun, changes neither.

A normal exit does not tear the slot down on the app's own thread. The exit
call only records the exit and stops the app's thread, and does both in one
step with local interrupts masked: a tick between the two would either strand
the stopped thread with the exit unrecorded, or let the system thread start
the teardown while the app still runs. The thread then hands the CPU on. The
system thread runs the teardown (end-of-app services, GL reclaim, publishing
`IDLE`, then the record retire that returns the loader state and every other
resource), the same place faults, forced stops and watchdog stops are torn
down. That matters because a stopped app thread is never scheduled again, and
the retire unmaps every host-visible block, which holds the address-space lock
for longer than a scheduler tick when a block is tens of MiB. The tick is taken
as soon as the lock is released, before the loader state is marked unbound. On
the app's own thread that left the slot idle with its loader state still bound,
and every later `LOAD_BEGIN` answered bad request until the card reset. The
system thread is resumed after the tick, and other apps keep running while a
long teardown is in progress.

From the exit call until the teardown has finished, the exit counts as owed
state of the slot, separate from the app being running. Core 1 records the
exit first and then publishes `running = 0` with release semantics; core 0
loads `running` with acquire semantics before it reads the exit state, so
whoever sees the app stopped also sees the cleanup it owes. That ordering is
by construction; the host tests cannot show it.

An Amiga reset is posted to every live record and core 0 waits for core 1's
acknowledgement of each before it goes on to reset the surface allocator.
Core 1's reset path does the work for every slot: it tears down a running
app, a loading slot or an exit still owed, so the allocator is not reset
under a cleanup that still frees blocks from the old run, and it releases
the slot's window, idle slots included, freeing the block and dropping the
pointer. The window is therefore given back by one core, never by both at
once. Core 0 touches it only when there is no core 1 at all, or when core 1
is halted. If a record has not acknowledged by the shared deadline, core 1
may only be slow, and a slow worker would run its reset later, after the
allocator was reset and its addresses reused. Core 0 then does not clean up
beside it: it halts core 1 and recovers every slot while it is held. A slot
with a pending reset ends with the reason `AMIGA_RESET`, and the reset
itself is completed by the routine core 1's worker would have run, not by a
part of it: the window is retired whatever state the slot is in (an app
whose audio close is still owed is `STOPPING` when it is stopped, not idle),
deferred work queued for the slot, such as a posted `LOAD_BEGIN`, is
cancelled, the guard of a commit the halted core could not finish is
dropped, and the request is acknowledged. Core 1 is then released with
nothing left to do: the restarted worker finds no reset request, no window,
no deferred work and no running app, so it can run before the allocator is
reset without allocating or freeing anything. The next claim allocates a
window again at the slot's fixed range. A slot that kept its pointer would
free, at a later reset, whatever new owner holds that address.

The timeout has a cost. All live records share one deadline of one second plus
20 ms per live record, capped at four seconds (`zzapp_reset_deadline_us`,
`zzapp_host.c:2604-2622`), so a reset that core 1 cannot acknowledge waits at
most four seconds, and then the recovery is a cold restart of core 1: it also
aborts the SDK requests queued for it and resets the shared graphics, audio and
module state, not only the app slots. It is the price of not cleaning up beside
a worker that may still be running. Core 0 prints `[app] reset: core 1 did not
acknowledge pid N within M ms` for the first record that missed.

A core-1 restart treats a slot that owes a cleanup like a running app and
tears it down with the reason `CORE_RESTART`, replaying teardown steps that
may have run already. The allocator frees by address and cannot tell owners
apart, so every resource it holds is detached from its owner's record before
it is freed: host-visible blocks, slot windows and the audio ring. A halt
between the two leaks the block until the allocator is reset, instead of
freeing the block of whoever was given the address meanwhile. Loader staging
and app memory are freed through the card pool, which checks the owner. A
restart that finds a slot that is claimed or loading releases its resources,
publishes the host state as idle before it reopens the slot, and frees the
slot's window block together with the window, so the next claim can allocate
it again at the same fixed range.

Before asserting core 1's reset, core 0 asks it to park. A responsive worker
finishes its record and heap writes, cleans its own L1 data cache, completes
a DSB, and release-publishes the park acknowledgement. Core 0 acquires that
acknowledgement before resetting it. The worker then stays parked without
touching record or heap state. The shared L2 survives the reset, and core 1
invalidates its local caches before rejoining coherency.

A wedged worker may miss the bounded park wait. That reset remains best
effort, as before the record cutover: dirty L1 updates can be lost when reset
removes the core from SCU snooping. Idempotent retire stages, handle checks
and allocator reconciliation repair interrupted operations whose published
state survived; they cannot recover arbitrary lost stores. In particular, a
lost pointer detach can expose a block that was already freed. Recovery
skips a host-visible block when its address and size no longer match a live
surface allocation, but same-size address reuse can still refer to another
owner. This path does not guarantee recovery from that case.

A reset or restart ends the records but does not touch the record arena, the
handle table, or the handle generations: a handle from before the reset
stays stale, and the arena survives because its class is permanent.

A core-1 restart retires a faulted app the same way. The fault snapshot in the
life block stays readable, and a teardown that is still finishing does not set
it again. A successful claim of the window therefore goes straight to
`LOAD_BEGIN`: nothing refuses it for an earlier fault.

A rejected app request that carries a reason prints a `[app] reject op
0x<opcode>` line on the console. That covers a deferred `LOAD_BEGIN`,
`LOAD_CHUNK`, `LOAD_END` or `START` the loader refuses, a `DISPLAY_BIND` with
bad fields, a limit above 4095 MiB, a host-visible limit the card cannot
grant, and a deferred request with request id zero. Every such rejection
prints, but at most four lines a second; the next line printed is
`[app] N rejected ops held back`. A request that fails the slot or generation
check prints nothing.

`LIST` (`0x0c09`) is extended-only. The selector names the slot and ignores
its generation. Its 48-byte reply has state, generation, committed bytes,
limit, app CPU milliseconds, and kernel service milliseconds at bytes 0
through 23. CPU time starts at zero when a generation is claimed. STATUS CPU
time is user execution plus SVC execution; it excludes idle and recovery
intervals. Byte 24 is the block reason: `NONE` (0), `FILE` (1), `CONSOLE` (2),
`EVENT` (3, `aar_msg_wait`), `SLEEP` (4), `VSYNC` (5), or `AUDIO` (6).
Byte 25 is the terminal reason. Bytes 26-27 hold the overload flag, and bytes
28-47 are the app name. Terminal reasons are `NONE` (0), `EXIT` (1),
`FORCED` (2), `USER_FAULT` (3), `STALL` (4), `STUCK_CALL` (5), `LEASE` (6),
`AMIGA_RESET` (7), `CORE_RESTART` (8), and `QUARANTINED` (9).

### Incremental-load progress

Firmware advertising `ZZAPP_CAP_LOAD_PROGRESS` increments the big-endian
32-bit word at lifecycle offset `0x68` after every incomplete `LOAD_END`
step. `LOAD_BEGIN` resets it to zero. The word wraps modulo $2^{32}$ and only
denotes loader progress while the slot is `LOADING`; once the app runs, the
existing runtime progress timestamp again owns offsets `0x68` and `0x6C`.
`zz9k.library` revision 39 and later resets its normal five-second inactivity
deadline whenever this word changes. With older firmware, which does not
advertise the bit, it retains the fixed 32-second load-lease fallback.

### Low-memory crossings

Version 2's lifecycle block reserves the unused bytes after `terminal_reason`
at `0xA4`; `0xA8` is its first append-only word. Existing version-2 launchers
continue their exact-version check and ignore that unadvertised word. A launcher
that needs the counter first sends extended `QUERY` and requires its capability
bit.

```text
capability bit: ZZAPP_CAP_LOW_MEMORY = 0x00000040
launcher caps: extended QUERY reply, big-endian u32 at byte 12
counter: lifecycle window offset 0xA8, big-endian u32
semantics: zeroed when a slot generation is claimed; increments once per
free-pool low-water crossing while that slot is running; wraps modulo 2^32
```

The same crossing queues one `AAR_EV_LOW_MEMORY` event for the app; no
additional crossing is counted or queued until the allocator has re-armed
above its high-water mark.

### Legacy slot 0

A request whose payload is not 48 bytes is legacy. It addresses slot 0 only,
uses the original single-slot request and reply shapes byte-for-byte, and has
no selector.
Legacy `QUERY` reports slot 0 state; legacy `ALLOC_WINDOW` claims slot 0 only.
Therefore an unchanged single-slot ZZRun or `zz9k.library` client continues to
run one app without learning about the other slots. A client without
`ZZAPP_CAP_SLOTS` uses these legacy forms against older firmware.

### Admission and refusal reasons

An extended `ALLOC_WINDOW` reserves, in this order, and undoes each step in
reverse on a later refusal. Nothing is published to the launcher until the
last step, so a refused claim leaves the handle queue, the handle
generations, the L2 reserve and free counts, the window memory, and the
committed bytes as they were (`claim_window`, `zzapp_services.c:4832-4921`;
`zzapp_rec_unadmit`, `zzapp_rec.c:325-351`):

1. Platform and window. Zorro III with the window ranges mapped, then a free
   window: unbound, not quarantined, no deferred operation pending. Refusal
   reason `LIMIT`.
2. Record and handle (`zzapp_rec_admit`): the oldest free record index, with
   its stack, L1 table, and table pages. Refusal reason `MEMORY` when no
   record is free.
3. L2 reserve of 16 tables (`zzapp_as_reserve_l2`). Refusal reason `TABLES`
   when the reserved tables plus 16 would exceed the pool less the kernel
   template (`zzapp_as.c:939-953`).
4. Window memory. The first claim of a window takes its fixed range from the
   host-visible heap; a window kept from an earlier claim skips this step.
   Refusal reason `HOSTHEAP` when the range is held by another client or the
   heap cannot give it.
5. Publish: the wire generation is bumped, the lifecycle block and the claim
   lease are set, and the record becomes `LIVE`.

Loading continues the same order. `LOAD_BEGIN` takes a 16 MiB staging block of
class `APP` (`ZZAPP_FILE_MAX`); if the pool cannot give it the claim ends with
reason `MEMORY` (`zzapp_loader.c:995-1002`, `zzapp_services.c:4483-4487`). The
first `LOAD_END` step takes the commit, whether the pool or the launcher's
memory limit refuses it: reason `MEMORY` (`zzapp_loader.c:535-575`). Mapping
the image takes tables from the record's reserve; if the image needs more than
16, the reason is `TABLES` (`zzapp_loader.c:786-788`). A failed `LOAD_END`
ends the claim: the frames, tables, and staging go back and the window reads
`IDLE`. The staging block is released at the end of a successful `LOAD_END`,
not kept for the life of the app (`zzapp_loader.c:1657-1720`), so a running
app holds none of the 96 MiB class `APP` cap; only loads in progress do, up to
six at once.

What the launcher sees:

- The status of the `ALLOC_WINDOW` reply. `LIMIT` is `SDK_STATUS_BUSY`;
  `MEMORY`, `TABLES`, and `HOSTHEAP` are `SDK_STATUS_NO_MEMORY` (6). The
  statuses are what they were before records
  (`zzapp_services.c:5077-5086`). A load refusal keeps the load statuses
  described under [App interface](#app-interface).
- On a refused extended `ALLOC_WINDOW` the reply is 4 bytes: a big-endian
  `AAR_REASON_*` at `ZZAPP_ALLOC_REASON` (0): `MEMORY` 1, `TABLES` 2,
  `HOSTHEAP` 3, `LIMIT` 4 (`aar/app.h`, `zzapp_proto.h:160-165`). A legacy
  request gets the status only.
- The lifecycle word at `ZZAPP_LIFE_REFUSAL_OFF` (`0xAC`) holds the reason of
  the last resource refusal of the claim, set before the failing operation
  completes and cleared at claim. It is where a `LOAD_BEGIN` or `LOAD_END`
  refusal is visible (`zzapp_proto.h:337-340`,
  `zzapp_services.c:1352-1356,4913`). Older launchers never read it.
- The extended `QUERY` reply is 48 bytes (the legacy one stays 16). After the
  existing `SLOTS` (16), `FREE_SLOTS` (20), `COMMIT_FREE` (24), and
  `COMMIT_TOTAL` (28) words it carries `RECORDS_FREE` (32, free records),
  `TABLES_FREE` (36, L2 tables a new admission can still reserve),
  `TABLES_TOTAL` (40, the pool less the kernel template), and
  `HOSTHEAP_FREE` (44, free bytes of the host-visible heap)
  (`zzapp_proto.h:140-155`, `zzapp_services.c:4786-4827`). `COMMIT_FREE` and
  `COMMIT_TOTAL` are real: the bytes still admissible for app commits after
  the system reserve, and that plus the committed bytes. A client that decodes
  32 bytes keeps working. After a claim, load, and release cycle the free
  counts return to their starting values.

A launcher that only reads the status codes behaves as before; one that
wants the reason reads the 4-byte reply or the `0xAC` word.

### Launcher windows

Each launcher window is `ZZAPP_WINDOW_BYTES` (592 KiB) with the
unchanged layout. The fixed ranges are slot 0
`0x0805C000`–`0x080F0000`, slot 1 `0x07FC8000`–`0x0805C000`, slot 2
`0x07F34000`–`0x07FC8000`, and slot 3
`0x07EA0000`–`0x07F34000`. Slot 0 is the legacy range and ends at the fixed
Z3 host window. Each range is mapped non-cacheable before core 1 runs; its
window is allocated when the launcher claims that slot.

These four windows are the one remaining ceiling on concurrent apps. The
record capacity, the table pool, and the commit ledger allow far more apps,
but every claim needs a window, so a fifth claim is refused with reason
`LIMIT` while the other counts stay free. The ceiling lasts until the
transport swap (slice 6), when requests name handles and the fixed windows
go away. The value is `ZZAPP_SLOT_COUNT` in `zzapp_proto.h`, asserted against
the carved ranges in the firmware (`zzapp_proto.h:60-67`,
`zzapp_services.c:86`); host tests lift it with
`-DZZAPP_SLOT_COUNT=64u`.

## Translation attribute changes

The firmware links with `-Wl,--wrap=Xil_SetTlbAttributes`. Every BSP call,
including calls originating inside the BSP archive, enters
`__wrap_Xil_SetTlbAttributes` in `zzapp_as.c`. The wrapper calls the real BSP
function first, then copies changed descriptors into the address-space template
and every live app table through the normal break-before-make path. Each
affected section, large page, or small page receives the required cache,
per-address TLB, and branch-predictor maintenance; core 0's unrelated
translations remain valid.
