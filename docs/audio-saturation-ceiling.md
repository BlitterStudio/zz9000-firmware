# ZZ9000AX DAC saturation ceiling and the enforced boundary

The firmware-authoritative control plane (`audio_scene.c`, plan R6/R7)
enforces a gain-staging boundary in combined-level units instead of
the community-forum figure both drivers used to quote. This document
defines the boundary's units, the bench method that replaces the
forum figure with a measured one, the analysis helper, the headroom
policy, the hardware smoke checklist for the verification session, and
the runbook that lands the measured value and the capability
advertising after a recorded pass.

Host tests in `test/audio` enforce the staging math; nothing on this
page is enforced until the hardware session records it.

## The combined-level model

The boundary applies to the composed level the staging code evaluates
(`audio_scene.c`: `resolve_output_volume`, `compute_mixer_stage`):

    combined = (baseline_paula + baseline_ax + sum(owner trims))
               * prefactor_gain * (volume / 100) * eq_worst_boost

- Mixer legs (`audio_adau_set_mixer_vol` scale): 0..255 each,
  127 = 0 dB. Power-on baseline is Paula 128 / AX 64 (the state
  `audio_adau_init` writes).
- Prefactor: 0..100, -12 dB .. +12 dB, 50 = 0 dB
  (`audio_dsp_gain.h`).
- Scene volume: 0..100, 0 dB at 100. Pan does not enter the model.
- EQ: 0..100, ±12 dB per band, 50 = 0 dB; the model uses the single
  worst positive band (`eq_worst_boost_linear`).

`ZZ9K_AUDIO_BALANCE_NEUTRAL` is the keep-baseline request: AHI and
MHI add no owner trim, so their allocation leaves the operator's
baseline pair applied verbatim.

## Provisional enforced boundary (current state)

The single named definition is:

    ZZ9000_proto.sdk/ZZ9000OS/src/audio_scene.h
    #define AUDIO_SCENE_ENFORCED_BOUNDARY 192.0

Provisional derivation: both drivers documented that summed mixer
values above ~0x100 (256) saturate the DAC (`zz9000ax-ahi.c`,
`mhizz9000.c`, since removed); scaled by 3/4 that gives ~2.5 dB of
stated headroom below the forum figure. This value ships until the
bench session below replaces it; `ZZ9K_OP_AUDIO_CONTROL_STATE_GET`
reports it as the `ceiling` field, and the staging host tests pin
behavior against it.

A composition over the boundary is reduced — the mixer sum first
(reported back to the requesting owner as a bound), the applied output
volume when the scene alone exceeds it — and each reduction emits
exactly one gain-reduction telemetry event.

## Measurement method (bench session)

Single session on target hardware, on the operator's own card. The
primary self-capture sweep calibrates the AX-only path in combined-level
units with one phase-coherent active leg whose coefficient equals the
model sum (0/254). A separate recorder then captures onset-relevant
AX-only reference steps and matching 254/0 Paula-only steps through the
same independent ADC. This checks both leg paths without combining
unsynchronized source clocks or observing a pre-output Paula feed.

Stimulus and routing:

Generate the deterministic AHI and Paula source files with:

    python3 util/generate_audio_ceiling_stimuli.py <output-directory>

1. **Loopback.** Cable the ZZ9000AX line-out RCA pair to the line-in
   RCA pair. Set both auxiliary jumpers to **IN**; the capture input is
   the fixed-gain `RCA In` path. Add an in-line passive attenuator if
   step 2 shows the capture itself nearing rail.
2. **Level sanity.** At a low combined level (e.g. prefactor -6 dB),
   record and check the capture peaks at or below roughly -6 dBFS and
   the analyzer reports no at-rail samples. At-rail samples in a
   capture mean the ADC clipped, which is a setup fault, not DAC
   saturation — attenuate and restart if this appears at any step.
