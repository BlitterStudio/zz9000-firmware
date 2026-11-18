# ZZ9000 app runtime

This is the firmware and launcher contract for the API 0.4 app kernel. The
public app interface is [zzapp-api.md](../sdk/docs/zzapp-api.md); application
build and usage guidance is in
[zzapp-developer-guide.md](../sdk/docs/zzapp-developer-guide.md). The shared
wire definitions in `ZZ9000_proto.sdk/ZZ9000OS/src/zzapp_proto.h` are the
source of truth for byte offsets and constants.

## Kernel architecture

Core 1 runs a small kernel. It has one highest-priority system thread and up
to four app threads. An app process owns its address space, memory commit,
resource ledger, launcher slot, and generation. Its thread owns the saved
user context, kernel stack, scheduler state, and CPU accounting. There is one
app thread per process today.

An exception frame is the saved context. It includes the interrupted user
registers, SYS `sp` and `lr`, SPSR, VFP registers, and FPSCR. A new app starts
from a synthetic frame with the ELF entry point, API table, `argc`, `argv`,
and user stack. The exception return restores the selected frame without a
nested call into app code.

### Scheduling and doorbell

The core-1 private timer gives apps a 5 ms quantum. The scheduler first runs
due system work: SDK tasks, MiniGL service, launcher work, and audio feeding.
It then selects runnable app threads round-robin. In an overload window,
service work is limited so runnable apps retain a 5% floor.
Deferred launcher operations are selected round-robin across slots. Each
service pass advances at most one slot by one bounded step, preserving
per-slot request order without letting a long `LOAD_END` starve another
slot's `LOAD_BEGIN`, `LOAD_CHUNK`, or `START`.

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
explicit `zz_yield` safe point releases that lock, lets one due system-service
pass run, then reacquires the lock before returning to the body. A standalone
`zz_yield` rotates to another runnable app and runs a system-service pass only
when service work is due. This bounds audio-service delay at cooperative yield
points to the current API segment plus one scheduler handoff; a body that does
not yield remains subject to the API watchdog rather than a scheduler preemption.

File and directory operations, console read and backpressure, display open and
flip, non-host audio open and close, `zz_msg_wait`, and sleep park the caller
at the SVC handoff. `zz_gl_swap` parks through its flip. Completion wakes the
slot READY; file and directory waits wake TIMEOUT after 30 seconds and return
`ZZ_ETIMEDOUT`. Stop wakes a parked waiter (an EVENT waiter wakes READY and
then sees the stop); waiting service calls return `ZZ_EIO`. `zz_input_poll`
and `zz_display_vsync` do not park; the
latter only yields once.

`zz_msg_wait` parks as an `EVENT` wait with the caller's timeout as its
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

Each slot has a private 16 KiB L1 table derived from a privileged kernel
template and a fixed ASID. The private virtual window is
`0xC0000000`–`0xDFFFFFFF`. Image, heap, and stack are mapped there as
non-global sections, 64 KiB large pages, or 4 KiB small pages as appropriate.
The app sees only its private pages plus grants for its launcher window,
host-visible blocks, and owned display surfaces. The rest of DDR, firmware,
devices, and other apps remain privileged-only.

A host-visible block uses the same mapping as the rest of DDR, Normal
write-back shareable (descriptor `0x15de6`). Zorro III accesses reach DDR
through the Zynq ACP (`m00_axi` of the Zorro master, ARCACHE `0xF` for reads
and AWCACHE `0x3` for writes). The ACP snoops the ARM L1 caches, so neither
side needs ARM cache maintenance. The 68k's own caches are outside the ACP:
a 68k client reads through a cache-inhibited mapping, as `zz9k.library` does
for the launcher window, or maintains its caches itself. The Zorro
master's second port, `m01_axi`, goes to HP1 but only writes video-capture
frames; it carries no Zorro access to the host-visible heap.

The app-facing API and GL tables retain their layout and `size` semantics, but
their entries point to user-RX `svc; bx lr` veneers. On a user veneer call,
the vector copies the exception frame to that slot's kernel stack, switches to
SYS mode, and enables IRQs before invoking the handler. Every SVC handler
checks user pointers against the caller's mappings and permissions before
touching them. Invalid, overflowed, read-only-for-output, device, and
other-slot pointers return `ZZ_EFAULT`. Firmware code never becomes a
user-call target.

GL client-array state is per slot. `DrawArrays` validates every enabled vertex,
colour, and client texture-coordinate element in its requested range.
`DrawElements` also validates its index buffer and every indexed element before
MiniGL is called. An unmapped or changed range returns `ZZ_EFAULT` and records
`GL_INVALID_VALUE` for `glGetError`.

### Frames, commit, and teardown

Private memory is allocated as zeroed 1 MiB frames from aligned 8 MiB
card-pool extents. Admission is atomic: aggregate commit plus the request
must fit in pool free memory after the system reserve and any per-app launcher
limit. API 0.1 and 0.2 images retain their 96 MiB load-time commit. API 0.3
images commit image, stack, and their requested minimum heap; `heap_grow` and
`heap_shrink` add or return whole frames while keeping the heap VA range
fixed.

