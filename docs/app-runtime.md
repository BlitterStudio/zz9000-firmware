# Trusted ARM app runtime (v0)

Contract for the first app runtime on the ZZ9000. The first app is GLQuake, built from id Software's GPL sources, running as one trusted program on core 1 and started from AmigaOS. This document freezes the layouts and the slice boundaries. It is not the implementation.

Base: firmware `feature/rasterix-ef` at `89aa8586`. Headers in `ZZ9000_proto.sdk/ZZ9000OS/src/` are the address authority. `docs/memory-map.md` on this branch still describes a 16 MiB module arena ending at `0x0A000000` and a 128 MiB MiniGL pool cap. Both are stale. `memorymap.h` and `minigl_pool.h` are what the build checks.

## 1. What v0 is

One app at a time. It runs on core 1 in the same privilege mode as the firmware (SVC, no user mode, no per-app page tables). The firmware does not link app code, and the app does not link firmware code. The app calls a table of function pointers passed in at start. Those pointers are the only firmware entry. Later, the loader can pass a page of SVC trampolines at the same table layout. App source does not change for user mode, MMU isolation, or preemptive time slicing. Relinking is not required either, provided the app keeps calling through the table (section 5) and never caches a firmware address it found by other means.

v0 is not a sandbox. A trusted app can store through any writable mapping the firmware has. Isolation is a later slice. v0's fault path covers aborts and undefined instructions whose faulting PC is in the app, plus a watchdog. It does not cover a store that lands in firmware data.

Out of scope for v0: more than one app, threads, sockets, stdin/stdout as a Shell conduit, ZZTop's Apps page, Zorro II, SD-card `ZZ9K/APPS/`, and pause/resume. The lifecycle enum still has a paused value so the wire format does not move when it is added.

### Licences

