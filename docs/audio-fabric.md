# ZZ9000AX audio fabric admitted budget and the measurement session

The audio fabric compositor (`audio_fabric.c`, plans U2/U3) mixes
bounded producers into the formatter TX ring from the audio period
interrupt. Admitting producers is a cycle-budget decision, and that
decision comes from measurement, not assumption. This document defines
the admitted-budget model, the instrument build that measures it, the
bench method for the hardware session, the results table (not yet
measured), the smoke checklist for that session, and the runbook that
lands the capability advertising after a recorded pass.

Host tests in `test/audio` enforce the compositor's mix and lease
semantics; nothing on this page is measured until the hardware session
records it.

## Reading "done" correctly (consumer honesty)

The fabric capability, once advertised, has **no shipping consumer**:
AHI, MHI and ZZTop do not touch the lease plane until the AHI
migration follow-on lands. Until then the proof client
`zz9k-fabriclease` (SDK `tools/zz9k-fabriclease.c`) is the only
exerciser of the opcodes. A recorded pass on this page means *the
budget is measured, the plane is qualified on hardware, and the
advertising is shipped in a matched set* — not that any shipping
application can yet mix a second producer.

## The admitted-budget model

The compositor runs once per audio formatter period: 20 ms, which is
13.33M CPU cycles at the card's 666.67 MHz core clock. Everything the
ISR does must fit inside that window with margin, concurrently with
whatever core 0 was doing when the interrupt landed.

What the instrument build measures (one accumulator per stage, values
in global-timer ticks — 2 ticks = 1 CPU cycle, see the method section):

| Stage | Spans |
|---|---|
| `isr` | One active compositor tick: frontier check through tail tracking, while at least one producer is live |
| `fill[n]` | One whole `fabric_slot_fill` pass on slot n: ring pull, cache invalidate, byte-swap, metering, mono expand, gain |
| `conv[n]` | The rate-conversion pass inside slot n's fill (polyphase resample only) |
| `mix` | Mix zero + per-slot accumulate + saturating commit + the per-period TX cache flush |

Analytic baseline, from the qualified conversion kernel
(`docs/audio-conversion.md`): worst-case playback conversion is the
densest ratio, 44.1 kHz → 48 kHz (101 taps, 160 phases), at 193,920
MAC per 20-ms period. One SMLAL per tap (1–2 cycles with loads) puts
a single conversion-bearing producer near 0.19–0.39M cycles, about
1.5–3% of the window. The conversion document's full-duplex worst
case — 2,521,800 MAC (6 pump + 7 capture periods in one window),
bounded near 2.5–5M cycles — is 19–38% of the window and remains the
analytic ceiling context. All of these are host-derived estimates
until this session's on-hardware rows replace them; per-ISR
conversion capping stays the documented fallback if measurement ever
fails to argue margin.

Admission decision rule: the default admitted budget is **2
conversion-bearing producers + 1 bypass producer** across the three
slots, revised down if the measured rows argue it. Justification: the
fill work is bounded per producer by construction (bounded ring walk,
per-slot fill capped at one 48-kHz period), two worst-case conversion
passes sit near 0.4–0.8M cycles (3–6% of the window) on top of mix and
cache costs an order of magnitude below the window, and the bypass
path is a period-sized copy. The bench rows below measure the
constructible subset of that budget directly; the remainder is a
linear extrapolation over identical per-slot work.

## Measurement method (bench session)

Single session on target hardware, on the operator's own card, in the
style of the saturation-ceiling session
(`docs/audio-saturation-ceiling.md`).

**Instrument build.** No `instrument` image target exists; the mechanism is the firmware Makefile's `EXTRA_CFLAGS` seam plus compile-time guards in `audio_fabric.c`:

    make -C ZZ9000_proto.sdk/ZZ9000OS \
        EXTRA_CFLAGS='-DAUDIO_FABRIC_BENCH'

Package with `./build_bootimage.sh` and record the BOOT.bin SHA-256 like the ceiling session did. Both Zorro III direct-ring slots are natively grantable; the former three-slot admission flag no longer exists. With the benchmark flag off, every instrumentation hook compiles away.