3. **Applied leg.** Set ZZTop's external baseline sliders to Paula 0 /
   AX 254. AHI's keep-baseline trim leaves that pair unchanged, so the
   model sum stays fixed at 254 while one phase-coherent AX signal
   carries the full combined level. Do not Save this temporary bench
   baseline and do not move either slider during the primary sweep.
4. **Tone and capture.** AHI's low-level allocation is exclusive, so
   `AHIRecord` plus a separate AHI player cannot use ZZ9000AX at the
   same time. Run `ZZAXDuplexTest <capture.raw> 5 ceiling` once per
   prefactor step. Ceiling mode uses one `AHIAudioCtrl` to play a
   coherent 48 kHz stereo 1 kHz sine at 0.99 FS and record AHI capture
   simultaneously; it writes raw S16BE stereo. Do not run a Paula
   player during this primary sweep: independent Paula and AHI clocks
   are not phase-locked, so their changing relative phase would make
   the physical peak unrelated to the labeled combined level.
5. **Scene setup.** Use scene 0 with LPF 23900, all EQ bands 50 and
   pan 50. Establish a low-level distortion floor, then refine the
   previously observed transition region with these integer settings:

   | Capture label | Prefactor | Volume | Model level |
   |---:|---:|---:|---:|
   | 32 | 0 | 50 | 31.9 |
   | 48 | 0 | 75 | 47.9 |
   | 64 | 0 | 100 | 63.8 |
   | 73 | 5 | 100 | 73.3 |
   | 80 | 8 | 100 | 79.6 |
   | 89 | 12 | 100 | 88.9 |
   | 97 | 15 | 100 | 96.6 |
   | 111 | 20 | 100 | 110.9 |
   | 127 | 25 | 100 | 127.3 |

   Each step is one staged scene-write commit in ZZTop's Audio window
   (glitch-free; playback keeps running). Wait one second after the
   final slider change so the incremental commit has settled, then run
   `ZZAXDuplexTest <capture.raw> 5 ceiling`. Record at least 2 seconds
   per step to a raw capture file named by its rounded combined level,
   and watch the Audio-window meters as a live cross-check. Extend the
   sweep upward only if level 127 remains clean; extend downward only
   if level 32 is already non-clean.
6. **Leg cross-check.** Do not use ZZ9000AX AHI recording for this
   step. Bench diagnostics established that it observes the Paula feed
   even with baseline `0/0` and with scene volume `0`, so it cannot
   isolate the controlled line output. Disconnect the self-loopback and
   connect the line-output RCA pair to an independent external
   ADC/recorder at fixed gain. Capture onset-relevant AX-only reference
   steps with baseline Paula 0 / AX 254 and the coherent AHI stimulus,
   then capture the same combined levels with baseline Paula 254 / AX 0
   and the deterministic full-scale Paula stimulus. Export stereo
   signed-16 raw PCM at the recorder's native endian/rate and analyze
   Paula with `--auto-tone`. Matching AX-only and Paula-only onset
   curves from the same external ADC support the linear mixer
   assumption; disagreement blocks any constant update.
7. **Detection.** Analyze the captures on the host with
   `util/analyze_audio_saturation.py` (below) in sweep order with the
   combined levels. The measured ceiling is the highest combined level
   whose capture is CLEAN; the true onset lies in the interval between
   it and the first non-clean step, so record both values plus the step
   size.
8. **Record.** Append the measurement to this file: measured ceiling,
   onset interval, sweep step size, loopback attenuation, capture rate,
   the cross-check result, the analyzer invocation, and the firmware
   commit measured.

The ZZTop meters are a live complement, not the detector: they see the
digital-domain rail (`audio_scene.c` meter accumulators count at-rail
regions of the int16 path), while the capture THD/flat-top analysis
sees the analog DAC output through the loopback. Both are worth
watching during the sweep; only the capture analysis decides the
ceiling.

