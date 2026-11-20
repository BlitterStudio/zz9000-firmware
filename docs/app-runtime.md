# ZZ9000 app runtime

This is the firmware and client contract for the app kernel, which
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
address space, memory commit, resource ledger, transport pair, and handle.
Its thread owns the saved user context, kernel stack, scheduler state,
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
from an earlier life of the index fails the lookup. The record keeps its
handle until `FREE` is published, so a halt between the generation store and
the state store is replayed from the handle alone: the free finishes whatever
store it stopped after, and a handle of another life never matches
(`zzapp_rec.c:382-425`). Free indices wait in a FIFO queue, so an index is
reused as late as possible (`zzapp_rec.c:297-323`). The table holds at least 8
and at most 1024 records (`ZZAPP_REC_MIN_APPS`, `ZZAPP_REC_MAX_APPS`,
`zzapp_rec.h:42-43`).

A record is one 16 KiB slot (`ZZAPP_REC_RECORD_BYTES`) holding the kernel
thread, the words core 0 polls, the per-app scheduler words, the claim
words (limits, load progress, terminal and refusal reasons, the client
capability word), the process memory ledger and its address space, the host
state, the 64-entry host-visible block table, the services, loader, SVC, and
decode state, and the transport part: the blocks of the claim, the two queue
views, the request table, the message inbox, the event ring, the doorbell
word, and the counters (`zzapp_rec.h`). Its states are `CLAIMED`, `LIVE`,
`RETIRING`, `RECLAIMED` (resources returned, shell and transport held until
the client releases the claim), and `FREE`; they only move forward until the
free (`zzapp_rec.h`).

A pass that walks the record indices, the core 1 worker pass and the core 0
poll, reads each record's state without a lock, and the other core may free
the record at any point of the walk: core 0 frees a claim that `RELEASE`
ends, core 1 frees the shell of a released app. The walk therefore selects
the record as its core's service context and takes the answer of
`zzapp_services_select_pid`: 1 when the record is selected, 0 when it was
freed since the state read (the context is cleared). A walk skips a record
it could not select. Going on with no selected record is not allowed: the
host code that works on "the selected record" treats an unselected context as
a bug and stops the core on an undefined instruction. The record's bytes stay
readable after the free, so a record freed after a successful select is
harmless to the rest of that visit.