All terminal paths call the same generation-guarded teardown: stop new calls,
wake and cancel waits, quiesce work and DMA holding app pointers, hand off or
unbind display, detach audio, release GL and decoder sessions, unmap grants,
return frames and host-visible blocks, release the slot, and publish the
firmware-latched end reason, or `EXIT` when no reason was latched. A slot
remains `STOPPING` until audio teardown is acknowledged and any running deferred
load cancellation completes; only then can it become reusable. `LIST` retains
that reason after the slot returns to idle.
The reasons are none, normal exit, forced stop, user fault, stall watchdog,
stuck API call, lease loss, Amiga reset, and core-1 restart. Only user-mode
faults stop a single app. A fault in firmware code restarts core 1 and ends
every app. Per-slot 30-second stall and 10-second stuck-call watchdogs stop
only their slot. A separate core-0 backstop cold-restarts core 1 after two
seconds without both progress and timer ticks; that ends every non-idle slot
with the restart reason.

## Slot protocol

The app service is `0x0c00`; its catalog version is `0x00030000`. Integers
in mailbox payloads and windows are big-endian. `ZZAPP_CAP_SLOTS` advertises
four slots. `ZZAPP_CAP_LOW_MEMORY`, `ZZAPP_CAP_LOAD_PROGRESS`, and
`ZZAPP_CAP_HOST_LIMIT` are
append-only bits in the extended `QUERY` capability word; a client with
`ZZAPP_CAP_SLOTS` uses the extended request form for all operations after
allocation and carries the returned slot and generation.

An extended request is exactly 48 payload bytes. The selector is at byte 40
(`slot`) and byte 44 (`generation`). Existing fields retain their old offsets;
unused bytes are zero. A selector with an out-of-range slot or a generation
that no longer owns the slot fails with the stale ownership error and changes
nothing.

Extended `ALLOC_WINDOW` atomically claims the lowest free slot and returns the
old 12-byte window reply followed by slot and generation at bytes 12 and 16.
A `CLAIMED` or `LOADING` slot has a 30-second lease. `LOAD_BEGIN`, every
completed `LOAD_CHUNK`, and each changed launcher heartbeat renew it. This
permits slow Zorro transfers and long asynchronous file reads while reclaiming
a launcher that has disappeared before or during a load.

Once the app runs, the launcher must change the slot heartbeat at least every
two seconds. A launcher that sets lifecycle flag bit 18
(`ZZAPP_F_LAUNCHER_HEARTBEAT`) gets ten seconds instead, so a briefly starved
Amiga task does not lose its app; it still renews only from its own main
loop, so a launcher that has died is still detected.

An expired load becomes `STOPPING` with terminal reason `QUARANTINED` (9).
Core 1 aborts the loader and frees the app memory, but its card window remains
reserved: a late `Read` from the old Amiga process must never write into a
replacement launcher's window. No allocator can choose a quarantined slot.
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
For API 0.3 images, the limit applies to the initial image, stack, and
minimum heap commit as well as later heap growth. `DISPLAY_BIND` always
carries the spare-buffer field at byte 28; zero means none. A second bind while
a surface is already bound returns `BUSY` and leaves the existing mappings alone.

Extended `LOAD_BEGIN` also carries the app's host-visible limit in MiB at
byte 4 (`ZZAPP_LOAD_HOST_VISIBLE_MIB`); firmware that advertises
`ZZAPP_CAP_HOST_LIMIT` reads it. Zero, and every request form that has no such
word, keeps the default of 16 MiB. A nonzero value replaces the default for
that app, so it can raise the limit or lower it, and `zz_mem_info` reports the
value in force as `host_visible_limit`. The limit is a cap, not a
reservation: nothing is set aside when the load begins, and an allocation can
still fail below the cap when the shared host-visible heap, which also serves
P96 surfaces and SDK buffers, or the app's alias range is full. Only a request
that could never be satisfied is refused. `LOAD_BEGIN` then returns
`SDK_STATUS_NO_MEMORY` (6) at once and leaves the claim as it was. That
ceiling is the smaller of the surface heap and one app's alias window of 62
MiB, so one block of 34 MiB maps as a single alias and a request of up to 62
MiB is accepted. The limit lasts until the slot is claimed again. Both limits
(the memory cap at byte 0 and this one) take effect only when the request is
admitted to the slot's queue and the slot is idle or claimed; a `LOAD_BEGIN`
answered `BUSY`, or sent after the load has begun, changes neither.

A normal exit does not tear the slot down on the app's own thread. The exit
call only records the exit and stops the app's thread, and does both in one
step with local interrupts masked: a tick between the two would either strand
the stopped thread with the exit unrecorded, or let the system thread start
the teardown while the app still runs. The thread then hands the CPU on. The
system thread runs the teardown (end-of-app services, GL reclaim, publishing
`IDLE`, dropping the loader arena), the same place faults, forced stops and
watchdog stops are torn down. That matters because a stopped app thread is
never scheduled again, and the arena drop unmaps every host-visible block,
which holds the address-space lock for longer than a scheduler tick when a
block is tens of MiB. The tick is taken as soon as the lock is released,
before the arena is marked unbound. On the app's own thread that left the slot
idle with its arena still bound, and every later `LOAD_BEGIN` answered bad
request until the card reset. The system thread is resumed after the tick,
and other apps keep running while a long teardown is in progress.