## Analysis helper

    python3 util/analyze_audio_saturation.py --levels 160,176,192,... \
        cap_0160.raw cap_0176.raw cap_0192.raw ...
    python3 util/analyze_audio_saturation.py --auto-tone --levels ... \
        cap_paula_....raw
    python3 util/test_analyze_audio_saturation.py   # self-check prints PASS

Reads raw signed-16-bit PCM (`--endian be` for Amiga-side AHI dumps,
`--endian le` for host-side captures; `--channels`, `--rate`,
`--tone`, `--auto-tone`, `--rail`, `--thd-pct` adjustable).
`--auto-tone` estimates each capture's realized fundamental from robust
local periods in the centered analysis window before the coherent THD
projection; use it for Paula-period captures. Per channel it reports:

| Metric | Meaning |
|---|---|
| peak dBFS | captured peak (attenuation makes absolute level meaningless; trend only) |
| fundamental dBFS | fitted fundamental amplitude in the centered analysis window — compare this across gain steps |
| dc | mean offset |
| at-rail samples/regions | capture-path clipping — a setup fault (add loopback attenuation) |
| flat-top regions (longest run) | consecutive samples parked within ~0.4% of the file's own peak — at least three regions are required for the clipped/hard-saturated crest verdict; one-off low-level plateaus remain diagnostic only |
| THD proxy | coherent H2..H5 against the fundamental over an integer-period window — catches soft saturation before hard clipping |
| notch residual dBFS | floor after removing DC, fundamental and harmonics |

Verdicts: `FAULT` (at-rail: fix the setup), `SATURATED` (at least
three flat-top regions or THD above `--thd-pct`, default 0.5%),
`CLEAN`. With `--levels` the
tool reports the onset step, the measured ceiling (last clean level),
the onset interval, and the suggested `AUDIO_SCENE_ENFORCED_BOUNDARY`
at the 3/4 headroom policy. `--json` emits the same report
machine-readably.

## Headroom policy and unit-to-unit variance

The bench measures one card. The enforced boundary carries 3/4
headroom (~2.5 dB) below the measured ceiling, the same policy the
provisional value used, chosen to cover expected component variance
across boards (mixer summing-stage and codec gain tolerances). This
single-unit scope and the variance rationale are adopted as-is for
this release; if field reports ever show cards whose clean ceiling
sits inside another card's headroom band, re-measure on a second unit
and widen the policy deliberately rather than silently.

## Measurement record — 2026-08-23 (superseded)

Instrument firmware SHA-256:
`fe7a61a19d86d338dc411c7ea0e69150a5a7a8e804f5d76c8319ba4262a4f78e`.
The primary Paula 0 / AX 254 self-capture sweep used the internal
0.99-FS 1 kHz ceiling stimulus and raw 48 kHz S16BE stereo:

    python3 util/analyze_audio_saturation.py \
        --levels 32,48,64,80,97,111,127,142,159 \
        cap_0032.raw cap_0048.raw cap_0064.raw cap_0080.raw \
        cap_0097.raw cap_0111.raw cap_0127.raw cap_0142.raw \
        cap_0159.raw

Levels 32, 48, 64 and 80 were CLEAN. Level 97 was the first
SATURATED step at approximately 1.11% THD; THD then rose monotonically
through approximately 5.19% at 159. No capture hit rail. The measured
AX-only ceiling is 80, the observed onset interval is `(80, 97]`, and
the 3/4 policy would suggest boundary 60.

Four self-captures labeled 64, 80, 97 and 111 all retained an
approximately -3.2 dBFS Paula fundamental with no gain trend.
Diagnostic captures then retained the same Paula tone with baseline
`0/0` and with scene volume `0`. An audible line-output test isolated
the real defect: the Paula baseline leg muted correctly, but scene
volume `0` did not mute Paula. The source-authoritative SigmaDSP graph
placed the Paula/ADC monitor branch into `St Mixer1` after the scene
blocks, while FPGA playback alone traversed LPF, prefactor, EQ and
volume.

