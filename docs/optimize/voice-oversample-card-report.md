# 2x voice rendering: card optimization

2026-11-24. Kernel `5adc3a66e13af9d3d2083828c044e741d6d31aae` retained on `optimize/voice-oversample-card`. This completes the rendering optimization, not product U5 integration. Oversampling is still unadvertised; the measured feature word is `0x8f`.

## Confirmed result

Fresh original-to-final comparison on the A4000/68060, ZZ9000 Zorro III. Each row uses one 20-second protocol sample per image, 1016 render periods and 999 steady full-pool periods.

| Required objective | Original peak, us | Final peak, us | Change, us | Reduction | Requirement |
|---|---:|---:|---:|---:|---|
| 32 stereo voices + 32 tails, linear 2x | 6237.95 | 4922.74 | -1315.21 | 21.08% | At most 5000 us: met |
| Same workload at 48 kHz, without 2x | 3342.36 | 2692.91 | -649.45 | 19.43% | No regression over 2%: met |

The comparison threshold is 2% relative; it is not a statistical confidence interval. These are observed maxima, not a bound for unmeasured workloads. Headroom below 5 ms is only 77.26 us (1.55%). All late, rejected, underrun, dropped and ring-full counters were zero. The 2x output delay remained 14 frames, and each client timeline covered 960000 frames.

The command was:

```text
Stack 131072
Work:zz/bench/oversample/zz9k-voiceplay LOAD VOICES=32 SECONDS=20 STEREO RATE=96000 RAMP=256 GAIN=65535 CHURN GROUPS=32 LIMITER INTERP=linear OVERSAMPLE
```

The control removes only `OVERSAMPLE`. No other player was launched by this run. Speakers were off; there is no listening result.

## Measured identities

Product reference: `34e76e65a602293330c8389e954dd815e620173a`. The benchmark images add the same media-ownership fix and seven fixed instrumentation commits, with the original 256-idle-pass polling policy and no 250 us deadline. Those benchmark-only changes are not part of the retained kernel diff.

| Image | Bench commit | BOOT.bin SHA-256 |
|---|---|---|
| Original | `6bb378f0e18190190ebf80e85dd2d8589b797bfe` | `3ffffa79a194bfae024108ae9326d331d4518b1bf584915a81d37aee4c06ccf6` |
| Final | `56a4139d04b803f92cb57e2dfb57e486f0562dec` | `14a4506ae8bd5223b5420e6abad35d160629960c74ff6eeb3c193ca55d697421` |

Both transfers were read back and SHA-verified. Full power cycles and UART build identities verified activation. The final candidate image was unchanged between selection and confirmation. Its `audio_voice.c` SHA-256 is `910a1aab2bdc1e1555dceeb5e1638507125729aa7b081111f046bb9334bf979f`.

## Opportunity and implementation

The retained change combines E1's exact adjacent-tap loads and persistent fractional vectors with E2's 8-bit structure-lane loads. Only `audio_voice.c` changed. Scalar math, sample rates, formats, coefficients, gain, limiter, final clamp and client records were not altered. Pair loads read exactly two adjacent frames; backward play swaps the lower/higher results. The caller passes the current-frame pointer, and the backward safe-span invariant keeps its preceding partner inside the sample even for a short NEON call.

E1's original primary forecast was 4.5–5.2 ms. It measured 4.815 ms for S16 stereo but missed the required all-format budget: S8 stereo took 5.368 ms. E1 was therefore not selected on its own.

E2 forecast a further 5–13% reduction of the S8 stereo path against E1. Its selection maximum fell from 5367.64 to 4920.95 us, an 8.32% reduction, within that range. This is an S8-specific comparison; it must not be added to the overall primary gain. The final combined primary result also fits the original 4.5–5.2 ms forecast. No separate final incremental S16 gain is claimed for E2.

The S8 stereo loop lost four table shuffles and two moves. Constants did not hoist as hoped. The S16 source was unchanged from E1, but GCC changed allocation/scheduling: its shorter loop nevertheless measured about 2.2% slower than unkept E1. The expected assembly-identity check failed; actual S16 primary/control measurements, not E1's timings or instruction count, established the unchanged required performance constraints.

## Additional supported-path qualification