Every mailbox request except `QUERY`, `CLAIM` and `LIST` names its record by
handle at byte 40 of the payload, and the lookup (`zzapp_rec_by_handle`)
fails for a stale one. The wire has no slot and no window; see
[Transport and claim protocol](#transport-and-claim-protocol).

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
- Per record: a 16 KiB L1 table, a 32 KiB core-1 stack, the 16 KiB record,
  and 8 KiB of user table pages (`zzapp_rec.h`), 72 KiB in all, plus the
  16 L2 tables it is guaranteed.
- L2 pool: 16 tables for the kernel template, 16 per record, and a shared
  remainder of 2 tables per MiB of host-visible heap, rounded up to a multiple
  of 32. A table is 1 KiB plus 6 bytes of bookkeeping (`zzapp_as.h:37-54`).
- The total is rounded up to 1 MiB. If the block does not fit, the carve
  retries with half the records down to 8; if even 8 do not fit, no app can
  run and the boot log says `record arena: no memory, apps unavailable`. The
  scheduler then starts with no records (`zzapp_kernel_init_system_only`): the
  system thread still takes the kernel lock, so the shared core-1 worker keeps
  serving the SDK, media and MiniGL, and every admission is refused `MEMORY`.

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
It then selects the runnable app thread with the smallest virtual run time,
described below. In an overload window, service work is limited so runnable
apps retain a 5% floor.
Deferred client operations (`LOAD_BEGIN`, `LOAD_CHUNK`, `LOAD_END`, `START`)
are selected round-robin across records. Each service pass advances at most
one record by one bounded step, preserving per-record request order without
letting a long `LOAD_END` starve another record's `LOAD_BEGIN`, `LOAD_CHUNK`,
or `START`. The scheduler walks a dense run list of the
records whose thread is not stopped, not the record capacity
(`zzapp_kernel.h:210-220`).

Apps share the core in proportion to their weights. Every thread keeps a
virtual run time. Each interval an app runs adds its run time in microseconds,
scaled by a fixed factor and divided by the app's weight, so a weight-3 app
advances a third as fast as a weight-1 app and runs three times as long. The
runnable app with the smallest value runs next. Apps with equal values take
turns in pid order. The system thread, the service floor, the API lock
handoff and the parked and blocked states are unchanged. The
scheduler reads the microsecond global timer at each switch
(`zzapp_kernel_now_us`), so run time under a millisecond is charged too;
the tick counters in `LIST` still count in milliseconds.

The running app keeps the core for its 5 ms quantum, with one exception: when
a runnable app lags it by more than the wake granularity
(`ZZAPP_KERNEL_WAKE_GRAN_US`, 1 ms of the lagging app's own run time at its
weight), the next pick takes that app at once. An app that blocks on its
client several times a frame is woken by a service pass, and the pass hands
the core back through the same pick, so the app runs as soon as its wake is
served and not after the rest of the quantum of a CPU-bound app. The rule is
in `choose_app`, after the system thread, the service floor and the API lock
owner and handoff picks. CPU-bound apps lag each other only by what one of
them ran since the last pick, so they change places about once per
millisecond of run time at most. A sleeper is woken when a switch finds its
deadline due, at the next tick (`ZZAPP_TICK_US`, 5 ms) at the latest.

The other rules keep one app from taking more than its share. An app that
starts begins at the smallest virtual run time among the runnable apps, or at
the floor the scheduler kept (a value that never decreases) when none is
runnable, so it neither waits for the others to catch up nor makes up for the
time before it existed. An app that wakes from a block or a sleep takes the
larger of its own value and the floor less one weight-1 quantum of run time
(`ZZAPP_KERNEL_WAKE_CREDIT`, 5 ms of virtual run time, not divided by the
app's weight), so time spent parked is not saved up beyond that. A weight-w
app therefore comes back up to w quanta of its own run time behind the floor
and may run that long before a weight-1 neighbour is ahead of it: a weight-8
app runs 40 ms and a weight-256 app 1.28 s, which is its share at work. The
first weight-1 app at the floor gets the core back within w * 5 ms plus a
quantum, and one that was a quantum ahead within twice that. A credit of one
quantum at the app's own weight is smaller than a weight-1 neighbour's
quantum: an interactive weight-8 app that blocks for 0.1 ms next to a
spinner then handed over a whole quantum and lost the 5 ms it ran ahead at
the next wake, and got 67 % of the core against an 8/9 share in the model.
A yield puts the app behind a runnable peer for the next pick, even if the
app's value is the smallest, when the peer is entitled to the core: its
value is not above the yielder's by more than the wake granularity at the
peer's weight (the scale of the lag rule). With no such peer the yield does
nothing and the app keeps the core and its quantum, so an app that yields
often is not made to hand a quantum to a neighbour that is far ahead. In the
model a weight-8 app that yields every 1.5 ms next to a weight-1 spin kept
60 % of the core with the old rule and keeps 8/9 with this one. The kernel
records the one yielder, and the first pick that gives the core to an app
spends the yield, whichever context made the pick: a timer interrupt that
puts the system thread between the yield and the voluntary dispatch does not
leave the flag set. The active app does not keep its quantum against a
pending yield. A pick scans the run list once, so its cost grows with the
number of records in it.
The weight comes from the client: `AAR_CB_WEIGHT`, read at `LOAD_BEGIN`.

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
flip, non-host audio open and close, `aar_msg_wait`, and sleep park
the caller at the SVC handoff. `aar_gl_swap` parks through its flip.
Completion wakes the record READY; file and directory waits wake TIMEOUT after
30 seconds and return `AAR_E_TIMEDOUT`. Stop wakes a parked waiter (an EVENT
waiter wakes READY and then sees the stop); waiting service calls return
`AAR_E_IO`. `aar_event_poll` and `aar_display_vsync` do not park; the
latter only yields once.

A service the app asks of its client is a request on the SQ. A record has
four request table entries: a blocking one (file, list, console read,
display open and close, audio open and close), the flip, the console output
stream, and the palette. Request ids come from a per-record counter, never zero and
never reused for the life of the record, so a late completion of a request
that timed out matches nothing and is dropped and counted. A request that
finds the SQ full parks the app until the client has made room; requests
never fail on a full SQ, messages do (`AAR_E_BUSY`), and log records are
dropped and counted in `AAR_CB_LOG_DROPPED`. The wait tests the request table
entry after a pump of the CQ, which consumes completions, events and messages
in order, so a completion is never stuck behind a message: the client posts a
message only while it holds a credit (`posted - AAR_CB_MSG_CONSUMED` below the
63-message capacity), and the firmware advances `AAR_CB_MSG_CONSUMED` when the
app takes a message out of its inbox.

`aar_display_set_palette` does not park. The call writes the colours into the
shadow of the service block's palette area, marks the entries dirty and
returns. At most one `AAR_OP_DISPLAY_PALETTE` request is outstanding. It
names the stage, a copy of one run of dirty entries that is not written again
until the client has answered. When it is answered the next run goes out, so
updates made meanwhile collapse to the latest colours, and an entry nobody
wrote is never sent. A full SQ leaves the entries dirty for the next pass of
the system thread. Closing the display drops the entries still waiting; a
request already in flight stays outstanding, because the client reads its
stage in order with the close. The end of the app gives the request up and
its late answer is dropped by id. A client that never answers only costs that
claim its palette updates.

`aar_msg_wait` parks as an `EVENT` wait with the caller's timeout as its
deadline. The client posts the message on the CQ and may ring `KICK`, but
the firmware also polls, so the wait is a level test: the service pass pumps
the CQ of every claim with a parked `EVENT` waiter and wakes it when the
inbox is not empty. The waiter reads the inbox again after every resume and
before it parks, so a message that lands in that gap is found by the next
service pass. A queued message ends the wait before a pending stop does. Like
every declared wait it keeps the record out of the 30-second stall and
10-second stuck-call watchdogs. Input events go into a 64-entry event ring of
the record when the pump runs; the ring drops the newest event and counts it
when full, and the client counts events it could not post in
`AAR_CB_EVENTS_DROPPED`. The runtime keeps at least 64 events.

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

With the counters on, an app's end also prints one `[app] sched slot=N` line
next to its `svc` line (`zzapp_kernel_sched_report`). It shows where the app's
time went since `START`, from `zzapp_kernel_now_us`. Times are milliseconds
with three decimals, and every microsecond of the app's life is in exactly one
of `run`, `wait_sys`, `wait_app` or a `park_*` total.

- `run`: on the core. `wait_sys`: runnable while the system thread had the
  core (service passes). `wait_app`: runnable while another app had it.
  `picks`: times a dispatch gave the app the core.
- `preempt=sys/lag/quantum/other`: times the app was runnable and lost the
  core. `sys` counts the switches to the system thread. `lag`, `quantum` and
  `other` count the pick that then gave the core to another app, by its
  reason: a lagging (woken) app under the wake rule, the smallest virtual run
  time after the quantum ended or the app switched voluntarily, and the API
  lock owner or handoff rules. A wake preemption goes through the system
  thread, so it adds one to `sys` and one to `lag`.
- `wakes`, and `clamps=N/ms`: the wakes whose clamp raised the app's virtual
  run time, and the run time at the app's weight those clamps discarded.
- `defer`: the number of times service was due and its window budget
  (`ZZAPP_KERNEL_SERVICE_LIMIT_MS`) was spent while apps were runnable, and
  this app was the one given the core instead.
- `why=sticky/lag/min/owner/handoff`: the dispatch decisions that gave the
  app the core, by reason, counting the app that keeps the core. `sticky` is
  the active app keeping its quantum, `lag` a lagging thread taking the core
  from it, `min` the smallest virtual run time when no app is active, `owner`
  the API lock owner finishing its call and `handoff` the lock passed to a
  waiter. `time_us` and `should_stop` take no API lock, so an app that only
  polls them never counts in `owner`.
- `yields=R/G/S`: yields the app made (R), the ones a peer was entitled to
  and that put the app behind it for the next pick (G), and the picks that
  passed the app over for a yield while its virtual run time was the smallest
  (S). `hold=N`: picks that kept the active app although this one was behind
  it by less than the wake granularity.
- `weight=W vr=ms floor=ms`: the weight the scheduler used, the app's virtual
  run time at its end and the floor, in run-time milliseconds at weight 1.
  Two apps that share fairly end close together whatever their weights; a
  weight that is not the one asked for shows here.
- `park_<cause>=ms/count`: time parked and the number of parks, for each cause
  that occurred. The causes are `lock` (a block with no wait registered,
  which is the wait for the API lock), the declared waits `file`, `conwr`,
  `conrd`, `dispopen`, `flip`, `audio`, `event`, `sqspace` and `audioreq`,
  and `sleep`.

The counters use two running totals of the core's time, in system and in app
context, and a thread's wait is the difference of the totals when it began
and ended, so a switch does no division and walks nothing.

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
appropriate. The app sees only its private pages plus grants for its
host-visible blocks and owned display surfaces. The rest of DDR,
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
for the transport buffers, or maintains its caches itself. The Zorro
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
must fit in pool free memory after the system reserve and any per-app client
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
record, so a restarted core 1 resumes after it (`zzapp_host.c`,
`zzapp_rec.h`): detach the thread from the scheduler, return the
host-visible blocks, grants, staging, and back buffer, destroy the address
space (alias extents, L2 tables, L1, ASID) and return the L2 reserve, return
the frames to the commit ledger, and finally publish the end reason in the
control block. The record then becomes `RECLAIMED` and keeps its
shell, its handle and its transport blocks until the client releases the
claim (`RELEASE`), which frees the record with a new handle generation. It
runs on core 1, or on core 0 while core 1 is halted. The end reason is the
firmware-latched one, or `EXIT` when none was latched. A claim remains
`STOPPING` until audio teardown is acknowledged and any running deferred load
cancellation completes. An ordinary exit also keeps `STOPPING` until the
client has completed all console output, including bytes still waiting in
the firmware's coalescing ring. The app's frames and tables have already
been returned. Only then does the state read `IDLE`, so a launcher that sees
it can release the claim without losing final output. An explicit `RELEASE`
can discard pending output without waiting for the client. `LIST` and the
control block retain the reason after the app ended. The reasons are none,
normal exit, forced stop, user fault,
stall watchdog, stuck API call, lease loss, Amiga reset, core-1 restart and
quarantined.

A fault retires the app at once. The state reads `IDLE` once the retire is
done, with the end reason `USER_FAULT`; there is no `FAULTED` latch to wait
out. The fault record (class, syndrome, `pstate`, `pc`, `sp`, `lr` and the
fault address) stays in the control block, and `LIST` keeps returning the end
reason, until the client releases the claim.

Only user-mode faults stop a single app. A fault in firmware code restarts
core 1 and ends every app. Per-app 30-second stall and 10-second stuck-call
watchdogs stop only their app. A separate core-0 backstop cold-restarts core 1
after two seconds without both progress and timer ticks; that ends every
live record with the restart reason.

## App interface

The firmware implements the AAR 0.5 app table (`aar_api`, `aar/app.h`), the GL
table (`aar_gl`, `aar/gl.h`) and the ZZ9000 extension table
(`aar_ext_zz9000`, `aar/ext_zz9000.h`) on the process records and the
transport described below. Calls are `aar_*`, constants `AAR_*`, status
codes `AAR_E_*`. The `zz_*`, `ZZ_*` and `zzapp_*` names of the 0.4 header are
gone with no compatibility layer; `docs/zzapp-to-aar.md` in the aar repository
maps them. The firmware's hardware names (`ZZ_*` audio, Ethernet, RasterIX and
MiniGL registers) are unchanged. `QUERY` reports 0.5 in its ABI major and
minor words (`ZZAPP_ABI_MAJOR`, `ZZAPP_ABI_MINOR`).

**Groups.** `aar_api.groups` and `aar_info.groups` report the same mask:
`AAR_GROUP_INPUT` and `AAR_GROUP_SERVICES` always, and `AAR_GROUP_DISPLAY`,
`AAR_GROUP_AUDIO`, `AAR_GROUP_GL` when the client capability word has the
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
status the client sees maps `AAR_E_INVAL` and `AAR_E_PROFILE` to
`BAD_REQUEST`, `AAR_E_NOGROUP` to `UNSUPPORTED`, and `AAR_E_NOMEM` to
`NO_MEMORY`; a profile or group refusal prints its reason in the `[app] reject
op` line. The groups a note names (required and optional) set the display,
audio and GL bits of `AAR_CB_FLAGS`, which the client reads. An image
without a v1 note, including one that carries a 0.4 note, does not load.

**Events.** `aar_event_poll` always delivers `AAR_EV_STOP` and
`AAR_EV_LOW_MEMORY`, and delivers the client's key, mouse, joystick and
break events only when the runtime has `AAR_GROUP_INPUT`. One stop request
yields one stop event: the firmware queues it itself, and the client's own
stop event for the same request counts as that delivery, so the second is
dropped. The low-memory event carries no fields other than its type.

**Files.** The open flags travel unchanged from the app to the client, in
the request's `AAR_A_FILE_OPEN_FLAGS` argument: `AAR_FILE_READ` 1, `WRITE` 2,
`CREATE` 4 (create if missing, keep an existing file), `TRUNCATE` 8, `EXCL` 16
(with `CREATE`, fail when the file exists). The firmware refuses an invalid
combination with `AAR_E_INVAL` before it reaches the client: an unknown bit,
neither `READ` nor `WRITE`, `CREATE` or `TRUNCATE` without `WRITE`, or `EXCL`
without `CREATE`. Create-or-replace is `CREATE | TRUNCATE`. Paths and names
travel as NUL-terminated bytes in the 1 KiB scratch area of the service block.
Reads and writes move through the data block, one request per chunk of at
most 256 KiB.

**Shared buffers.** `aar_shared_alloc` takes flags 0 and returns a block from
the host-visible heap, rounded to the 4096-byte granule, with at most 64 live
blocks per app and the `shared_limit` described under `LOAD_BEGIN`. The
result's `haddr` is the board-relative Zorro III offset of the block (the card
address minus the board's window base), `ptr` is the app mapping, and `flags`
always has `AAR_SHAREDF_COHERENT`. `aar_publish` and `aar_acquire` check that
the range lies inside one live block and add a memory barrier only, because
the Zorro master reaches the heap through the ACP. A byte count whose high
word is not zero is `AAR_E_INVAL`. `aar_get_info` reports `msg_capacity` 63
(the inbox holds 63 messages), a 4096-byte `shared_granule`, `heap_granule` 1
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

## Transport and claim protocol

The app service is `0x0c00`; its catalog version is `0x00040000`. A client
reaches an app through one claim. A claim is one process record and one
transport pair in host-visible memory: a control block and two rings, the
submission queue (SQ, app to client) and the completion queue (CQ, client to
app), defined by `aar/transport.h` in the aar repository and described in its
`docs/transport.md`. The mailbox carries only the private wire of
`zzapp_proto.h`: claim, load, start, stop, list, release and the doorbell.
Everything an app asks of its client (files, console, log, display, audio,
messages) and everything the client sends (input events, messages, replies)
travels in the rings. There is no launcher window, no slot and no fixed
address range. The Amiga client is `aar.library` (its ZZ9000 platform layer is
`sdk/amiga/src/aar_zz9000.[ch]`, described in `sdk/docs/zz9k-library.md`).

Integers in mailbox payloads are big-endian. A request is 48 payload bytes
except `QUERY`, `CLAIM` and `LIST`, which take no handle. Every other
operation names its claim by handle at byte 40 (bytes 44 to 47 are reserved
and zero). A handle that is not the live one of a record that owns a pair
(handle 0, generation 0, an index out of range, a free record, a generation
that moved on) gets `ZZAPP_ERR_STALE` (5, `SDK_STATUS_BAD_HANDLE`) and
changes nothing.

| Operation | Code | Notes |
| --- | --- | --- |
| `QUERY` | `0x0c00` | No handle. 48-byte reply, below. |
| `CLAIM` | `0x0c01` | No handle. Admits an app and returns the handle and the control block. |
| `LOAD_BEGIN` | `0x0c02` | Reads the limits from the control block; request word 0 is the client capability word. Deferred. |
| `LOAD_CHUNK` | `0x0c03` | File offset at 0, length at 4; the bytes are at the start of the load buffer. Deferred. |
| `LOAD_END` | `0x0c04` | Deferred; incremental steps advance `AAR_CB_PROGRESS`. |
| `START` | `0x0c05` | `argc` at 0; the argv table and strings are in the load buffer. Deferred. |
| `STOP` | `0x0c06` | An unconditional stop of the named claim. |
| `LIST` | `0x0c09` | Cursor at 0, no handle. The next live claim at or after the cursor index. |
| `RELEASE` | `0x0c0a` | Ends the claim and returns the pair and every buffer. |
| `KICK` | `0x0c0b` | The doorbell, flags at 0. A hint only. |

`0x0c07` (`STATUS`) and `0x0c08` (`DISPLAY_BIND`) are retired: the client reads
the control block, and the display is bound when the client completes
`AAR_OP_DISPLAY_OPEN`. A request with an opcode in `0x0c00` to `0x0c1f` that
the table does not list is unsupported.

`QUERY` (no handle) fills 48 bytes: the ABI major and minor words (0 and 5),
the state at 8, the capability word at 12 (`Z3`, `DISPLAY`, `AUDIO`, `GL`,
`CLUT8`), `HOSTHEAP_LARGEST` at 16 (the largest free block of the
host-visible heap), `QUEUE_DEPTH` at 20 (the depth of a new claim's queues,
64), `COMMIT_FREE` at 24 and `COMMIT_TOTAL` at 28 (the bytes still admissible
for app commits after the system reserve, and that plus the committed bytes),
`RECORDS_FREE` at 32, `TABLES_FREE` at 36 (L2 tables a new admission can
still reserve), `TABLES_TOTAL` at 40 (the pool less the kernel template) and
`HOSTHEAP_FREE` at 44. After a claim, load and release cycle every count
returns to its starting value, `HOSTHEAP_FREE` included.

### Claim and admission

`CLAIM` reserves, in this order, and undoes each step in reverse on a later
refusal. Nothing is published until the last step, so a refused claim leaves
the handle queue, the handle generations, the L2 reserve and free counts, the
host-visible heap and the committed bytes as they were (`zzapp_rec_admit`,
`zzapp_xport_admit`, `zzapp_rec_unadmit`):

1. Record and handle (`zzapp_rec_admit`): the oldest free record index, with
   its stack, L1 table and table pages. Refusal reason `MEMORY` when no
   record is free.
2. L2 reserve of 16 tables (`zzapp_as_reserve_l2`). Refusal reason `TABLES`
   when the reserved tables plus 16 would exceed the pool less the kernel
   template, or when fewer than 16 tables are free and not already promised
   to the unused part of another record's reserve. `TABLES_FREE` in `QUERY` is
   that count.
3. The pair (`zzapp_xport_admit`): one 16 KiB block of the host-visible heap,
   8 KiB aligned, placed in the cacheable part of the heap (below the SDK
   mailbox section at `0x08100000`) so a pair never lands in a page the
   firmware maps non-cacheable. 12 KiB are used: the 4 KiB control block, then
   the SQ and CQ at depth 64. The extra 4 KiB is allocator slack that is never
   touched. Refusal reason `HOSTHEAP` when no such block fits. The control
   block is written with its magic last, `AAR_CB_STATE` reads `CLAIMED`, and a
   barrier follows.
4. Publish: the record becomes `LIVE` and the claim lease starts.

The `CLAIM` reply is the handle at 0, the token of the control block at 4 (a
board offset: the card address less `ADDR_ADJ`, the same form as an
`aar_shared` `haddr`) and the contract size of the block at 8
(`AAR_PAIR_BYTES`, 0x3000). The client reads depth, ring offsets and ring
tokens from the control block descriptors. A refused `CLAIM` replies with the
4-byte `AAR_REASON_*` word alone: `MEMORY` 1, `TABLES` 2, `HOSTHEAP` 3. The
status is `SDK_STATUS_NO_MEMORY` (6) for all three. A Zorro II board refuses
every claim as unsupported.

The pair and every buffer the runtime allocates later (the transport blocks)
are multiples of 8 KiB and 8 KiB aligned. The pair, the service block and the
data block live until `RELEASE`, not until the app ends. That is what lets the
client drain messages and read the final status after the app exited, and what
keeps a late client write harmless: a client that lost its lease may still
write through its mapping, so the memory is not reused until the old client is
known to be gone. The audio ring and the load buffer have earlier ends, listed
below, but a block the client may still touch is kept until `RELEASE` too. The
blocks of a claim are:

| Block | Size | Allocated | Freed |
| --- | --- | --- | --- |
| pair | 16 KiB | `CLAIM` | `RELEASE` |
| service | 48 KiB: 1 KiB scratch, 2 KiB palette (shadow and stage), 4 KiB console input, 8 KiB log, 32 KiB console output ring | first path, name, palette, console or log use | `RELEASE` |
| data | 8 KiB steps up to 256 KiB | first file read or write | `RELEASE` (a growth replaces it, but not while a request still names it, below) |
| audio ring | `0x80 + frames * 4` bytes, rounded to 8 KiB | `AAR_OP_AUDIO_OPEN` | the audio close, once it is answered, and exit on the card route. On the AHI route the client maps the ring, so a close that was not posted, not answered or answered with an error, an exit without a close and a quarantined claim keep it until `RELEASE` |
| load buffer | 72 KiB: 64 KiB staging, 2 KiB argv area, rounded | `LOAD_BEGIN` | once `START` has copied argv, or at the end of the claim |

A spin app with no services costs 16 KiB. A console app costs 64 KiB. A game
that reads files costs up to 320 KiB plus its audio ring. A claim holds at
most five blocks of the surface allocator on top of the app's own 64
(`SURFACE_ALLOCATOR_MAX_BLOCKS` is 4096, 32 KiB of BSS, shared with every P96
bitmap and SDK buffer).

A buffer a request names stays allocated and unchanged until the client has
completed that request. The data block (`FILE_READ`, `FILE_WRITE`), the audio
ring (`AUDIO_OPEN`; `AUDIO_CLOSE` also names it, because it tells the client to
stop using it), the 1 KiB scratch area (every path) and the console-input room
are such buffers. When the app gives a request up while it is still posted (a
timeout, a stop, a corrupt completion queue) its entry keeps the buffer held,
with the request's id and op, until a completion with both arrives. Meanwhile
nothing frees, replaces, reuses or rewrites the buffer (a read or write of any
size, another path call, a larger read: each answers `BUSY`), and the entry
takes no new request, so every blocking call answers `BUSY`. Core 1 takes the
completion queue before it answers, so the late completion is seen without a
doorbell. A completion with another id or op, or a duplicate, releases
nothing. The palette stage, the console output ring and the log slots have
their own in-flight rules and are not held this way. `RELEASE` and an Amiga
reset return everything. A request that could not be posted names nothing.

All of these blocks count against the app's shared limit: `AAR_CB_SHARED_USED`
is the sum of the transport blocks and the app's own `aar_shared_alloc` blocks
(`zzapp_xport_publish_shared`), and an allocation is refused when the total
would pass the limit. The pair itself is exempt from the check, because it is
allocated before the client has written a limit.

### The control block and the doorbell

The control block replaces the old life block word for word:

- The platform writes the lifecycle line (`AAR_CB_STATE`, `AAR_CB_END_REASON`,
  `AAR_CB_EXIT_CODE`, `AAR_CB_FLAGS`, `AAR_CB_PROGRESS`,
  `AAR_CB_LEASE_PERIOD_MS`, `AAR_CB_MSG_CONSUMED`), the fault record
  (`AAR_CB_FAULT_*`: class, syndrome, `pstate`, `pc`, `sp`, `lr` and address),
  the accounting words and the queue indices it owns. The fault record is
  written first, then a barrier, then the end reason and the state, so a
  client that sees the state sees the record. The words are written under the
  services slot-state lock, so both cores may write them.
- The client writes the three limit words (`AAR_CB_PRIVATE_MIB`,
  `AAR_CB_SHARED_MIB`, `AAR_CB_WEIGHT`) before `LOAD_BEGIN`, the stop request
  (`AAR_CB_STOP_REQUEST`), the lease counter (`AAR_CB_LEASE_COUNTER`) and the
  drop counters. The firmware reads them with a load and a barrier and never
  writes them: a lease loss latches the record's own stop and leaves the
  client's stop word alone.
- A fault is the `AAR_FAULT_*` class of the runtime's fault code: data abort
  (alignment aborts as `ALIGN`), prefetch abort as `FETCH`, undefined
  instruction as `ILLEGAL`, anything else (watchdog, forced stop, core
  restart) as `OTHER`. `AAR_CB_FLAGS` carries `AAR_CBF_DISPLAY`, `AUDIO` and
  `GL` from the groups of the app's note, and `CONSOLE` once the console was
  used.

Display state that the old window shared (`FLIP_GEN`, `FLIP_ACK`, the surface
words, the palette generation) is private to the firmware. Client capabilities
that have no control block word (`ZZAPP_CLIENT_LEASE_EXTENDED`, `_AUDIO_AHI`,
`_CLUT8`, `_SCANOUT_ACK`, `_LATEST`) travel in word 0 of `LOAD_BEGIN` and are
kept in the claim words of the record.

Core 1, under the API lock, is the only agent that produces on the SQ and
consumes from the CQ, so each ring has one producer and one consumer at any
time. Core 0 produces and consumes nothing. It binds the pair at `CLAIM`,
before the record is published `LIVE` and so before core 1 can use it, and
afterwards touches only the control block words and block lifetime. A claim's
blocks are freed only once core 1 has finished with the record (its retire
reached `RECLAIMED`); core 0 holds core 1 only in the reset-deadline recovery.
Barriers are the ARM
`dmb ish` at the places `transport.h` names: after an index is read, and before
one is written. There is no cache maintenance on the ARM side, because the
pair is normal cacheable memory that the Zorro master reaches through the ACP.

`KICK` (`0x0c0b`) is the client's doorbell. The flags are `CQ_POSTED` (1) and
`SQ_SPACE` (2). Core 0 looks the handle up, ORs the flags into the claim's
kick word and raises the core-1 doorbell (`scheduler_core1_publish`, SGI 15
only when `service_pending` goes from zero to one). Core 1's service pass pumps
a claim that has a parked wait or a kick bit. A lost kick costs latency, not
data, because the pass also polls parked claims and the queue indices are
authoritative. The firmware never rings the client: the client polls
`AAR_CB_SQ_TAIL`.

The doorbell makes the pass run at once: the SGI handler marks service due,
the interrupt return selects the system thread, and the pass hands the core
back through the pick described under Scheduling. Without a doorbell the pass
comes at the next tick (5 ms) while another app is runnable, and when no app
is runnable the worker polls parked waits continuously. A wait that ends
without a `KICK` is therefore tick-bound unless its producer rings the
doorbell. The card rings it when the vblank interrupt acknowledges a flip
(`zzapp_services_display_vblank`): a client that is synchronised to the
vblank would otherwise lose a frame to the tick. A message or event the
client posts on the completion queue rings the doorbell like a reply: the
client sends a `KICK` with `CQ_POSTED` after each post, and a merged mouse
event sends none. The wait is still a level test the pass repeats, so a lost
kick costs latency only.

### Load, start and stop

`LOAD_BEGIN` reads `AAR_CB_PRIVATE_MIB`, `AAR_CB_SHARED_MIB` and
`AAR_CB_WEIGHT` from the control block. A memory limit above 4095 MiB returns
`BAD_REQUEST`; a shared limit the card cannot grant (the smaller of the
surface heap and one app's alias window of 62 MiB) returns
`SDK_STATUS_NO_MEMORY` at once and leaves the claim as it was. Zero selects
the pool for the private limit and 16 MiB for the shared one
(`ZZAPP_HOST_VISIBLE_DEFAULT_BYTES`). The limits cap the image, stack and
minimum heap commit as well as later growth, and a shared cap is not a
reservation: nothing is set aside, and an allocation can still fail below the
cap when the shared heap or the app's alias range is full. The limits last for
the claim.

`AAR_CB_WEIGHT` is the app's CPU share, described under Scheduling. Zero
selects 1, 1 to 256 (`ZZAPP_WEIGHT_MAX`) is taken as written, and a larger
value returns `BAD_REQUEST`, reported as "weight above 256", and leaves the
claim as it was. The weight is kept in the claim words of the record and goes
to the app's thread when `START` prepares it. It lasts for the claim: the next
claim of the record starts from 1.

`LOAD_BEGIN` then allocates the load buffer and the deferred reply (16 bytes)
names it: the buffer token at 0, its size at 4 (72 KiB), the largest
`LOAD_CHUNK` at 8 (64 KiB) and the offset of the argv area at 12 (64 KiB). It
also takes a 16 MiB staging block of class `APP` (`ZZAPP_FILE_MAX`). The class
is capped at 96 MiB, so six loads that have not reached `LOAD_END` use it all.
When the pool cannot give the block and another claim still holds its staging,
`LOAD_BEGIN` answers `SDK_STATUS_BUSY` and the claim stays `CLAIMED`: the
client asks again once a load has finished. When no other claim holds staging
the claim ends with reason `MEMORY`. `LOAD_CHUNK` places
`length` bytes from offset 0 of the load buffer at `file_off` of the image;
nothing is invalidated, because the buffer is cacheable memory. `LOAD_END`
commits the image on its first step, all or nothing and before any zeroing
(whether the pool or the client's private limit refuses it, the reason is
`MEMORY`), and releases the staging block when it completes. Mapping the image
takes tables from the record's reserve; an image that needs more than 16
gives `TABLES`. A failed `LOAD_END` ends the claim: the frames, tables and
staging go back and the state reads `IDLE`, with the reason in the reply (a
4-byte `AAR_REASON_*`).

`START` carries only `argc`. The argv area of the load buffer holds a `u32`
argc, 16 offsets from the start of the load buffer, then the strings, each at
most 128 bytes including the NUL. The firmware copies the strings into the
app's heap and builds its `argv` there, then frees the load buffer. `STOP`
latches a stop with reason `FORCED` on the claim; the app sees `AAR_EV_STOP`.

A rejected request that carries a reason prints a `[app] reject op
0x<opcode>` line on the console, for a deferred `LOAD_BEGIN`, `LOAD_CHUNK`,
`LOAD_END` or `START` the loader refuses, a limit above 4095 MiB, a limit the
card cannot grant, and a deferred request with request id zero. At most four
lines a second print; the next line printed is `[app] N rejected ops held
back`. A request that fails the handle check prints nothing.

### Leases

A `CLAIMED` or `LOADING` claim has a 30-second lease. `LOAD_BEGIN`, every
completed `LOAD_CHUNK`, each incomplete `LOAD_END` step (it advances
`AAR_CB_PROGRESS`) and each change of the lease counter renew it. This permits
slow Zorro transfers and long asynchronous file reads while reclaiming a
client that has disappeared before or during a load. The client resets its
own inactivity deadline whenever `AAR_CB_PROGRESS` changes.

Once the app runs, the client must change `AAR_CB_LEASE_COUNTER` at least every
two seconds. A client that announced `ZZAPP_CLIENT_LEASE_EXTENDED` gets ten
seconds, so a starved dispatcher or a long interrupts-off period does not lose
the app. The Amiga client, `aar.library`, announces it by default and stores
the counter from a vertical-blank interrupt about every 100 ms, while its
dispatcher process has run in the last five seconds. The dispatcher finds
owners that died without closing their handles and releases their claims, so
a dead client is reclaimed within about a second; a wedged but live owner
keeps its app. If the dispatcher itself stops, the interrupt stops storing and
the lease expires as described here. The firmware writes the period in force
to `AAR_CB_LEASE_PERIOD_MS`
at the claim and when the app starts. Core 0 compares the counter with the
last value it saw on every poll, for each live record.

### Release, quarantine and dead clients

The control block, the rings and the buffers outlive the app and live until
the client releases the claim.

- A normal end (exit, fault, forced stop) retires the record through the
  single retire path. The record becomes `RECLAIMED` (resources returned, shell
  and handle held) with the end reason and exit code set. `AAR_CB_STATE` reads
  `STOPPING` while teardown or ordinary-exit console output is pending, then
  `IDLE`; the pair and every transport block stay until `RELEASE`.
  `RELEASE(handle)` frees the blocks and the record, with a new handle
  generation: the pointers are detached first, then the blocks are returned,
  so a replayed teardown frees nothing twice.
  A client `STOP` of a claim that never ran (claimed, loading or loaded but
  not started) ends it the same way, with the reason `FORCED`.
- Lease loss and an expired load quarantine the claim. A running app whose
  lease expires is stopped and ends with the reason `LEASE`; a claim whose
  load was running ends with `QUARANTINED`, and one with nothing loaded ends
  with `LEASE`. The same retire runs, but the app's own host-visible blocks
  and the audio ring are held, not returned, because the lost client may
  still write through its mapping of them (the AHI route writes the ring's
  `READ_FRAME`). The record's handle is revoked: its
  generation advances to a value the old client does not know, and the shell
  stays under that new handle. A `STOP` or `RELEASE` that names the old handle
  fails the lookup and releases the shell: the client reads its answer before
  it touches the claim again. A `KICK` does not, because the client posts it
  without reading its status. A deferred load call (`LOAD_BEGIN`, `LOAD_CHUNK`,
  `LOAD_END`, `START`) does not either, because the client's lease store may
  still write the control block while the answer is on the way; `aar.library`
  stops storing at the stale answer and releases the shell with a plain
  `RELEASE` under the old handle when its owner closes the handle (or when its
  dispatcher reaps a dead owner).
  `LIST` returns the current handle of a quarantined shell, so an
  administrative `RELEASE` works. The quarantine flag stays on the shell until
  it is released, and the release returns the held blocks with it. Quarantine
  never ages out; it holds one record, up to 360 KiB of transport blocks (pair,
  service, data and audio ring) and the app's own host-visible blocks until
  the client's stale `STOP` or `RELEASE`, an administrative `RELEASE` or an
  Amiga reset, which releases every held shell (see below).
- `RELEASE` on a `LIVE` record latches a stop with reason `FORCED` and sets
  `release_requested`; the retire that core 1 runs completes the release when
  it reaches its last stage. On a `RECLAIMED` record core 0 frees under the
  slot-state lock, after checking the retire stage. On a `CLAIMED` or `LOADING`
  record the record retires first, then it is freed. The reply is the status:
  `OK` for a valid handle whatever the state, `ZZAPP_ERR_STALE` otherwise.

`LIST` takes a cursor (a record index) and returns the next live claim at or
after it. The 48-byte reply has the state at 0, the handle at 4, committed
bytes at 8, the record index at 12 (so a client passes `index + 1` to go on),
app CPU milliseconds at 16 and kernel service milliseconds at 20, the block
reason (byte 24), the end reason (byte 25) and the overload flag (bytes 26 and
27), and the app name at 28 (20 bytes). Handle 0 means there is no further
claim. CPU time starts at zero when a claim is admitted. The block reason is
`NONE` (0), `FILE` (1), `CONSOLE` (2), `EVENT` (3, `aar_msg_wait`), `SLEEP`
(4), `VSYNC` (5) or `AUDIO` (6). The end reasons are `NONE` (0), `EXIT` (1),
`FORCED` (2), `USER_FAULT` (3), `STALL` (4), `STUCK_CALL` (5), `LEASE` (6),
`AMIGA_RESET` (7), `CORE_RESTART` (8) and `QUARANTINED` (9).

### Teardown, reset and restart

A normal exit does not tear the record down on the app's own thread. The exit
call only records the exit and stops the app's thread, in one step with local
interrupts masked: a tick between the two would either strand the stopped
thread with the exit unrecorded, or let the system thread start the teardown
while the app still runs. The thread then hands the CPU on. The system thread
runs the teardown (end-of-app services, GL reclaim, publishing the end state,
then the record retire), the same place faults, forced stops and watchdog
stops are torn down. That matters because a stopped app thread is never
scheduled again, and the retire unmaps every host-visible block, which holds
the address-space lock for longer than a scheduler tick when a block is tens
of MiB.

From the exit call until the teardown has finished, the exit counts as owed
state of the record, separate from the app being run. Core 1 records the exit
first and then publishes `running = 0` with release semantics; core 0 loads
`running` with acquire semantics before it reads the exit state, so whoever
sees the app stopped also sees the cleanup it owes. That ordering is by
construction; the host tests cannot show it.

An Amiga reset is posted to every live record and core 0 waits for core 1's
acknowledgement of each before it resets the surface allocator. Core 1's reset
path tears down a running app, a loading record or an exit still owed, so the
allocator is not reset under a cleanup that still frees blocks from the old
run. Afterwards every held shell (`RECLAIMED` records, quarantined ones
included) is released once. The quarantine flag stays on a quarantined shell
until that release, so the release returns the app's own host-visible blocks
along with the transport blocks to the allocator, which is reset next; the
records are then freed. Core 1 detaches a block's pointer in the
record before the free, then core 0 resets the allocator, which drops every
remaining block; no `surface_allocator_free` is issued for a block the reset
already dropped. Handles and generations survive the reset: a handle from
before it stays stale, and the arena survives because its class is permanent.

If a record has not acknowledged by the shared deadline, core 1 may only be
slow, and a slow worker would run its reset later, after the allocator was
reset and its addresses reused. Core 0 then does not clean up beside it: it
halts core 1 and recovers every record while it is held, through the retire
path the worker would have run. Every step is idempotent, "free the pair"
included (a NULL pointer means done), so a core-1 restart replays what it
interrupted. All live records share one deadline of one second plus 20 ms per
live record, capped at four seconds (`zzapp_reset_deadline_us`); a reset that
core 1 cannot acknowledge waits at most four seconds, and then the recovery is
a cold restart of core 1: it also aborts the SDK requests queued for it and
resets the shared graphics, audio and module state. Core 0 prints `[app]
reset: core 1 did not acknowledge pid N within M ms` for the first record that
missed.

A core-1 restart treats a record that owes a cleanup like a running app and
tears it down with the reason `CORE_RESTART`. The allocator frees by address
and cannot tell owners apart, so every resource it holds (host-visible blocks,
transport blocks, the audio ring) is detached from its owner's record before
it is freed. A halt between the two leaks the block until the allocator is
reset, instead of freeing the block of whoever was given the address
meanwhile. Loader staging and app memory are freed through the card pool,
which checks the owner.

Before asserting core 1's reset, core 0 asks it to park. A responsive worker
finishes its record and heap writes, cleans its own L1 data cache, completes a
DSB, and release-publishes the park acknowledgement. Core 0 acquires that
acknowledgement before resetting it. The worker then stays parked without
touching record or heap state. The shared L2 survives the reset, and core 1
invalidates its local caches before rejoining coherency.

A wedged worker may miss the bounded park wait. That reset remains best
effort: dirty L1 updates can be lost when reset removes the core from SCU
snooping. Idempotent retire stages, handle checks and allocator reconciliation
repair interrupted operations whose published state survived; they cannot
recover arbitrary lost stores. In particular, a lost pointer detach can expose
a block that was already freed. Recovery skips a host-visible block when its
address and size no longer match a live surface allocation, but same-size
address reuse can still refer to another owner.

A record is freed only under the slot-state lock, after its transport is
detached, and its bytes stay readable once it is free. A pass on either core
that walks the records (core 0's vblank scanout acknowledgement and audio
fabric callbacks, core 0's audio poll, core 1's audio poll) therefore uses a
record in the same lock section that found it: an app that exits or is stopped
on the other core cannot free the record between the lookup and the use. A pass
that copies a ring or allocates drops the lock after reading what it needs,
which is safe because an open audio ring or a pending audio request holds the
shell until the handoff ends, and the display mailbox is disabled under its
own lock before the free.

A core-1 restart retires a faulted app the same way. The fault record in the
control block stays readable until `RELEASE`, and a teardown that is still
finishing does not set it again.

### Low-memory events

A free-pool low-water crossing queues one `AAR_EV_LOW_MEMORY` event for each
running app; no additional crossing is queued until the allocator has re-armed
above its high-water mark. There is no crossing counter in the control block.

## Translation attribute changes

The firmware links with `-Wl,--wrap=Xil_SetTlbAttributes`. Every BSP call,
including calls originating inside the BSP archive, enters
`__wrap_Xil_SetTlbAttributes` in `zzapp_as.c`. The wrapper calls the real BSP
function first, then copies changed descriptors into the address-space template
and every live app table through the normal break-before-make path. Each
affected section, large page, or small page receives the required cache,
per-address TLB, and branch-predictor maintenance; core 0's unrelated
translations remain valid.