The graph was corrected so Paula/ADC and FPGA playback enter `St
Mixer1` first and its combined output traverses the complete scene
chain; the raw ADC capture taps remain before that mixer. Because the
recorded sweep predates this topology correction, its `(80, 97]`
AX-only interval and candidate boundary 60 are informational only and
must be remeasured on the corrected instrument build. Boundary 60 is
not accepted; `AUDIO_SCENE_ENFORCED_BOUNDARY` remains the provisional
192 and capability advertising remains blocked.

Corrected instrument image
`5cc8265d81421d6c132c594b68d1dd68ae95d53a8181c18311f66f78f474a046`
(DSP profile `a205`) passed the source-leg portion of S9 on the same
card: scene volume `100 -> 0 -> 100` sounded, muted and restored Paula
and AX independently; prefactor `50 -> 0 -> 50` reduced and restored
both sources. This confirms both stereo prefactor algorithms and the
post-mixer scene volume on hardware. EQ and raw-capture isolation remain
covered by the subsequent release-candidate session.

## Hardware smoke checklist (verification session)

Run on the release-candidate bench build from the runbook below. No
item here is done until the session records its observation; each
expected observation traces to the plan's acceptance examples.

| # | Item | Steps | Expected observation |
|---|---|---|---|
| S1 | Boot/warm-reset scene apply (AE5) | Save a distinctive scene (e.g. LPF 12 kHz, prefactor 45, volume 70), power-cycle, then warm-reset with Ctrl-Amiga-Amiga | After both resets the master chain returns to the saved scene, not DSP defaults, before any app allocates; ZZTop's Audio window shows the saved scene active |
| S2 | AHI/MHI handoff, no stamps (AE3) | Play via MHI, stop, then allocate AHI and play | Master chain stays scene-owned throughout: no 20 kHz LPF appears at MHI Play start, no mixer stamp at AHI allocate; audio uninterrupted |
| S3 | Live switch glitch-free (R5/F3) | Switch scenes and commit scene edits during active playback | No interruption, click, or partial assignment; rapid double-switches serialize (no torn state) |
| S4 | Clamp event visible (AE1) | Request a trim that, with baseline and scene level, exceeds the enforced boundary | Applied trim is bounded and the requester can observe it; ZZTop's gain-reduction indicator lights and the event (requested/applied/boundary) is readable from telemetry |
| S5 | Meters update (AE4) | Play a deliberately hot signal; read meters between periods from the Audio window | Peak-hold and clip count reflect the signal; repeated reads are self-consistent; no audible or timing disturbance from reading |
| S6 | Save round-trip through reboot (AE6) | Set operator baseline balance (e.g. 100/90), Save, cold boot | Baseline and scenes persist and apply to every owner's playback with no env var anywhere |
| S7 | Leftover `ZZ9K_MIX_LEVELS` no effect (AE2) | Set `ENV:ZZ9K_MIX_LEVELS` to an extreme pair, keep CFG audio keys present, boot | Scene applies; the env var changes nothing in either driver or firmware |
| S8 | Non-AX entry disabled (R18) | On a ZZ9000 without the AX daughterboard, open ZZTop | The Audio button is present but disabled (greyed), not hidden; all other ZZTop features unaffected |
| S9 | Master scene covers both legs | Play Paula only, then AX only; for each source move scene volume 100 -> 0 -> 100 and exercise prefactor/EQ | Volume 0 silences each source independently, restoring volume restores it, and prefactor/EQ audibly affect both; RCA capture remains the pre-mix physical ADC feed |

S8 needs non-AX hardware (or the AX-absent probe path); if the session
only has an AX card, record it as not exercised rather than passed.

## Flip-after-pass runbook

The capability flip lands only in the commit following a recorded
hardware pass (plan R12/KTD6). Until then nothing advertises
`ZZ9K_CAP_AUDIO_CONTROL` or `ZZ9K_CAP_AUDIO_METERING`, and ZZTop's
Audio button stays disabled on every matched set — discoverable, not
hidden.