**Report.** `main.c` polls `audio_fabric_bench_poll()` each main-loop
pass; the ISR never prints. One block per second on the firmware
serial console:

    FABRIC-BENCH hz=333333332 ticks (2 ticks = 1 CPU cycle)
    FABRIC-BENCH isr avg=<t> peak=<t> n=<calls>
    FABRIC-BENCH mix avg=<t> peak=<t> n=<calls>
    FABRIC-BENCH slot0 fill avg=<t> peak=<t> n=<calls>
    FABRIC-BENCH slot0 conv avg=<t> peak=<t> n=<calls>
    ... slot1, slot2 ...

`hz` is `COUNTS_PER_SECOND` (the ARM global timer, half the core
clock): **cycles = 2 x ticks**. `avg` is cumulative ticks/calls,
`peak` the worst single pass since boot, `n` the accumulated pass
count. Each measured region includes its own two timer reads (tens of
ns), which is why per-stage numbers are read against the whole-tick
`isr` row rather than summed blindly.

**Representative load profile.** Every row runs with core-1 MP3 decode
active *and* RTG traffic active — the contention case the compositor
exists for (heavy main-loop passes must not let the DMA overrun the TX
frontier):

- Core-1 decode: ZZPlay or MHI playing an MP3 throughout the
  measurement (the pump producer's decoder runs as a core-1 task).
- RTG traffic: continuously drag a full-screen Workbench window (or
  run any full-screen RTG animation), forcing sustained RTG blits
  through the card while measuring.

**Producer configurations.** One deviation from the plan's first
sketch, forced by the ABI: the lease plane's geometry is fixed bypass
— 48 kHz stereo S16LE, `flags` required zero, the lease source pinned
to 48000 — so a lease can never be the conversion-bearing producer.
The conversion-bearing producer is therefore always the pump (an SDK
playback binding at a non-48-kHz rate), and the conversion rows pair
it with bypass leases:

| Row | Producers | How to produce it |
|---|---|---|
| B1 | Pump alone, 48 kHz bypass | ZZPlay/MHI playing a 48 kHz stereo MP3 |
| B2 | Pump alone, 44.1 kHz worst-case conversion | ZZPlay/MHI playing a 44.1 kHz stereo MP3 (densest ratio, 160 phases) |
| B3 | Pump 44.1 kHz + one bypass lease | B2 plus `zz9k-fabriclease` playing its generated 48 kHz tone |
| B4 | Pump 44.1 kHz + two direct-ring producers (three sources) | The normal benchmark build; two independent `zz9k-fabriclease` instances acquire slots 1 and 2 |

The three-source row measures the production admission path directly. Slot 0 remains the pump and the two Zorro III direct-ring slots are native peers. No instrument-only admission override is involved.
What that build cannot construct is the two-conversion-bearing case
(only the pump converts, through any ABI), so that half of the
admission rule stays extrapolated: measured `slot0 conv` doubled over
identical per-slot converter instances, which the bounded per-slot
fill makes linear by construction.

**Procedure per row:** start the standing load plus the row's
producers, wait at least 30 s of steady state, transcribe the report
block, and read the underrun counters (state read below) — they must
be stable (0 for healthy rows). Record the firmware/image SHA-256
with the results.

## Results — recorded PASS 2026-08-28 (operator session, PAL A4000 / R1 card)

Firmware: `feat/audio-fabric-compositor` commit `5e9bed8`; instrument image
`BOOT-audio-bench.bin` SHA-256 `9b39eab1…1c28`; stock candidate
`BOOT.bin` = `BOOT-audio-final-fixed.bin` SHA-256 `e89c387c…39f9`. All rows
run with the representative load profile (core-1 MP3 decode + full-screen
RTG drag). The operator confirmed PASS for every row; the per-stage numeric
report blocks were not retained, so the cells below record the qualitative
result and the underrun observation rather than transcribed timings.

| Row | Configuration | Result | Underruns |
|---|---|---|---|
| B1 | pump 48 kHz bypass | PASS (user-confirmed 2026-08-28) | 0, stable |
| B2 | pump 44.1 kHz conversion | PASS (user-confirmed 2026-08-28) | 0, stable |
| B3 | B2 + 1 bypass lease | PASS (user-confirmed 2026-08-28) | 0, stable |
| B4 | B2 + 2 direct-ring producers | PASS (user-confirmed 2026-08-28) | 0, stable |

Admission-rule verdict: the default **2 conversion-bearing + 1 bypass**
budget stands — every constructible row passed with stable counters and no
audible defects, so nothing argues a revision. The per-stage timing cells
were not transcribed; if a future session re-measures, replace the
qualitative rows with the numeric avg/peak blocks and re-affirm the verdict.

Sustained two-stream listening under RTG/network load on the stock candidate
`BOOT.bin` also passed (user-confirmed 2026-08-28): zero audible drops, the
fix state that closes the two-client drop investigation (handover Unit 29).

## Hardware smoke checklist (verification session) — recorded PASS 2026-08-28

Run on the instrument build `BOOT-audio-bench.bin` (SHA-256 `9b39eab1…1c28`,
firmware `5e9bed8`) during the same operator session as the B rows. Every
item below passed; observations recorded from the operator's confirmation.

| # | Item | Steps | Expected observation |
|---|---|---|---|
| S1 | Coexistence audible check | Start ZZPlay/MHI playing an MP3, then run `zz9k-fabriclease` alongside it | Both sources audible simultaneously, mixed without dropouts or level discontinuities |
| S2 | Underrun counters stable | During S1 and each bench row, poll the per-slot state (`ZZ9K_OP_AUDIO_FABRIC_STATE_GET`) | Underrun counters for active slots stay 0 (or saturate only on deliberately starved producers); other slots' counters unaffected |
| S3 | Ghost-period listen check | Rapidly cycle the lease: begin, feed briefly, release, repeat while listening | At most one residual period per release (the KTD3 ghost bound): no stuck tone, no stale audio, output returns to pump-only cleanly |
| S4 | Warm reset during two producers | With pump + lease active and audible, warm-reset (Ctrl-Amiga-Amiga) | Fail-closed teardown: no stale audio after reboot, TX ring silent until a new producer starts, pre-reset lease handles never validate |
| S5 | State read, ZZTop-independent | Drive every state read through the proof client / SDK opcodes, no ZZTop involvement | Framed per-slot snapshot self-consistent (state, cursors, underruns, 16.16 peak, clips); `HOLD_RESET` consumes the peak window |

All five items observed PASS by the operator on 2026-08-28: coexistence
clean (S1), counters stable at 0 across the session (S2), ghost bound held
on rapid cycling (S3), fail-closed warm reset (S4), framed snapshots
self-consistent through the proof client (S5).

## Post-pass runbook

On a recorded pass — results transcribed into the table above, the
S-list observations recorded, the admission-rule verdict written —
the advertising flip lands as the matched-set sequence from the
saturation-ceiling doc's coordination section:

1. **Firmware flip commit:** add `SDK_CAP_AUDIO_FABRIC` (bit 27) to
   the audio service word and the global capability set in the
   `sdk_mailbox.c` service table, set
   `SDK_SERVICE_FLAG_AUDIO_FABRIC` (bit 22), and extend the audio
   service's opcode count from 15 to 19 (`0x0500..0x0512`).
2. **SDK flip:** register the fabric entries in the `zz9k-services`
   release tables (`tools/zz9k-services.c`, the `--check-release`
   expectations) and replace the gated-advertising wording in
   `docs/zz9k-library.md`.
3. **Tag order** (drivers' `RELEASING.md` dependency order): SDK
   first (merge + tag + push), then the drivers' `SDK_REF` bump to
   that release commit, then firmware, then drivers; firmware and
   drivers carry the same `vX.Y.Z`.
4. Update this file's results table with the measured rows and the
   firmware/image SHAs, and re-read the honesty section above: the
   advertised capability still waits on the AHI migration follow-on
   for its first shipping consumer.