| Piece | Licence |
|---|---|
| Firmware, including the runtime and the in-firmware GL build | GPL-3.0-or-later |
| `libzzapp`, `zzapp.h`, crt0, linker script, templates | MIT |
| GL front end listed in section 6 | PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later |
| GLQuake port | GPL-2.0-or-later (id's licence) |

The Quake binary links MIT `libzzapp` only. It reaches GL through the table. It must not be linked against firmware objects, the GL front-end objects, or `libzzm_rt`. That table is the project's firmware/app boundary. It is a project rule, not a legal opinion; revisit it before any closed app ships (section 12).

The dual-licence grant on the front-end files is a prerequisite for compiling them into the GPL firmware. Slice C does not start the firmware link until those SPDX lines have actually changed. The 68k library keeps building under the PolyForm option.

### Names

`ZZRun` is the Amiga launcher. `zzapp.h` is the app-facing header. `zzapp_proto.h` is the firmware/launcher shared layout (GPL, not included by apps). An app image is a PIE ELF. There is no `.ZZM` wrapper and no module-slot assignment for apps.

## 2. Core 1 hosting

### What core 1 does today

Core 0 starts core 1 at `core1_entry` (`core2.c`). That stub sets the SVC stack to `SDK_CORE1_STACK_TOP` (`0x08800000`, `memorymap.h`) and branches to `core1_loop`, which programs VBAR and the banked exception stacks, then calls `scheduler_core1_worker` and does not return (`scheduler_arm.c`, worker at line 287). The worker claims one SDK task, or polls the wire-v9 MiniGL service (`mw_service_poll`), or `wfe`s. Core 0 harvests results in `scheduler_core0_poll` and must not wait on core 1 inside a Zorro service. A data abort, prefetch abort, or undefined instruction on core 1 calls `record_fault` (`core2.c` line 185), which parks the core and asks core 0 for `core1_cold_restart` (`scheduler_arm.c` restart path around line 542). That restart throws away every in-flight core-1 task and MiniGL context. It is the right backstop for a firmware fault. It is the wrong first response to an app fault.

The CPU clock is `APU_FREQ 666666687` (`ZZ9000_proto.sdk/ZZ9000FSBL/src/ps7_init_gpl.h` line 87) and `PCW_APU_PERIPHERAL_FREQMHZ {666.666}` (`zz9000_project.tcl`). The global timer counts at half that rate (`COUNTS_PER_SECOND`, used by `XTime_GetTime`; `audio_fabric.c` states the relationship next to its timer helper, around line 285). `zz_time_us` uses that timer. Do not invent a second clock.

Firmware CPU flags, which apps reuse: `-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard` (`ZZ9000OS/Makefile` line 25).

### v0 model

The worker remains the only thread on core 1. When an app is running, the worker is parked on the firmware stack (`SDK_CORE1_STACK_TOP` region) and the app runs on the arena stack, still in SVC. Two ways back to firmware code:

1. Cooperative. `zz_yield`, `zz_sleep_us`, a blocking file call, a flip wait, and the epilogue of every other API call switch to the firmware stack, run one service tick, and switch back. GL calls are API calls, so a Quake frame full of `glVertex` returns to the tick between calls.
2. Timer. Core 1's private timer interrupt fires every 5 ms. The handler does not call `mw_service_poll` and does not `printf`. It saves the app's SVC `sp`/`lr`/`spsr` and the full VFP/NEON state (`d0`–`d31`, `fpscr`), switches to the saved firmware stack, runs one service tick, restores VFP and the app registers, and returns to the app. The existing 8 KiB banked exception stacks (`core2.c` comment on `core1_init_exception_state`) are too small for a tick. The tick runs on the firmware SVC stack.

The tick is the body the worker already has, bounded: honour a pending reset or reclaim flag, claim at most one SDK task, and if `mw_service_has_work()` then call `mw_service_poll` once. Then return. No `wfe` while an app is running. The private-timer IRQ is enabled only while an app is running. Core 0 does not take this timer.

`in_api` counts entries into firmware API functions. The timer tick runs MiniGL and SDK work only when `in_api == 0`, so it cannot re-enter RasterIX or the card-pool lock under an app GL call. Two firmware paths may call the tick while `in_api` is set, because they are not holding the GL lock: the file-wait loop and `zz_sleep_us`. They call it between polls. A GL call does not return until that call's RasterIX work is drained (`minigl_rix_backend_drain` / `minigl_raster_finish`, as the format requires). Between GL calls the engine is idle and a 68k packet may run.

Before entering the app, the worker saves its SVC `sp` in `app_host_sp` and switches to the arena stack. Yield and the timer tick switch back to `app_host_sp` to run services, then back to the app `sp`. The app context record holds the SVC registers and the full VFP state. A tick that used NEON must restore that state or Quake's floats are garbage.

### 68k MiniGL clients while an app runs

They are not rejected and they are not concurrent. Wire-v9 stays bound. Packets remain in the existing transport window. The tick drains one packet when the app is outside a GL call. A 68k client blocked on its fence sees extra latency, not a lost context. Core 0 still only notices the doorbell and wakes core 1; it does not poll the service itself while core 1 is alive.

`MW_MAX_CONTEXTS` is 8 (`mw_service.c`). v0 uses one of those slots for the app context and leaves the rest for 68k clients. Destroying the app context must not touch the others. RasterIX is one engine (`minigl_rix_backend.c`). The lock is "app `in_api` on a GL call" versus "tick inside `mw_service_poll`". Never both.

If the app stays inside one GL call for more than 50 ms, the tick cannot drain 68k work until the call returns. Log it. Do not preempt inside the call. A GL call that does not return for 2 seconds is a firmware hang: the existing core-1 cold restart applies.

### Core 0

Core 0 never blocks on the app, on a file completion, or on a flip ack. Mailbox ops are posted from the existing main-loop poll (`sdk_mailbox.c`) and complete when the short core-1 action finishes (a 64 KiB copy, a state read), not when Quake exits. `APP_START` completes once the app has been entered, not when it returns. `APP_STOP` completes once the stop word is published. ZZRun polls `APP_STATUS` and the shared lifecycle block. The 68k wait is a completion-ring poll in ZZRun, which is the existing mailbox pattern. It is not a wait inside core 0's Zorro service.

Zorro II: `APP_QUERY` returns unsupported. Do not allocate the rings window. The Z2 host window cannot hold it. Z2 behaviour of every existing service stays as it is.

### Faults

Classify in the data-abort, prefetch-abort, and undefined handlers (`arm_exception_handler_id_*` in `core2.c`) before `record_fault`'s park:

- Faulting PC (the address those handlers already print from `DataAbortAddr`, `PrefetchAbortAddr`, `UndefinedExceptionAddr`) lies in the app arena, or the abort mode's return address lies in the app arena: this is an app fault. Capture `pc`, `sp`, `lr`, DFAR/IFAR, DFSR/IFSR, `cpsr` into the lifecycle block. Switch to the firmware SVC stack. Do not return to the faulting instruction. Do not set `core1_restart_request`. Enter `zzapp_force_stop` with `ZZ_FAULT_DATA`, `ZZ_FAULT_PREFETCH`, or `ZZ_FAULT_UNDEF`, reclaim, and resume `scheduler_core1_worker`. Firmware services keep running. 68k MiniGL contexts that were not the app context stay up. The app context is destroyed.
- Any other fault, including a fault inside an API call or the GL front end: existing `record_fault` path, cold restart, existing poison and reclaim. The app is not restarted after that restart. Core 0 clears the app-running flag in the coherent task-queue block and reclaims `CARD_POOL_CLASS_APP`.

The app-running flag and the progress timestamp live in the coherent task-queue region (`SDK_TASKQ_REGION_ADDRESS`, `0x08600000`), which core 0 owns. They do not live in core-1 BSS. `core1_cold_restart` does not zero firmware BSS, and a stale BSS flag would relaunch a dead app or skip reclaim.

Write-protect firmware `.text` (read-execute, not writable) on the core-1 translation tables before the first app entry, if the current tables are writable there. That is an attribute change, not user mode. Firmware `.data` and the card pool stay writable. A wild store into those will not abort. Do not claim the app was contained.

The handler still frees a card-pool or malloc lock only if this core owns it (`sdk_smp_lock_reset_*_if_owned` in `record_fault`). An app fault that interrupted an API call owns those locks the same way a task fault does. Free them only with the owner check, then reclaim the app owner. Do not free a lock core 0 holds.

### Watchdog

App progress is any yield or API epilogue, which stores `XTime_GetTime()` into the coherent shared word. The 5 ms timer IRQ checks it. If 2 seconds pass with no progress and `in_api == 0`, the IRQ does not return to the app: it force-stops with `ZZ_FAULT_WATCHDOG`, same reclaim as an abort, no cold restart.

If the app has executed `cpsid` (it can; v0 is privileged), the timer IRQ does not run and the progress word goes stale. Core 0's existing poll sees that. After 2 seconds stale, core 0 takes the existing cold-restart path. That is the backstop, not the normal stop. The status report must say watchdog-plus-restart, so a hung Quake is not mistaken for a firmware crash that happened to lose 68k clients. After this restart, 68k MiniGL contexts are gone, as they are today after any core-1 restart (`mw_service_on_core1_fault`).

A stop request (Ctrl-C, close gadget, `APP_STOP`) sets the stop word. The app sees `zz_should_stop` and a `ZZ_EV_STOP` input. Grace is 500 ms from the stop word, then the same force-stop as the watchdog. Normal stop does not cold-restart.

### Amiga reset and client loss

`minigl_begin_amiga_reset` (`main.c` line 580) is the existing entry. Call `zzapp_on_amiga_reset` at the start of that path, before transport and card-pool teardown. `scheduler_quiesce_for_reset` (`scheduler_arm.c`) only drains for a bounded time and then cold-restarts core 1 if a task is still active. Force-stop the app before that wait, or a live app becomes a firmware restart. App card-pool blocks use a reset-reclaimable class, never `CARD_POOL_CLASS_FIRMWARE` (that class survives reset, `card_pool.h`).

Client loss without a reset: ZZRun writes `heartbeat_ms` in the lifecycle block on every pass of its loop. If that word is stale for 2 seconds while state is RUNNING, treat it as stop. Firmware progress does not refresh the launcher heartbeat. The launcher heartbeat does not refresh the app watchdog.

### Constants

```c
#define ZZAPP_TICK_US          5000u
#define ZZAPP_WATCHDOG_US      2000000u
#define ZZAPP_STOP_GRACE_US    500000u
#define ZZAPP_LEASE_US         2000000u
#define ZZAPP_GL_CALL_LOG_US   50000u
#define ZZAPP_API_STUCK_US     2000000u
#define ZZAPP_FILE_TIMEOUT_US  30000000u
```

## 3. Image and loader

### Where the arena is

Not a new fixed DDR carve-out (`memorymap.h` header comment: large working memory comes from the card pool). Not the module arena.

Module arena: `FIRMWARE_MODULE_ARENA_ADDRESS 0x09000000`, size `0x02000000`, end `0x0B000000` (`memorymap.h` lines 380–383). Slots in `zzm_manifest.c` lines 8–15 run from MPEG2 at `0x09000000` through AAC at `0x0A500000` (1 MiB). Apps do not get a slot and do not link `libzzm_rt`.

Card pool permanent prefix starts at `CARD_POOL_PERMANENT_ADDRESS` (arena end, `0x0B000000`) and ends at `Z3_FASTRAM_DDR_ADDRESS` (`0x2FE00000`). Page size `0x00010000`. The revocable tail `0x2FE00000`–`0x3FE00000` is Fast RAM while advertised.

The arena is one `card_pool_alloc` (`card_pool.h` line 122) of 96 MiB from the permanent prefix only. Refuse the revocable range even when Fast RAM is off: an app must not be aliased by a later Fast RAM advertisement. If the permanent prefix cannot satisfy 96 MiB, start fails with `ZZ_ENOMEM`. Do not fall back to the tail.

New owner class, next value after `CARD_POOL_CLASS_MINIGL` (7):

```c
#define CARD_POOL_CLASS_APP  8
#define CARD_POOL_LIMIT_APP  (96u << 20)
```

`CARD_POOL_CLASS_COUNT` becomes 9. Instance is the app generation, so a recycled owner does not match a block the previous run lost. Reclaim the owner on stop, fault, and Amiga reset. GL texture and depth memory stays `CARD_POOL_CLASS_MINIGL` (`minigl_pool.h` cap is 384 MiB, a ceiling, not a reservation). Destroying the app's `mw_ctx` frees that context's MiniGL owner and no other context's.

96 MiB covers the ELF, BSS, a 1 MiB stack, and Quake's hunk. It is a quota, not a tuneable in v0. The ELF note may request less. It may not request more.

### ELF

Position-independent ARM ELF, little-endian, EABI, hard-float (`EF_ARM_ABI_FLOAT_HARD`). Link `-fPIE -pie -nostdlib` with the script in section 7. No `PT_INTERP`, no `DT_NEEDED`, no dynamic linker.

Relocations: `SHT_RELA` only, type `R_ARM_RELATIVE` (23) only, symbol index 0. Apply `*(uint32_t *)(load_bias + r_offset) = r_addend + load_bias` for word-sized RELATIVE records. Reject `SHT_REL`, every other type, and any allocated section that would fall outside the arena. This is deliberately not the module loader: `docs/modules.md` rejects relocations because a `.ZZM` is fixed-address. An app is not a module.

Segments: `PT_LOAD` only, at most 4. Reject a segment that is both writable and executable. `p_vaddr` is an offset from the arena base, not a firmware address. The loader picks the physical base; the relocator fixes the image up to that base. v0 then runs identity-mapped, so the relocated VAs equal the card addresses. User mode later maps those same physical pages at those same VAs. That is why the image is PIE rather than linked at a constant: the pool cannot promise a fixed base, and a later mapping can keep the VAs the relocator already wrote.

Entry is `e_entry`. The firmware sets, then branches:

- `r0` = pointer to the API table (section 5)
- `r1` = argc
- `r2` = argv (pointers into the arena, copied from the launcher window before the jump)
- `sp` = arena top, 8-byte aligned, descending

crt0 does not touch firmware BSS, does not install vectors, and does not enable or disable the MMU. It clears no BSS; the loader did. It calls `zz_main(argc, argv)`. Return is `zz_exit` of that status.

Stack is the top 1 MiB of the arena. The note may request less, down to 64 KiB, not more. A 64-byte canary sits at the low end of the stack; the yield path checks it and force-stops with `ZZ_FAULT_DATA` if it is damaged. There is no unmapped guard page until user mode.

Heap is `[image_end, stack_base)`. `libzzapp` provides `_sbrk` over that range and newlib malloc. The app must not call the firmware `_sbrk`. Firmware newlib heap is `0x08800000`–`0x09000000` (`memorymap.h`) and is not the app heap. Quake's hunk is a malloc from this heap, not a second pool.

`.zzapp` ELF note, little-endian, name `ZZAPP`:

```c
struct zzapp_note {
    uint32_t magic;       /* 0x50505A41 */
    uint32_t abi_major;   /* 0 */
    uint32_t abi_minor;   /* 0 */
    uint32_t stack_bytes; /* 0 = 1 MiB */
    uint32_t flags;       /* bit0 display, bit1 audio, bit2 gl */
    uint32_t reserved;
};
```

Reject major != 0. A higher minor is accepted only if the runtime's minor is at least the image's minor. A lower minor is fine.

Limits: file image (sum of `p_filesz`) at most 16 MiB. Sum of `p_memsz` plus stack plus 64 KiB must fit in 96 MiB. BSS is the `p_memsz - p_filesz` tail of each segment; the loader zeroes it. Argv and the API table are not inside the ELF.

Before the branch: `Xil_DCacheFlushRange` on the whole arena, `Xil_ICacheInvalidateRange` on each executable segment, then `dsb` and `isb`. Same obligation the module loader has before its entry call.

### Load path

The launcher never picks the address. It streams the file. Core 1 copies.

1. `APP_LOAD_BEGIN` allocates the arena and a generation. State LOADING.
2. `APP_LOAD_CHUNK` copies from the non-cacheable staging window into the arena at the given file offset. Chunk length 1..65536. Core 0 does not copy.
3. `APP_LOAD_END` checks the ELF, relocates, zeroes BSS, runs cache maintenance. On reject, free the arena and return to IDLE. State becomes STARTING only on `APP_START`.

A second begin while not IDLE is `ZZ_EBUSY`. Stop during LOADING frees the arena and returns to IDLE.

### States

`IDLE`, `LOADING`, `STARTING`, `RUNNING`, `PAUSED` (unused in v0), `STOPPING`, `FAULTED`. `FAULTED` is visible until the launcher has read `APP_STATUS` once after the fault, or until 2 seconds, then IDLE. The generation increments on every leave from IDLE. A flip ack or file completion carrying the old generation is ignored.

## 4. Launcher protocol

ZZRun is a Shell command and a Workbench tool. One running app per machine in v0. A second `APP_LOAD_BEGIN` gets `ZZ_EBUSY`.

Zorro III only. Open `zz9k.library`, reject a board that is not Z3, and use the existing mailbox (`SDK_MAILBOX_MAGIC 0x5a5a394b`, `sdk_mailbox.h`). Wire entries are big-endian 64-byte records with a 48-byte payload (`ZZ9KMailboxWireEntry` in `sdk/include/zz9k/abi.h`; the comment above that struct says one representation for m68k, ARM, and host tools). New ops follow that. Do not add a second doorbell.

### Opcodes

New service id `ZZ9K_SERVICE_APP = 0x0c00` in `sdk/include/zz9k/abi.h` `enum ZZ9KService` (next id after `ZZ9K_SERVICE_VIDEO` `0x0b00`; `0x8200` is the console vendor service and is not a pattern to extend). Matching `SDK_OP_*` in `sdk_mailbox.h`. Range `0x0c00`–`0x0c1f` is reserved. v0 uses:

| Opcode | Name | Completes when |
|---|---|---|
| `0x0c00` | `APP_QUERY` | immediately; caps and state |
| `0x0c01` | `APP_ALLOC_WINDOW` | rings window allocated |
| `0x0c02` | `APP_LOAD_BEGIN` | arena reserved or rejected |
| `0x0c03` | `APP_LOAD_CHUNK` | chunk copied or rejected |
| `0x0c04` | `APP_LOAD_END` | ELF accepted or rejected |
| `0x0c05` | `APP_START` | app entered, or entry faulted |
| `0x0c06` | `APP_STOP` | stop word published; does not wait |
| `0x0c07` | `APP_STATUS` | lifecycle snapshot copied out |
| `0x0c08` | `APP_DISPLAY_BIND` | surface recorded or rejected |

Payloads are big-endian in the 48-byte mailbox payload. Fields that do not fit live in the rings window, not in a stretched mailbox entry.

`APP_QUERY` reply: `abi_major`, `abi_minor`, `state`, `caps`. Caps bit0 Z3 app runtime, bit1 display, bit2 audio, bit3 GL. Z2 clears all of them and returns unsupported on every other app op.

`APP_ALLOC_WINDOW` reply: `arm_address`, `size`, `host_offset`. `host_offset` is `arm_address - address_adjust`, using the same adjust the firmware already publishes for shared allocations (`ADDR_ADJ` in `memorymap.h` is `FRAMEBUFFER_ADDRESS - MNT_FB_BASE`). ZZRun maps `board_base + host_offset` and does not hardcode `0x1F0000`. The window is allocated from the host-visible heap (`0x033F0000`–`0x08200000`) with the non-cacheable 4 KiB attribute used for the mailbox and direct-ring pages. Size is `ZZAPP_WINDOW_BYTES` below. Failure is `ZZ_ENOMEM`. Do not place this window in P96 VRAM. The Z3 mailbox reservation itself stays at `SDK_MAILBOX_Z3_ADDRESS` (`0x08100000`, `memorymap.h`); the app window is a separate heap allocation, not an extension of that 64 KiB.

`APP_LOAD_CHUNK` payload: file offset, length (1..65536), staging offset within the window (always `ZZAPP_STAGING_OFF`). The 68k writes the bytes before posting the op. The ARM side reads them only through the non-cacheable mapping.

`APP_START` payload: argc. argv strings are already in the window's argv area. Completion status is the entry result. Exit status comes later via `APP_STATUS`.

`APP_DISPLAY_BIND` payload: width, height, pitch (bytes), format, `card_front`, `card_back`. Addresses are ARM physical. `card_back == 0` means one P96 bitmap: the runtime allocates a back buffer as a second `CARD_POOL_CLASS_APP` block, counted against the 96 MiB cap, and copy-flips into `card_front`. Both addresses, when non-zero, must lie in card DDR the firmware may write: P96 VRAM `0x00200000`–`0x033F0000` or the host-visible heap. The firmware does not allocate inside P96 VRAM. P96 allocations are invisible to it (`memorymap.h` comment on `Z3_P96_VRAM_*`). ZZRun allocates screen bitmaps through P96 and passes the addresses.

`APP_STOP` and the lifecycle stop word are the same request. Either is enough.

### Rings window

One non-cacheable block. Offsets are part of the contract. Big-endian integer fields. `_Static_assert` these offsets in `zzapp_proto.h`.

```c
#define ZZAPP_WINDOW_BYTES     0x00060000u   /* 384 KiB allocation */

#define ZZAPP_LIFE_OFF         0x00000000u
#define ZZAPP_LIFE_BYTES       0x00000100u

/* header at +0: head, tail, reserved0, reserved1 (16 bytes).
   events at +16. 256 * 16 = 4096. Region padded to 0x1100. */
#define ZZAPP_INPUT_OFF        0x00000100u
#define ZZAPP_INPUT_HDR        16u
#define ZZAPP_INPUT_CAP        256u
#define ZZAPP_EVENT_BYTES      16u
#define ZZAPP_INPUT_BYTES      0x00001100u   /* ends at 0x1200 */

/* header at +0: head, tail (8 bytes). records follow. */
#define ZZAPP_LOG_OFF          0x00001200u
#define ZZAPP_LOG_BYTES        0x00008000u   /* ends at 0x9200 */

#define ZZAPP_FILE_OFF         0x00009200u
#define ZZAPP_FILE_BYTES       64u
#define ZZAPP_PATH_OFF         0x00009240u
#define ZZAPP_PATH_BYTES       256u
#define ZZAPP_ARGV_OFF         0x00009340u
#define ZZAPP_ARGV_BYTES       2048u          /* ends at 0x9B40 */

#define ZZAPP_DATA_OFF         0x00009B40u
#define ZZAPP_DATA_BYTES       0x00040000u   /* 256 KiB, ends at 0x49B40 */
#define ZZAPP_STAGING_OFF      0x00049B40u
#define ZZAPP_STAGING_BYTES    0x00010000u   /* 64 KiB, ends at 0x59B40 */
```

Bytes from `0x59B40` to `0x60000` are reserved zero.

Lifecycle block (`ZZAPP_LIFE_OFF`), each field a big-endian `u32` unless noted:

| Offset | Field | Writer |
|---:|---|---|
| `0x00` | magic `0x5A415050` (`'ZAPP'`) | firmware, at alloc |
| `0x04` | layout version `1` | firmware |
| `0x08` | state | firmware |
| `0x0C` | generation | firmware |
| `0x10` | heartbeat_ms | launcher |
| `0x14` | stop (1 = request) | launcher |
| `0x18` | fault_code | firmware |
| `0x1C` | exit_code (`int32`) | firmware |
| `0x20` | pc | firmware |
| `0x24` | sp | firmware |
| `0x28` | lr | firmware |
| `0x2C` | far | firmware |
| `0x30` | fsr | firmware |
| `0x34` | cpsr | firmware |
| `0x38` | flags | firmware |
| `0x3C` | input_drops | firmware |
| `0x40` | log_drops | firmware |
| `0x44` | flip_gen | firmware |
| `0x48` | flip_ack | launcher |
| `0x4C` | flip_showing (0 or 1) | firmware |
| `0x50` | width | mirrored from bind |
| `0x54` | height | mirrored from bind |
| `0x58` | pitch | mirrored from bind |
| `0x5C` | format | mirrored from bind |
| `0x60` | card_front | launcher |
| `0x64` | card_back | firmware if it allocated one, else launcher |
| `0x68` | app progress xtime low | firmware, diagnostic |
| `0x6C` | app progress xtime high | firmware, diagnostic |

State enum: `0` idle, `1` loading, `2` starting, `3` running, `4` paused (unused), `5` stopping, `6` faulted.

Fault codes: `0` none, `1` data abort, `2` prefetch abort, `3` undefined, `4` watchdog, `5` forced stop (grace or reset), `6` watchdog that fell through to cold restart.

Flags: bit0 display wanted (ELF note, mirrored), bit1 audio, bit2 GL, bit8 display requested and not yet bound, bit9 vsync wanted.

Input event, 16 bytes, big-endian, array starts at `ZZAPP_INPUT_OFF + ZZAPP_INPUT_HDR`:

```c
struct zzapp_event {
    uint16_t type;
    uint16_t code;         /* Amiga rawkey, or button id */
    int16_t  dx;
    int16_t  dy;
    uint16_t buttons;      /* IEQUALIFIER on keys; mouse/joy buttons */
    uint16_t reserved;
    uint32_t time_ms;
};
```

Head and tail at `ZZAPP_INPUT_OFF` are monotonic `u32` indices. Mask with 255. The launcher is the only producer. A full ring drops the new event and increments `input_drops`. Do not block the Amiga input handler.

Event types: `1` key down, `2` key up, `3` mouse, `4` joystick, `5` stop, `6` resize. Keys are Amiga rawkey codes, not ASCII. Joystick `dx`/`dy` are `-1, 0, 1` in v0. The fields are `int16` so a later analog source can use the full range without a layout change.

Log ring: `u32` head and `u32` tail at `ZZAPP_LOG_OFF` (byte offsets, monotonic), records at `ZZAPP_LOG_OFF + 8`. A record is a big-endian `u16` length (excluding the length field) plus bytes, padded to 4. Firmware writes, launcher prints. A full ring drops the line and increments `log_drops`. Usable log bytes are `ZZAPP_LOG_BYTES - 8`.

File slot at `ZZAPP_FILE_OFF`, 64 bytes, big-endian. One outstanding request. Seqlock: the writer sets `seq` odd, fills the rest, then sets `seq` even. The reader retries while `seq` is odd or the two reads of `seq` differ.

```c
struct zzapp_file_wire {   /* 64 bytes; do not add a field */
    uint32_t seq;          /* 0 */
    uint32_t op;           /* 4 */
    int32_t  fd;           /* 8 */
    int32_t  flags;        /* 12 */
    int32_t  whence;       /* 16: 0 set, 1 cur, 2 end */
    int32_t  err;          /* 20: 0 or a ZZ_* code */
    uint32_t path_off;     /* 24: offset from window base; 0 if none */
    uint32_t length;       /* 28 */
    uint32_t out_n;        /* 32 */
    uint32_t mode;         /* 36: stat, 1 file, 2 dir */
    uint64_t offset;       /* 40 */
    uint64_t out_pos;      /* 48: seek position; open/stat file size */
    uint32_t mtime;        /* 56: seconds, 0 if unknown */
    uint32_t reserved;     /* 60 */
};
```

File ops: `1` open, `2` close, `3` read, `4` write, `5` seek, `6` stat, `7` list open, `8` list next, `9` list close. Read and write transfer through `ZZAPP_DATA_OFF`, at most `ZZAPP_DATA_BYTES` per completion. The app-facing `zz_file_read` loops until the caller's buffer is full, the file ends, or the call errors. Quake's lump reads are larger than 256 KiB; the loop is in the firmware API, not in Quake.

Paths sit at `path_off` as a NUL-terminated Amiga string, at most 255 bytes, inside `ZZAPP_PATH_OFF`. Relative paths are under the launcher root (Shell current directory, or the icon's directory, or ToolType `ROOT`). A path containing `:` is rejected unless ToolType `ABS=1`. A `..` that would leave the root is rejected. This stops a bad path. It is not a security boundary. Open flags: `1` read, `2` write, `4` create. FDs are small integers, 16 of them, starting at 3.

Argv area: `u32` argc, then 16 offsets from the window base, then the strings, each at most 127 bytes plus NUL. The launcher fills this before `APP_START`. Firmware copies the strings into the arena before the jump so the app does not retain a pointer into the non-cacheable window.

### Screen, input, Ctrl-C, reset

Display open is an app API call. Firmware sets `flags` bit 8 and writes the requested width, height, and format into the lifecycle fields. ZZRun sees that on its poll, opens the P96 screen, binds with `APP_DISPLAY_BIND`, and clears bit 8 only after bind succeeds. If it cannot open the mode, it sets the stop word. The app's `zz_display_open` returns `ZZ_EIO`.

Default Quake request, issued by the port, not hardcoded in ZZRun: 640×480, `ZZ_FMT_RGB565`, two P96 bitmaps if the screen can allocate them, otherwise one. Pitch is the bitmap's byte stride, not `width * bytes_per_pixel` assumed. A P96 bitmap's ARM address is `(Planes[0] - MemoryBase) + 0x00200000`, the same `board_offset + ADDR_ADJ` the mailbox already uses. ZZRun passes that address and does not invent a second conversion. RasterIX renders into the context-owned card-pool target (`minigl_present.h`: that target is packed RGB565). Present always copies that target into the bound P96 bitmap (`minigl_present_copy_rgb565`, or `minigl_present_copy` for a 32-bit screen). v0 does not point RasterIX at the scanout address.

Flip: the app calls `zz_display_flip` or `zz_gl_swap`. Firmware drains the context, copies the RasterIX target into the back P96 bitmap (or the only bitmap), publishes `flip_gen`, and yields until `flip_ack == flip_gen` or stop. ZZRun sees the new generation, points the screen at the completed bitmap when it has two, then `WaitTOF` if bit 9 is set, and writes `flip_ack`. The firmware updates `flip_showing`. A late ack for a previous generation is ignored. The next present copies into the buffer whose previous flip has been acked. A CPU-drawn surface (`pixels`) writes `surface.back` itself; GL does not.

On exit, fault, or reset, ZZRun closes the screen it opened and returns to the public screen it recorded at start. It does this even if the firmware has already reclaimed the app. Firmware must finish or abandon any present-copy into that bitmap before reclaim (`minigl_raster_quiesce` and the backend abandon used by `minigl_reset_abort_and_free` in `main.c`). RasterIX itself writes the context buffer, not the P96 bitmap.

Ctrl-C (`SIGBREAKF_CTRL_C`) and the window close gadget set the stop word and push `ZZ_EV_STOP`. ZZRun stays alive through the grace period so it can read the fault block and restore the screen. It then exits with the app's `exit_code`, or 20 on fault.

Amiga reset: ZZRun will not get to clean up. Firmware reclaim is what frees the arena. The next launch after reboot starts at IDLE.

ToolTypes: `ROOT`, `ABS`, `MODE` (optional; the app request wins if MODE is absent). Shell form: `ZZRun <app.elf> [args]`. Args after the ELF path become argv. Quake timedemo: `ZZRun RAM:glquake.elf +timedemo demo1`, with `ROOT` pointing at the directory that contains `id1/`.

Build ZZRun like `ZZCapture`: `tools/amiga-docker.sh`, image `sacredbanana/amiga-compiler:m68k-amigaos` (`zz9000-drivers/tools/amiga-docker.sh` line 23), `m68k-amigaos-gcc -m68030 -O2 -Wall -Wextra -noixemul -lamiga` (`ZZCapture/build.sh`). Never the gcc10 image.

## 5. App API table v0

Header: `sdk/zzapp/include/zzapp.h`, MIT. Apps include it and link `libzzapp`. They do not include `zzapp_proto.h`, firmware headers, or MiniGL dispatch headers.

```c
#define ZZAPP_API_MAJOR 0u
#define ZZAPP_API_MINOR 0u
#define ZZAPP_API_VERSION ((ZZAPP_API_MAJOR << 16) | ZZAPP_API_MINOR)

#define ZZ_OK         0
#define ZZ_EIO       -1
#define ZZ_EINVAL    -2
#define ZZ_ENOMEM    -3
#define ZZ_EBUSY     -4
#define ZZ_ENOENT    -5
#define ZZ_EACCES    -6
#define ZZ_ENOSYS    -7
#define ZZ_ETIMEDOUT -8
#define ZZ_EFAULT    -9
#define ZZ_ENOSPC    -10
#define ZZ_EEXIST    -11
#define ZZ_EBADF     -12

#define ZZ_FMT_RGB565   1u
#define ZZ_FMT_BGRA8888 2u   /* P96 32-bit; memory bytes B,G,R,A. Not ARGB. */

#define ZZ_EV_KEY_DOWN 1u
#define ZZ_EV_KEY_UP   2u
#define ZZ_EV_MOUSE    3u
#define ZZ_EV_JOY      4u
#define ZZ_EV_STOP     5u
#define ZZ_EV_RESIZE   6u

#define ZZ_ALLOC_UNCACHED 1u
#define ZZ_FILE_READ   1u
#define ZZ_FILE_WRITE  2u
#define ZZ_FILE_CREATE 4u
#define ZZ_SEEK_SET 0
#define ZZ_SEEK_CUR 1
#define ZZ_SEEK_END 2
```

The table begins with `size` and `version`. A newer runtime may append fields. A caller reads a field only when `size` covers it. A missing optional call returns `ZZ_ENOSYS`. v0 does not bump major. A later major keeps the old table for one release; that is not this slice.

```c
typedef struct zzapp_surface {
    uint32_t id;
    uint32_t width, height, pitch, format;
    void    *front;       /* CPU mapping; NULL if the app does not draw it */
    void    *back;
    uint32_t card_front;
    uint32_t card_back;
} zzapp_surface;

typedef struct zzapp_event {
    uint16_t type, code;
    int16_t  dx, dy;
    uint16_t buttons, reserved;
    uint32_t time_ms;
} zzapp_event;

typedef struct zzapp_stat {
    uint64_t size;
    uint32_t mtime;
    uint32_t mode;        /* 1 file, 2 dir */
} zzapp_stat;

typedef struct zzapp_dirent {
    uint32_t mode;
    char     name[256];
} zzapp_dirent;

typedef struct zzapp_audio {
    int16_t *samples;              /* stereo interleaved S16LE */
    uint32_t frames;               /* capacity in frames, power of two */
    volatile uint32_t *read_frame; /* firmware advances */
    volatile uint32_t *write_frame;
    uint32_t rate;                 /* granted */
    uint32_t channels;             /* 2 */
} zzapp_audio;

typedef struct zzapp_gl zzapp_gl;

typedef struct zzapp_api {
    uint32_t size;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved0;

    void     (*exit)(int code);
    void     (*yield)(void);
    int      (*should_stop)(void);
    uint64_t (*time_us)(void);
    void     (*sleep_us)(uint32_t us);

    void    *(*alloc)(uint32_t bytes, uint32_t align, uint32_t flags);
    void     (*free)(void *p);
    void     (*heap_range)(void **base, uint32_t *bytes);

    void     (*log)(const char *msg, uint32_t len);

    int      (*file_open)(const char *path, uint32_t flags);
    int      (*file_close)(int fd);
    int      (*file_read)(int fd, void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_write)(int fd, const void *buf, uint32_t len, uint32_t *out_n);
    int      (*file_seek)(int fd, int64_t off, int whence, int64_t *out_pos);
    int      (*file_stat)(const char *path, zzapp_stat *out);
    int      (*file_list_open)(const char *path);
    int      (*file_list_next)(int list, zzapp_dirent *out);
    int      (*file_list_close)(int list);

    int      (*display_open)(uint32_t w, uint32_t h, uint32_t format,
                             uint32_t flags, zzapp_surface *out);
    int      (*display_flip)(uint32_t surface_id);
    int      (*display_vsync)(void);
    int      (*display_close)(uint32_t surface_id);

    int      (*input_poll)(zzapp_event *out, uint32_t max, uint32_t *out_n);

    int      (*audio_open)(uint32_t rate, uint32_t channels, zzapp_audio *out);
    int      (*audio_close)(void);

    const zzapp_gl *(*gl_bind)(uint32_t surface_id);
    void     (*gl_swap)(void);
    void     (*gl_unbind)(void);
} zzapp_api;
```

`display_open` flags: bit0 vsync. Quake sets it. `gl_bind` returns NULL on failure. One GL context in v0. A second bind returns NULL until unbind.

`alloc`/`free` are the arena heap. `align` is a power of two, at least 8. `heap_range` reports the whole heap span. `ZZ_ALLOC_UNCACHED` is accepted and ignored while the arena is a normal cacheable mapping. When user mode maps an uncached grant, the flag starts meaning something. Apps that need uncached memory must pass it now.

`log` is rate-limited to 32 lines per second into the ring and 8 lines per second to the UART. Excess increments `log_drops` and is discarded. App code has no other path to the firmware console.

`time_us` is monotonic microseconds from the global timer. `sleep_us` yields in tick steps until the deadline. It is not a busy wait that skips the service tick.

`file_*` yields until the launcher completes the seqlock or 30 seconds pass (`ZZ_ETIMEDOUT`). Stop during a wait returns `ZZ_EIO` after the current launcher op is abandoned (generation bump).

`input_poll` never blocks. An empty ring returns `out_n = 0`. The app's idle loop calls `yield` or `sleep_us`, not a spinning poll.

`audio_open` negotiates rate. Channels must be 2. The ring is stereo S16LE, 8192 frames, in a non-cacheable mapping so the fabric snapshot does not depend on the app cleaning the cache. `read_frame` is the device position. Quake's `SNDDMA_GetDMAPos` returns `*read_frame * channels` (mono samples), matching `shm->samplepos`. The firmware side attaches `audio_fabric_producer_ops` (`audio_fabric.h`) and calls `audio_fabric_producer_rate_set`. It does not write the formatter ring itself.

Audio slots today: pump `0`, mailbox `1`, reserved `2` (`AUDIO_FABRIC_SLOT_*` in `audio_fabric.h`). Slot 0 stays the SDK/media pump. The app takes slot 2 if it is free, else slot 1. If both are live, `audio_open` returns `ZZ_EBUSY` and the Quake port runs silent. Do not steal a lease. Do not add a fourth slot in v0. Do not open the app stream with `SDK_OP_AUDIO_RING_ACQUIRE`: that dispatch accepts only 8000/12000/24000/32000/44100/48000. The app is an in-firmware producer (`audio_fabric_producer_attach` / `audio_fabric_producer_rate_set`). `fabric_rate_known` (`audio_fabric_lease.c`) includes 22050, and `audio_convert.c` has the 22050 to 48000 ratio. Grant 22050 when asked. 11025 is pump-only (comment above `fabric_rate_known`); do not grant it on this producer. The app must use `out->rate`. A fabric period is `AUDIO_BYTES_PER_PERIOD` 3840 bytes, 20 ms of 48 kHz stereo S16 (`memorymap.h` lines 13-14). The app ring is not that period; the producer slices it.

`gl_swap` flushes the front end, drains RasterIX, copies the context target into the bound P96 bitmap (section 6), and `display_flip`s. It is the `MGLSwitchDisplay` equivalent (`amiberry_backend.c` line 1726). It is not a wire present to a 68k-owned bitmap, and it does not retarget RasterIX at the scanout address.

`exit` does not return. crt0 calls it if `zz_main` returns.

### 5.1 GL table

`zzapp_gl` is named function pointers, not the 214-slot MiniGL dispatch table (`amiberry_dispatch_entries.h`). That table includes window, GLUT, and quadric calls the ARM build does not export. Append-only. `size` first. Signatures are classic GL 1.1 C, no `GLcontext` argument. The runtime holds the one context and calls the front end's `GL*(context, ...)` internally. Token values are the Khronos numbers Quake already uses. Integer parameters in the table are `int32_t` / `uint32_t`. `zzapp.h` does not include a Hyperion or Khronos header.

Initial set, in this order, which is the v0 `size`:

`Begin`, `End`, `Vertex2f`, `Vertex3f`, `Vertex3fv`, `TexCoord2f`, `TexCoord2fv`, `Color3f`, `Color3fv`, `Color3ubv`, `Color4f`, `Color4fv`, `Color4ub`, `Color4ubv`, `Enable`, `Disable`, `IsEnabled`, `AlphaFunc`, `BlendFunc`, `DepthFunc`, `DepthMask`, `DepthRange` (two `double`s), `CullFace`, `FrontFace`, `ShadeModel`, `PolygonMode`, `Clear`, `ClearColor`, `Viewport`, `Scissor`, `MatrixMode`, `LoadIdentity`, `LoadMatrixf`, `PushMatrix`, `PopMatrix`, `Rotatef`, `Translatef`, `Scalef`, `Ortho` (six `double`s), `Frustum` (six `double`s), `TexImage2D`, `TexSubImage2D`, `TexParameteri`, `TexParameterf`, `TexEnvi`, `TexEnvf`, `GenTextures`, `DeleteTextures`, `BindTexture`, `Finish`, `Flush`, `DrawBuffer`, `ReadBuffer`, `ReadPixels`, `GetString`, `GetIntegerv`, `GetFloatv`, `GetError`, `Hint`, `PixelStorei`, `Perspective` (four `double`s; this is `gluPerspective`), `Build2DMipmaps` (returns `int32_t`; this is `gluBuild2DMipmaps`).

`libzzapp` exports those as `glBegin`, `glEnd`, and the rest, plus `gluPerspective` and `gluBuild2DMipmaps`, so Quake's calls do not change. The wrappers live in `libzzapp`, not in Quake. A wrapper whose field is past `gl->size` is a no-op or returns 0. Quake's `glquake.h` includes `<GL/gl.h>`. The port supplies a short `GL/gl.h` and `GL/glu.h` in the Quake tree (GPL, written for the port, token numbers only, no Khronos header text) that declares the same signatures.

Not in the v0 table: quadric and GLUT entry points, `MGLCreateContext`, `MGLSwitchDisplay`, lock-arrays, VBOs, client arrays. The front end still has that code for the 68k library. The ARM table does not point at it. Quake's GL path is immediate mode plus `gluBuild2DMipmaps` and `gluPerspective`.

`GetString(GL_RENDERER)` returns a static `"ZZ9000 RasterIX"`, or `"ZZ9000 CPU"` if the RasterIX probe failed. `GL_VENDOR` is `"ZZ9000"`. `GL_VERSION` is `"1.1"`. Do not return the Amiberry bridge string; that string is the 68k transport's.

App-facing pointers are valid for the process. v0 identity-maps DDR, so a surface pointer happens to equal a card address. Apps must use the pointer the API returned. They must not hardcode `0x00200000` or any other DDR base. User mode will keep the pointer and change the mapping under it.

## 6. GL on ARM

### Files to dual-licence

These files are Dimitris Panokostas, `PolyForm-Noncommercial-1.0.0`, and no other copyright in the file header. Change the SPDX identifier to `PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later`. Do not change behaviour under the 68k build.

| File | Lines (this tree) | Why it is in the set |
|---|---:|---|
| `src/amiberry_gl_state.c` | 2690 | matrix, enable, query, `gluPerspective` |
| `src/amiberry_gl_draw.c` | 1682 | immediate mode |
| `src/amiberry_gl_texture.c` | 1483 | teximage, mipmaps, readback |
| `src/amiberry_gl_light.c` | 754 | lighting setters |
| `src/amiberry_gl_light_get.c` | 210 | lighting getters |
| `src/amiberry_gl_list.c` | 304 | display lists |
| `src/amiberry_gl_buffer.c` | 361 | VBOs; compiled on ARM so the 68k and ARM objects stay one set, even though the v0 table does not export them |
| `src/amiberry_glu.c` | 193 | GLU errors and the quadric-object wrappers |
| `src/amiberry_gl_light.h` | 105 | shared lighting layout |
| `src/amiberry_private.h` | 285 | context struct the slices share |
| `src/minigl_wire.h` | 168 | packet layout the slices emit |
| `src/amiberry_profile.h` | (header) | included by draw and texture; release macros are empty |

New files written for the split carry the same OR identifier from the first line: `mgl_platform.h`, `amiberry_types.h`, `mgl_debug_stub.h`, `amiberry_backend_inproc.c`, `amiberry_transport_inproc.c`.

Do not dual-licence, and do not compile into the firmware:

- `src/amiberry_glu_quad.c` and `src/amiberry_geom.h`. Quadric surfaces are SGI 1991–2000 plus later edits, `PolyForm-Noncommercial-1.0.0 AND SGI-B-2.0`, derived from Mesa GLU `quad.c`. Quake does not call them. The ARM link omits this file. `amiberry_glu.c` quadric functions (`GLUNewQuadric`, `GLUCylinder`, `GLUSphere`, `GLUDisk`) compile to stubs that return NULL / `GLU_INVALID_OPERATION` when `MGL_PLATFORM_ZZARM` or `MGL_PLATFORM_HOST` is set. The 68k build, which does not set those macros, still compiles `amiberry_glu_quad.c` and the real functions.
- `vendor/minigl-framework/**`, including `debug_output.h` (Hyperion MiniGL OR Zlib, Steffen Haeuser). `amiberry_gl_state.c` includes it only for `MGLPrintMatrix`. The ARM build includes `mgl_debug_stub.h` instead. The 68k build includes the vendor header, unchanged.
- `include/libraries/minigl_abi_types.h` (Hyperion MiniGL; types derived from the MiniGL V30 SDK). The ARM build uses `amiberry_types.h`, written for this port: `GLfloat` is `float`, `GLdouble` is `double`, `GLint`/`GLsizei` are 32-bit signed, `GLuint`/`GLenum`/`GLbitfield` are 32-bit unsigned, `GLboolean` is an 8-bit unsigned, `GLcontext` is `uint32_t`. Do not copy the Hyperion header text.
- `include/libraries/minigl_gl_tokens.h` (Khronos, Apache-2.0) may be included by the firmware GL build. Apache-2.0 combines with GPL-3. Do not copy it into the Quake tree. The front end can keep using the numeric tokens it already emits.
- `include/libraries/minigl_glu_tokens.h` (SGI-B-2.0) and `minigl_glut_tokens.h` (MIT, Pawel Olszta / freeglut). Not needed on ARM if quadric and GLUT objects are not linked.
- `amiberry_backend.c`, `amiberry_glut.c`, `amiberry_glut_shapes.c`, `amiberry_transport_zz9000.c`, `amiberry_transport_trap.c`, `amiberry_profile.c`. Amiga or host-emulator code. They stay PolyForm-only.

### Platform header

The named `.c` files today include `<exec/types.h>`, `<exec/memory.h>`, `<proto/exec.h>` and call `AllocMem` / `AllocVec` / `FreeMem` / `FreeVec` / `CopyMem`. They do not call `Forbid`, `Permit`, `CacheClearE`, or `ReadEClock` (the Forbid mention in `amiberry_gl_draw.c` is a comment). Screen, intuition, P96, and DOS calls live in `amiberry_backend.c`, which the ARM build does not compile.

`mgl_platform.h` replaces those includes. The 68k side of the header is macros to the existing Exec calls, so the generated 68k code does not change:

```c
#if !defined(MGL_PLATFORM_ZZARM) && !defined(MGL_PLATFORM_HOST)
#define MGL_PLATFORM_AMIGA 1
#endif

#if defined(MGL_PLATFORM_AMIGA)
#include <exec/types.h>
#include <proto/exec.h>
#define mgl_plat_alloc(n)        AllocMem((n), MEMF_PUBLIC)
#define mgl_plat_alloc_clear(n)  AllocMem((n), MEMF_PUBLIC | MEMF_CLEAR)
#define mgl_plat_alloc_vec(n)    AllocVec((n), MEMF_PUBLIC | MEMF_CLEAR)
#define mgl_plat_free(p, n)      FreeMem((p), (n))
#define mgl_plat_free_vec(p)     FreeVec(p)
#define mgl_plat_copy(d, s, n)   CopyMem((APTR)(s), (APTR)(d), (n))
#else
#include <stdint.h>
#include "amiberry_types.h"
void *mgl_plat_alloc(uint32_t n);
void *mgl_plat_alloc_clear(uint32_t n);
void *mgl_plat_alloc_vec(uint32_t n);
void  mgl_plat_free(void *p, uint32_t n);
void  mgl_plat_free_vec(void *p);
void  mgl_plat_copy(void *d, const void *s, uint32_t n);
#endif
```

ARM `mgl_plat_alloc*` allocates from the app GL context's existing card-pool owner (`CARD_POOL_CLASS_MINIGL`), the same owner `mw_ctx` uses, so context destroy frees front-end scratch too. Host builds malloc. `Forbid`/`Permit` are not wrapped because nothing in the slice calls them. If a later edit needs a critical section, it goes through `mgl_plat_lock` / `mgl_plat_unlock` added to this header, not a raw `Forbid` in a slice file. Cache maintenance of packet bytes is the transport's job. On ARM the transport is in-process and does not call `CacheClearE`.

`amiberry_private.h` includes `amiberry_backend.h` and `<graphics/rastport.h>`. Wrap those includes and the `Window` / `BitMap` / rastport fields of `struct AmiberryContext` in `MGL_PLATFORM_AMIGA`. The default macro above means a 68k build that does not pass a new flag keeps today's struct layout. Do not reorder fields that remain in the 68k struct. ARM code uses the context only through the accessors already in the header (`amiberry_context`, `amiberry_command`, `amiberry_flush`, `amiberry_read`, `amiberry_error`).

Acceptance for the 68k side: the library's existing test suite, built without `MGL_PLATFORM_ZZARM`, stays green. No flag change in the 68k makefile.

### In-process transport

`struct MiniGLTransport` (`src/amiberry_transport.h`) is the seam the backend already calls: `query`, `create`, `submit`, `readback`, `readback_rows`, `destroy`, `request`, `blit`, `queue_present`, `sync`, `term`. The 68k card transport (`amiberry_transport_zz9000.c`) implements it with a host-visible slot window, a doorbell, `CacheClearE`, and fence polls against `completion_fence`. The ARM transport implements the same struct and is compiled only when `MGL_PLATFORM_ZZARM` or `MGL_PLATFORM_HOST` is set.

- `submit` calls `mw_host_submit(ctx, packet, bytes)` (`mw_host.h`). The call is synchronous. There is no fence poll and no Zorro window. Return the status `mw_host_submit` returns. Packet cap remains `MW_MAX_PACKET_BYTES` (256 KiB, `mw_types.h`).
- `request` and readback call `mw_host_request` and `mw_host_readback_rgba8` / `mw_host_readback_latest_rows`.
- `create` calls `mw_host_create` (the existing context allocator). The color and depth surfaces stay the context-owned card-pool buffers `mw_ctx` already allocates (`CARD_POOL_CLASS_MINIGL`). RasterIX renders into those, not into the P96 bitmap. `minigl_rix_backend_begin_draw` takes a color address, and today that address is the context buffer. v0 does not retarget RasterIX at an arbitrary P96 address. The present step copies.
- `queue_present` / `blit` are the swap. `zz_gl_swap` drains (`minigl_raster_finish` / `minigl_raster_prepare_present`) and copies the context target into the bound P96 buffer with the existing present copy (`minigl_present_copy_rgb565` when the target is RGB565, `minigl_present_copy` otherwise). That copy is what `mw_blit` already does for a 68k `MGLSwitchDisplay`, aimed at `card_offset + ADDR_ADJ`. Then `display_flip` publishes `flip_gen`. No new wire opcode.
- `sync` is a drain.

Host builds (`MGL_PLATFORM_HOST`, `MINIGL_HOST_TEST`) use the same transport against the host-compiled `mw_*` objects. `test/minigl_v9/Makefile` already builds `mw_service.o`, `minigl_raster.o`, `minigl_rix_backend.o` and the library front-end objects together under `-DMINIGL_HOST_TEST`. The in-proc transport replaces `amiberry_transport_zz9000.c` in that link. It does not replace the service.

`amiberry_backend_inproc.c` is the thin context owner: one `AmiberryContext`, the transport pointer, `amiberry_command` / `amiberry_flush` / `amiberry_read` / `amiberry_error`. It does not open intuition, graphics, or P96. The 68k `amiberry_backend.c` is untouched and is not in the ARM link.

Wire protocol stays v9. The front end still emits `MW_*` packets. The firmware parser (`mw_parse.c`, `mw_service.c`) does not grow an app-specific opcode. 68k clients keep the slot window, the doorbell, and the fence. The app does not use that window.

Dimension cap remains `MW_MAX_DIMENSION` 4096 (`mw_types.h`). Reject a display bind above that.

## 7. Repositories and build

| Piece | Where |
|---|---|
| `zzapp.h`, `libzzapp`, crt0, `zzapp.ld`, templates `hello`, `pixels`, and `fault` | firmware repo `sdk/zzapp/`, MIT, ships with the SDK |
| `zzapp_proto.h` | firmware `ZZ9000OS/src/zzapp_proto.h`, GPL. Drivers build picks it up through the existing header stage (`zz9000-drivers/tools/stage-zz9k-headers.sh`), not a second copy maintained by hand |
| Runtime, loader, faults, rings, mailbox ops | firmware `ZZ9000OS/src/zzapp_*.c`, GPL |
| GL front end | stays in `amiberry-minigl-library-zz9k`. Firmware compiles the listed `.c` files with `-DMGL_PLATFORM_ZZARM` and `-I` that tree. Do not vendor-copy them into the firmware tree |
| `ZZRun` | `zz9000-drivers/ZZRun/`, same shape as `ZZCapture/` |
| Quake | separate local repo, not a firmware subdirectory and not a firmware submodule. Not published in this slice |

Firmware GL build pin: the library commit slice C was written against, recorded in the firmware makefile as `MINIGL_LIB_DIR` (default `../amiberry-minigl-library-zz9k`) and a one-line commit id in that makefile's comment. No git submodule in v0.

App link, same toolchain the firmware uses (`build_firmware.sh` / the firmware Docker ARM toolchain, official Arm GNU with newlib, not picolibc):

```text
-mcpu=cortex-a9 -marm -mfpu=neon -mfloat-abi=hard -fPIE -pie
-nostdlib -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections
-Wl,-T,sdk/zzapp/zzapp.ld -Wl,--gc-sections
```

`-fno-strict-aliasing` because id's sources alias. No `-ffast-math`. newlib is linked into the app (`-lc -lgcc -lm`), with `_sbrk` from `libzzapp`. Not the firmware's libc.

crt0 (`sdk/zzapp/crt0.S`): save `r0` as the API pointer, set up no vectors, call `zz_main`, branch to `exit` with the return value.

Linker script: PHDRs for RX and RW, `.rela.dyn` kept and allocated so the loader can see `R_ARM_RELATIVE`, note `ZZAPP`, entry `zz_start`. No fixed `ORIGIN`. Vaddrs are offsets from 0; the loader adds the arena base.

68k launcher: container `sacredbanana/amiga-compiler:m68k-amigaos`, never a gcc10 image.

Templates, each a few dozen lines, MIT: `hello` logs a line and exits; `pixels` opens 320×240 RGB565, fills the back buffer, flips until `ZZ_EV_STOP` or a key; `fault` logs then deliberately stores to address zero for the fault-path hardware check. `pixels` draws the CPU mapping from `zz_display_open`. It does not go through GL. That is how the kit separates a bad present-copy from a bad launcher screen.

## 8. Quake port

Source pin: id Software's Quake, `WinQuake/` at `https://github.com/id-Software/Quake` (GPL-2.0-or-later). The GL objects in `WinQuake/WinQuake.dsp` are the build list. Do not import the PiStorm3D or MiniGL_Library Quake archives as the source base. Those `.lha` files contain a full Amiga GLQuake tree (GPL text, later authors, 68k asm, Hyperion MiniGL calls) plus a `glquake` binary, and no pak. They are a reference for rawkey values if a mapping is unclear, not files to copy. `q1.zip` next to them contains shareware `id1/pak0.pak` (18,689,235 bytes), `id1/glquake/*.ms2`, and `quake.exe`. The port does not ship pak files. The user points `ROOT` at their own `id1/` directory. Shareware `pak0.pak` is enough to reach `demo1`.

Compile these `WinQuake` files, with `GLQUAKE` defined and `id386` not defined (C math, no x86 asm):

`chase.c`, `cl_demo.c`, `cl_input.c`, `cl_main.c`, `cl_parse.c`, `cl_tent.c`, `cmd.c`, `common.c`, `console.c`, `crc.c`, `cvar.c`, `host.c`, `host_cmd.c`, `keys.c`, `mathlib.c`, `menu.c`, `gl_draw.c`, `gl_mesh.c`, `gl_model.c`, `gl_refrag.c`, `gl_rlight.c`, `gl_rmain.c`, `gl_rmisc.c`, `gl_rsurf.c`, `gl_screen.c`, `gl_warp.c`, `pr_cmds.c`, `pr_edict.c`, `pr_exec.c`, `r_part.c`, `sbar.c`, `sv_main.c`, `sv_move.c`, `sv_phys.c`, `sv_user.c`, `view.c`, `wad.c`, `world.c`, `zone.c`.

Do not compile the software renderer (`d_*.c`, `r_aclip.c`, `r_alias.c`, `r_bsp.c`, `r_draw.c`, `r_edge.c`, `r_efrag.c`, `r_light.c`, `r_main.c`, `r_misc.c`, `r_sky.c`, `r_sprite.c`, `r_surf.c`, `r_vars.c`, `draw.c`, `screen.c`, `model.c`), `gl_test.c`, `conproc.c`, `vid_win.c`, `sys_win.c`, `in_win.c`, `snd_win.c`, `cd_win.c`, `net_win.c`, `net_wins.c`, `net_wipx.c`, `net_vcr.c`.

Platform files, new, GPL-2.0-or-later, in the Quake repo:

| File | Replaces | Calls |
|---|---|---|
| `sys_zzapp.c` | `sys_win.c` | `zz_file_*` for `Sys_FileOpenRead` / `OpenWrite` / `Read` / `Write` / `Seek` / `Close` / `FileTime` (`sys_win.c` lines 134, 163, 183, 193, 202, 212); `zz_time_us` for `Sys_FloatTime` (line 467); `zz_log` for `Sys_Printf`; `zz_exit` for `Sys_Quit` / `Sys_Error`; `zz_should_stop` in the main loop. `Sys_mkdir` succeeds if the directory exists. `Sys_MakeCodeWriteable` returns |
| `gl_vidzz.c` | `gl_vidnt.c` | `zz_display_open(640, 480, ZZ_FMT_RGB565, vsync)` in `VID_Init`; `GL_EndRendering` (line 674) calls `zz_gl_swap` instead of `SwapBuffers` (line 677). No DIB, no window class |
| `in_zz.c` | `in_win.c` | `zz_input_poll`; map Amiga rawkeys to Quake `Key_*` values; mouse `dx`/`dy` into `IN_Move` |
| `snd_zz.c` | `snd_win.c` | `SNDDMA_Init` (the WinQuake one is at `snd_win.c` line 557) calls `zz_audio_open(22050, 2, ...)`, sets `shm->speed` to the granted rate, `shm->channels` 2, `shm->samplebits` 16, `shm->buffer` at the S16 ring, `shm->samples` = frames × channels. `SNDDMA_GetDMAPos` (line 635) reads `*read_frame`. `SNDDMA_Submit` is a yield. On `ZZ_EBUSY`, fail init and let the engine run silent |
| `cd_null.c` | `cd_win.c` | id's null CD stubs, or a short equivalent. No CD audio in v0 |
| `net_none.c` | `net_win.c` / `net_wipx.c` | loopback plus a null driver so `-noudp` is the default. No sockets |

`net_loop.c` stays. If `net_dgrm.c` pulls in WinSock types, do not compile it; provide the symbols the loop driver needs from `net_none.c`. Do not port UDP in v0.

Defines: `GLQUAKE`, no `id386`, no `_WIN32`, no `AMIGA`. The last one is so the port does not pick up an Amiga `sys_amiga.c` path. Float and NEON come from the app link line in section 7. Do not rewrite `mathlib.c` in NEON for v0.

`Sys_FileOpenRead("id1/pak0.pak")` is the access pattern: a few large files, seek to a lump, read the lump. The firmware loops 256 KiB chunks. The port does not special-case that.

Performance. The reason this slice exists is that GLQuake's own game code was measured at 105–135 ms/frame on the 68060 (maintainer note, 2026-10-31). The card CPU is 666.67 MHz with a VFP/NEON unit. This contract does not set an fps gate. The hardware check is `timedemo demo1` with the user's pak, frame time recorded. Expect the 68k game-loop cost to move off the 68k. Do not publish a number until that timedemo has been run. If the hunk OOMs at 96 MiB, that is a quota bug to report, not a silent bump.

Save games: `file_write` is enough for `id1/*.sav`. If a save returns an error, surface it. Do not pretend it worked.

## 9. Tests without hardware

Follow `test/minigl_v9/Makefile`: host `cc`, `-DMINIGL_HOST_TEST`, firmware `mw_*.c` + `minigl_raster.c` + `minigl_lighting.c` + `minigl_present.c` + `minigl_rix_backend.c` and the rix objects, plus the library front end compiled `-DMGL_PLATFORM_HOST`. That makefile already compiles those firmware objects into `libmw_host.a` and the front-end slices as client objects against `mock_amiga/`. The app harness adds the in-proc transport in place of `amiberry_transport_zz9000.c`.

`test/zzapp/` (firmware, GPL test driver):

1. Loader rejects a bad ELF (wrong relocation, RWX segment, major 2, image past 16 MiB) without jumping.
2. A tiny app image, linked with the real crt0, runs `hello` against a mocked launcher window: log line appears, exit code comes back, generation bumps on relaunch, a stale flip ack is ignored.
3. Forced data abort at an app PC stops the app, fills the fault block, and leaves a second mocked 68k MiniGL context usable.
4. Watchdog: an app that never yields is force-stopped without a real private timer (the test calls the tick with a faked time).
5. File seqlock: one chunked read of 300 KiB assembles in order; a generation bump mid-read returns `ZZ_EIO`.
6. GL: a 16-triangle app binds a 64×64 RGB565 surface, draws, swaps, and the host readback of the context target, then the present-copy into the mocked P96 buffer, sees non-background pixels. This links the real front end and the real `mw_service`, not a mock GL.

ARM build check, no board: firmware ELF links with the front-end objects and `zzapp_*.c`; `hello` and `pixels` link with the app toolchain and `readelf` shows only `R_ARM_RELATIVE`. Host tests are the functional bar. Do not require qemu.

68k: library suite green without the new macros defined. ZZRun compiles in the drivers container. The protocol test is on the firmware side with a mocked 68k writer.

### First hardware kit

On a Z3 machine, in order, stop on the first failure:

1. `hello`: UART or ZZRun shows the log line, Shell exit code matches, a second launch works, Workbench and an ordinary SDK call still work.
2. `pixels`: a P96 screen shows the pattern, a key or Ctrl-C returns to the public screen, the card does not need a reboot.
3. Fault: a debug app that stores to `0` reports pc/far and returns to the Shell. Then an ordinary SDK call works. 68k MiniGL, if a client is up, either still answers or has been reset only if the backstop cold-restart fired. The report must say which.
4. Quake: `timedemo demo1` from the user's `id1/pak0.pak`, screen restores on quit, audio either plays or stays silent because the fabric slots were busy (record which). No pak is copied onto the card's SD.

Zorro II is not in the kit. Confirm only that `APP_QUERY` on a Z2 board returns unsupported and that existing Z2 tools still run.

## 10. Slices

Shared text lives in this document. Nobody invents a second layout. Firmware file split, so two slices are not editing one `.c`:

| Slice | Owns | Does not touch |
|---|---|---|
| A runtime core | `zzapp_loader.c`, `zzapp_host.c` (stack switch, timer, fault classify, watchdog), `zzapp_api.c` (non-GL slots), `zzapp_runtime.h`, `card_pool.h` class 8, fault classify in `core2.c`, worker hook in `scheduler_arm.c`, reset hook at the start of `minigl_begin_amiga_reset` | `sdk_mailbox.c`, GL front end, drivers, Quake |
| B services | `zzapp_proto.h` (byte-for-byte with section 4), `zzapp_services.c` (rings, files, input, log, display bind, audio attach), `SDK_OP_APP_*` in `sdk_mailbox.c` / `sdk_mailbox.h`, the same ids in `sdk/include/zz9k/abi.h` and `sdk/host/include/zz9k/reply.h` | loader, `core2.c`, `scheduler_arm.c` except the weak `zzapp_services_poll` that A calls and B defines |
| C GL | library licence lines, `mgl_platform.h`, types, in-proc backend and transport, firmware `zzapp_gl.c` that fills `zzapp_gl` and performs the present-copy into the bound surface | mailbox ops, scheduler, Quake |
| D ZZRun | `zz9000-drivers/ZZRun/` only | firmware sources; consumes the staged `zzapp_proto.h` |
| E libzzapp | `sdk/zzapp/**` MIT, `test/zzapp/` harness | `sdk_mailbox.c`, drivers |
| F Quake | the separate Quake repo | every firmware and library file |

Weak seams, so A links before B and C land:

- `void zzapp_services_poll(void)` — B's tick body. A's copy is an empty weak symbol.
- `int zzapp_gl_bind(uint32_t surface_id, const zzapp_gl **out)`, `void zzapp_gl_swap(void)`, `void zzapp_gl_unbind(void)` — C defines them. A's weak copies return `ZZ_ENOSYS` / NULL.

A's `zzapp_api.c` calls those. It does not contain GL or file-protocol code.

C, D, E, and F start from this document without waiting. A and B start together: B adds `zzapp_proto.h` exactly as section 4, A includes it and does not edit it. F compiles once E has published `zzapp.h` (the text in section 5 is that header). The hardware kit waits for all six.

Slice acceptance:

- A: host tests 1–4 in section 9, plus an ARM link. Fault in the app does not call `core1_cold_restart`. Fault in a firmware PC still does.
- B: host test of the seqlock, chunked read, drop-on-full input ring, audio-open when slot 2 is busy returns `ZZ_EBUSY`. Mailbox ops fit in 48 bytes.
- C: 68k library suite green; host GL test 6 green; firmware link of the front end does not pull `amiberry_glu_quad.c`, `amiberry_backend.c`, or `vendor/`.
- D: builds in the gcc 6.5 container; posts the ops in section 4; restores the screen in the hardware kit.
- E: `hello` and `pixels` link as PIE with only `R_ARM_RELATIVE`; wrappers call the table.
- F: `timedemo demo1` links and, on hardware, runs from a user-supplied pak. No id data in the repo.

## 11. Risks

- Trusted code can corrupt firmware data without aborting. `.text` write-protect only closes stores to code. User mode is the real fix and is not in v0.
- A privileged app can mask IRQs. The cold restart then fires, and 68k MiniGL clients die with it, as they do on any core-1 restart today.
- A long GL call stalls 68k MiniGL until it returns. Logging at 50 ms is the only mitigation in v0.
- The card-pool caps already sum to more than the permanent prefix (MiniGL ceiling alone is 384 MiB, `minigl_pool.h`). A 96 MiB app plus a large GL context can fail an allocation that looks fine on paper. Fail closed. Do not borrow the Fast RAM tail.
- Every frame copies the RasterIX target into the P96 bitmap. 640x480 RGB565 is about 600 KiB. A 32-bit screen is BGRA8888 (`SDK_SURFACE_FORMAT_BGRA8888`, 7) and is not the first kit.
- Audio is best-effort if both lease slots are taken. 11025 is not a fabric-producer rate.
- Dual-licensing the front end requires the copyright holder's actual grant before the firmware link. Until that lands, slice C stops at the library edits and the host build.
- The function-table boundary is the project's rule for closed apps. It has not been reviewed as a GPL linking opinion. No closed app ships on v0.
- `docs/memory-map.md` disagrees with `memorymap.h`. Slices that copy an address out of that doc will place the arena on top of a module slot.

## 12. Decisions worth revisiting

These are choices this contract makes so the slices do not each make them. They are not unfinished design.

1. PIE with `R_ARM_RELATIVE` only, arena base chosen by the card pool, rather than a fixed link address. The pool will not hand out a reserved VA, and user mode should keep the VAs the relocator wrote. A fixed address would be less loader code and was the alternative.
2. 96 MiB, permanent prefix only. The old pool-size figures assumed the 16 MiB arena `docs/memory-map.md` still describes. The header's permanent prefix starts at `0x0B000000`.
3. Service tick from the private timer, plus cooperative yields, rather than making Quake return every frame. Cold restart only if IRQs never arrive. Full preemptive app slicing waits for user mode.
4. 68k MiniGL stays queued and is drained between app GL calls, rather than being suspended for the whole Quake session.
5. App audio uses a free fabric slot or runs silent. A fourth slot is a fabric change (ISR budget, lease plane) and is not in v0.
6. App audio is an internal fabric producer and is granted 22050, because `fabric_rate_known` and the conversion table already have it. The mailbox acquire path does not. Routing the app through `SDK_OP_AUDIO_RING_ACQUIRE` would force 24000 and was rejected.
7. Present is a copy from the context-owned RasterIX target into the P96 bitmap, then a launcher flip ack. Firmware does not allocate P96 VRAM, does not point RasterIX at the scanout bitmap, and does not write the RTG scanout register behind the driver.
8. GL sources stay in the library repo and are compiled into the firmware. They are not vendored and not a submodule in v0.
9. Quake is ported from id's `WinQuake`, not from the PiStorm tree that already has Amiga input. The PiStorm tree has a wider licence inventory than this slice should take on.
10. Rings are big-endian, like `ZZ9KMailboxWireEntry`. A little-endian app-only layout would be a second convention on the same bus.