Bench builds (local working-tree edits, never committed as-is):

- **Instrument build** (ceiling measurement): set
  `AUDIO_SCENE_ENFORCED_BOUNDARY` to `4096.0` (above any reachable
  composition, so staging never reduces during the sweep) **and** apply
  the advertising flip below, so ZZTop's Audio window and meters are
  live for the sweep. Build with `./build_firmware.sh` and flash.
- **Release-candidate build** (smoke checklist): restore the working
  tree, then set `AUDIO_SCENE_ENFORCED_BOUNDARY` to the measured
  ceiling x 3/4 **and** apply the advertising flip. Re-flash and run
  S1-S8.

After a recorded pass, in this order:

1. **Constant commit.** Restore the working tree completely. Update the
   single site `ZZ9000_proto.sdk/ZZ9000OS/src/audio_scene.h`
   (`#define AUDIO_SCENE_ENFORCED_BOUNDARY`) to the measured boundary,
   re-derive the boundary-dependent fixtures that pin `192` literals
   (`test/audio/audio_scene_test.c` staging/event/headroom checks,
   `test/audio/audio_control_test.c` ceiling report), re-run
   `make -C test/audio test` and `make -C test/config test`, and append
   the measurement record to this document.
2. **Advertising commit.** In `ZZ9000_proto.sdk/ZZ9000OS/src/sdk_mailbox.c`
   only:
   - `SDK_MAILBOX_BASE_CAPABILITY_BITS` gains
     `| SDK_CAP_AUDIO_CONTROL | SDK_CAP_AUDIO_METERING` — this word is
     what `SDK_OP_QUERY_CAPS` returns via `mailbox_capability_bits()`,
     and what ZZTop, AHI and MHI gate on.
   - The `SDK_SERVICE_AUDIO` entry in `sdk_services[]` gains the same
     two caps in its capability field, gains
     `| SDK_SERVICE_FLAG_AUDIO_CONTROL` (bit 21) in its flags field,
     and its opcode count rises from `9` to `15` so `QUERY_SERVICE`
     reports the full `0x0500..0x050e` range including the control
     plane.
   Optional same-commit polish: add `"audio-control"` /
   `"audio-metering"` names for the two bits to
   `zz9k_capability_name()` in the SDK's `include/zz9k/caps.h` so
   `zz9k-services` prints them.
3. Both commits run `git diff --check` clean; firmware CI green.

## Matched-set release coordination

- The drivers' `sdk/SDK_REF` pins SDK commit `2873f1d`
  (`feat(abi): define audio control-plane and metering opcodes and
  caps`), which currently exists **only on the SDK's local branch**
  `feat/audio-control-plane-scenes-metering`. That branch must be
  pushed and merged before drivers CI runs on fresh runners — the `sdk`
  and `amissl` jobs clone the pinned commit, and the ZZTop/AHI build
  scripts hard-fail when the staged headers lack
  `ZZ9K_OP_AUDIO_SCENE_SELECT` / `ZZ9K_OP_AUDIO_TRIM_SUBMIT`
  (`ZZTop/build-gcc.sh`, `ahi/driver/build.sh`).
- Tag order is the drivers' `RELEASING.md` dependency order: SDK first
  (merge + tag + push), then drivers `SDK_REF` bump to the SDK release
  commit (CI green on the pin), then firmware, then drivers. Firmware
  and drivers carry the same `vX.Y.Z`.
- ZZTop's Audio window, both drivers' capability gates, and their
  warning strings already reference the new surface; they activate the
  moment the advertising commit's firmware ships in the matched set.
  Mixed-version pairings degrade per the AHI README: old firmware +
  new drivers means no trim and a disabled Audio button (normal, not an
  error); new firmware + old drivers means the old stamps are rejected
  by the authority gate and playback continues unaffected.