These selection measurements used the same final image, 20 seconds per case, 1016 render periods and clean counters. S16 stereo linear is covered by the primary comparison above. The private 8-bit tool used SHA-256 `acf8e473c30d1c1abf09676272a0202893adb13155380e187434d329af1e3f45`; the normal tool used `6e568d4e0f2a375c99c5d594ea43817e75f0e69d2720d86464cab55f08c4df6e`.

| Additional case | Peak render, us |
|---|---:|
| S8 stereo, linear 2x | 4920.95 |
| S8 mono, linear 2x | 3946.61 |
| S16 mono, linear 2x | 4281.11 |
| S16 stereo, nearest 2x | 3621.39 |
| S16 stereo, linear 2x without limiter | 4823.43 |

## Reserved-workload confirmation

Collected only after E2 was eligible on the selection pair. The same original and final images were cold-booted for these cohorts. There were 1016 periods per standard case, 1006 for LOWLAT, and 999 steady periods in each. The paired 48 kHz control is the one in the first table.

| Reserved case | Original peak, us | Final peak, us |
|---|---:|---:|
| RATE22050, GROUPS32 | 6209.31 | 4894.96 |
| RATE192000, GROUPS32 | 6228.42 | 4855.60 |
| RATE96000, LOWLAT, GROUPS1 | 6235.19 | 4918.14 |

All cases passed their counter, timeline, delay and sample-count checks. The holdout comparison agreed with the keep decision; regression list: empty. Reserved results were not used to develop another hypothesis. This original-to-final cohort supplies both the sole keep's confirmation and the final-state comparison; it is not a comparison against an intermediate retained revision.

## Correctness evidence

Actual candidate logs show the host audio/session suites and 118 SDK service checks passing. Default-off output matched the frozen engine for 30000 sequences; 3000 oversampled scenarios matched the independent 96 kHz convolution reference. The qemu-arm NEON baseline, ramp, limiter, cubic, cubic guard and oversampling suites passed.

Additional host and ARM proofs cover 116640 linear guarded-reference scenarios and 15552 cases with actual registered banks of 1, 2 or 3 frames, all four formats, both directions, loops, one-shot ends, ramps and partial vectors. Five new S8/S8S lane-order, backward-address and over-read mutants failed both ARM guard probes, by PCM mismatch or a protected-page fault as appropriate.

The original image's correctness receipt uses the explicit recorded-source exception bound only to the original build/image identity; its earlier host raw logs were not preserved. New candidates are not allowed that exception and use the actual logs above.

## Limits and remaining work

- The oversampling feature bit remains clear. Product integration still needs atomic cubic+2x refusal and its tests before advertising nearest/linear 2x. Cubic+2x measured 8.563 ms on the earlier E1 image and is not qualified.
- Full-load media/voice coexistence remains unqualified. This optimization does not claim to fix its video drops or late producer records.
- No fresh post-keep phase profile was taken. Earlier estimated mixing/FIR shares are stale; they do not identify a new bottleneck.
- Any later feature or compiler change needs its own render-budget qualification. The margin is small.

## Accounting and local receipts

Stop: every required target reached. Two serial candidates: one kept, one not selected; zero reverted, inconclusive, censored, degenerate, errors, timeouts or deferred dependencies. No judge calls ($0). Whole-run model cost is unavailable and was uncapped by approval.

Phase 3 stopped at `2026-11-24T08:04:51.935098Z`: 6453.212 active seconds (107.55 minutes), 33731.061 elapsed seconds from the original approval anchor (9 h 22 min 11 s). External power waits, including the earlier overnight wait, are excluded from active time. The user explicitly extended the caps to four candidates, two active hours total and ten elapsed hours from the unchanged original anchor; no experiment ran while the earlier approval was expired.

The local ledger is `.context/compound-engineering/ce-optimize/voice-oversample-card/experiment-log.yaml`, with `spec.yaml` beside it. Actual CLI/UART snapshots, per-case receipts, correctness logs and canonical decisions are in `.docker-tmp/oversample-opt/e2/`, especially `baseline-confirm-*`, `final-confirm-*`, `qualification.json`, `host-suite.log`, `arm-suite.log` and `confirmation-decision.json`. These ignored local artifacts and private benchmark refs are not bundled with this report.