From the exit call until the teardown has finished, the exit counts as owed
state of the slot, separate from the app being running. Core 1 records the
exit first and then publishes `running = 0` with release semantics; core 0
loads `running` with acquire semantics before it reads the exit state, so
whoever sees the app stopped also sees the cleanup it owes. That ordering is
by construction; the host tests cannot show it.

An Amiga reset is posted to every slot and core 0 waits for core 1's
acknowledgement of each before it goes on to reset the surface allocator.
Core 1's reset path does the work for every slot: it tears down a running
app, a loading slot or an exit still owed, so the allocator is not reset
under a cleanup that still frees blocks from the old run, and it releases
the slot's window, idle slots included, freeing the block and dropping the
pointer. The window is therefore given back by one core, never by both at
once. Core 0 touches it only when there is no core 1 at all, or when core 1
is halted. If a slot's acknowledgement has not come within one second, core 1
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

The timeout has a cost. Each slot is given up to one second, so a reset that
core 1 cannot acknowledge waits up to four seconds, and then the recovery is
a cold restart of core 1: it also aborts the SDK requests queued for it and
resets the shared graphics, audio and module state, not only the app slots.
It is the price of not cleaning up beside a worker that may still be running.

A core-1 restart treats a slot that owes a cleanup like a running app and
tears it down with the reason `CORE_RESTART`, replaying teardown steps that
may have run already. The allocator frees by address and cannot tell owners
apart, so every resource it holds is detached from its owner's record before
it is freed: host-visible blocks, slot windows and the audio ring. A halt
between the two leaks the block until the allocator is reset, instead of
freeing the block of whoever was given the address meanwhile. Loader arena
and app memory are freed through the card pool, which checks the owner. A
restart that finds a slot that is claimed or loading releases its arena,
publishes the host state as idle before it reopens the slot, and frees the
slot's window block together with the window, so the next claim can allocate
it again at the same fixed range.

A fault or restart leaves the slot's loader state `FAULTED` until `STATUS`
observes it or the latch expires, and `LOAD_BEGIN` is refused meanwhile.
A successful claim of the slot releases the latch, because a launcher that
claims again has moved on and goes straight from claim to `LOAD_BEGIN`. The
fault snapshot in the life block stays readable, and a teardown that is
still finishing does not set the latch again.

Every rejected app operation prints a `[app] reject op` line on the console,
at most four a second; the next line reports how many were held back.

`LIST` (`0x0c09`) is extended-only. The selector names the slot and ignores
its generation. Its 48-byte reply has state, generation, committed bytes,
limit, app CPU milliseconds, and kernel service milliseconds at bytes 0
through 23. CPU time starts at zero when a generation is claimed. STATUS CPU
time is user execution plus SVC execution; it excludes idle and recovery
intervals. Byte 24 is the block reason: `NONE` (0), `FILE` (1), `CONSOLE` (2),
`EVENT` (3, `zz_msg_wait`), `SLEEP` (4), `VSYNC` (5), or `AUDIO` (6).
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
Revision 39 of `zz9k.library` resets its normal five-second inactivity
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

The same crossing queues one `ZZ_EV_LOW_MEMORY` event for the app; no
additional crossing is counted or queued until the allocator has re-armed
above its high-water mark.

### Legacy slot 0

A request whose payload is not 48 bytes is legacy. It addresses slot 0 only,
uses the API 0.2 request and reply shapes byte-for-byte, and has no selector.
Legacy `QUERY` reports slot 0 state; legacy `ALLOC_WINDOW` claims slot 0 only.
Therefore an unchanged API 0.2 ZZRun or `zz9k.library` client continues to
run one app without learning about the other slots. A client without
`ZZAPP_CAP_SLOTS` uses these legacy forms against older firmware.

### Launcher windows

Each slot gets a `ZZAPP_WINDOW_BYTES` (592 KiB) launcher window with the
unchanged layout. The fixed ranges are slot 0
`0x0805C000`–`0x080F0000`, slot 1 `0x07FC8000`–`0x0805C000`, slot 2
`0x07F34000`–`0x07FC8000`, and slot 3
`0x07EA0000`–`0x07F34000`. Slot 0 is the legacy range and ends at the fixed
Z3 host window. Each range is mapped non-cacheable before core 1 runs; its
window is allocated when the launcher claims that slot.

## Translation attribute changes

The firmware links with `-Wl,--wrap=Xil_SetTlbAttributes`. Every BSP call,
including calls originating inside the BSP archive, enters
`__wrap_Xil_SetTlbAttributes` in `zzapp_as.c`. The wrapper calls the real BSP
function first, then copies changed descriptors into the address-space template
and every live app table through the normal break-before-make path. Each
affected section, large page, or small page receives the required cache,
per-address TLB, and branch-predictor maintenance; core 0's unrelated
translations remain valid.
