# ZZ9000AX DAC saturation ceiling and the enforced boundary

Firmware (`audio_scene.c`) enforces per-source ceilings and a weighted
combined-level boundary. This document records the bench method and
the corrected R1 measurements (48/80); these are **per-card** results,
not safe defaults for every board revision. The permanent post-mix
limiter protects the combined output, not distortion introduced earlier
on either input.

Host tests verify staging arithmetic and DSP write order. Clean output
and click-free filter transitions require measurements on real hardware.

## The combined-level model

The boundary applies to the composed level evaluated by
`audio_scene.c` (`resolve_output_volume`, `compute_mixer_stage`):

    combined = ((baseline_paula + paula_trims) * ceiling_ax / ceiling_paula
                + baseline_ax + ax_trims)
               * prefactor_gain * (volume / 100) * eq_worst_boost

- Mixer legs are 0..255 (127 = 0 dB). `audio_adau_init` starts at
  Paula 36 / AX 72; boot re-applies a saved baseline if present.
  With no saved calibration the one-card-informed fallback ceilings
  are Paula 48 / AX 80 (Level 132/160 at the parity baseline).
- Prefactor: 0..100, -12 dB .. +12 dB, 50 = 0 dB
  (`audio_dsp_gain.h`).
- Scene volume: 0..100, 0 dB at 100. Pan does not enter the model.
- EQ: 0..100, ±12 dB per band, 50 = 0 dB; the model uses the single
  worst positive band (`eq_worst_boost_linear`).

`ZZ9K_AUDIO_BALANCE_NEUTRAL` is the keep-baseline request: AHI and
MHI add no owner trim, so their allocation leaves the operator's
baseline pair applied verbatim.

## Current limiter-era boundary

This firmware derives the AX-equivalent boundary as `2 * ceiling_ax`
and independently caps each applied mixer leg at its configured
ceiling. With no saved calibration the fallback is Paula 48 / AX 80,
giving a boundary of 160 and a boot baseline of 36/72. The pair was
measured on one R1 board; it is a conservative shipping candidate,
**not a clean guarantee for other boards**. Saved per-card settings
remain authoritative. `ZZ9K_OP_AUDIO_CONTROL_STATE_GET` reports the
resulting boundary.

The original pre-limiter provisional value was 192: a 3/4 reduction
from an uncalibrated 256-unit forum estimate. It is historical and is
not an enforced constant in the current firmware. Scene-alone boost
reduces applied output volume; a composed over-boundary trim is bounded
and reported. Neither action undoes distortion that occurred before
the limiter. Do not derive a universal clean input ceiling from the
limiter threshold or the one qualified R1 card.

## Historical pre-limiter measurement method (2026-08)

The following 0/254 and 254/0 sweeps documented the original bench
session. **Do not replay them unchanged on the limiter build:** the
current per-leg ceilings reject such uncalibrated mixer levels, and
the engaged limiter changes the distortion curve. Do not raise
ceilings solely to run this historical stimulus. For current hardware
qualification use independent line-output/source-reference captures
within each card's safe range, and separately note limiter engagement.

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
6. **Leg cross-check.** A normal same-channel Paula loopback is invalid
   because AHI recording taps the physical ADC before `St Mixer1`.
   Use `ZZAXDuplexTest <capture.raw> 5 paula-cross`: it generates a
   deterministic left-only Paula tone and starts AHI record without AHI
   playback. First disconnect every line-output-to-auxiliary cable and
   capture `cap_a205_paula_isolation.raw`. The qualified R1 card placed
   the direct left Paula reference on capture channel 2 and isolated
   channel 1 by approximately 101 dB. Identify the active line-output
   RCA with an amplifier, then connect only that output to
   auxiliary-input **left**; leave the other line output and auxiliary
   input right disconnected. The internal Paula ordering makes this
   cross-channel in the capture domain. Set baseline Paula 254 / AX 0
   and capture the onset-relevant scene levels. Analyze only the
   returned output with `--auto-tone --channel 1 --reference-channel 2`;
   the 0.5% threshold then applies to THD growth above the simultaneous
   direct-source floor. Matching AX-only and cross-channel Paula
   onset curves support the mixer-unit assumption; disagreement blocks
   any constant update.
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
    python3 util/analyze_audio_saturation.py --auto-tone --channel 1 \
        --reference-channel 2 --levels ... cap_a205_paula_....raw
    python3 util/test_analyze_audio_saturation.py   # self-check prints PASS

Reads raw signed-16-bit PCM (`--endian be` for Amiga-side AHI dumps,
`--endian le` for host-side captures; `--channels`, 1-based `--channel`
and `--reference-channel`, `--rate`, `--tone`, `--auto-tone`, `--rail`,
`--thd-pct` adjustable).
`--auto-tone` estimates each capture's realized fundamental from robust
local periods in the centered analysis window before the coherent THD
projection; use it for Paula-period captures. Per channel it reports:

| Metric | Meaning |
|---|---|
| peak dBFS | captured peak (attenuation makes absolute level meaningless; trend only) |
| fundamental dBFS | fitted fundamental amplitude in the centered analysis window — compare this across gain steps |
| dc | mean offset |
| at-rail samples/regions | capture-path clipping — a setup fault (add loopback attenuation) |
| THD rise | selected-channel THD minus the simultaneous reference-channel THD floor; when a reference is selected, this replaces absolute THD for the verdict |
| flat-top regions (longest run) | consecutive samples parked within ~0.4% of the file's own peak — at least three regions are required for the clipped/hard-saturated crest verdict; one-off low-level plateaus remain diagnostic only |
| THD proxy | coherent H2..H5 against the fundamental over an integer-period window — catches soft saturation before hard clipping |
| notch residual dBFS | floor after removing DC, fundamental and harmonics |

Verdicts: `FAULT` (at-rail: fix the setup), `SATURATED` (at least
three flat-top regions or THD above `--thd-pct`, default 0.5%),
`CLEAN`. With `--reference-channel`, the threshold applies to THD rise
above that simultaneous source floor instead of absolute THD.
`--levels` reports onset, last clean level and the 3/4 suggestion;
`--json` emits the same report machine-readably.

## Headroom policy and unit-to-unit variance

The bench persists two per-card clean ceilings. For Paula ceiling
$C_P$, AX ceiling $C_A$, mixer legs $P/A$ and scene gain $G$, firmware
enforces:

$$G\left(P\frac{C_A}{C_P}+A\right) \le C_P\frac{C_A}{C_P}+C_A = 2C_A$$

Each applied leg is additionally capped to its configured ceiling,
independent of the sum. The post-mix limiter (engaged at 0.47 FS)
bounds the summed output, so a card using the 48/80 fallback has a
modeled boundary of 160. The earlier pre-limiter
$\frac{3}{4}C_A$ headroom policy (60 on that R1 card) applies only
to the 2026-08 measurements below. The 48/80 fallback is not a
universal per-card calibration; saving a measured pair replaces it.

## Measurement records — 2026-08-23

### Superseded pre-`a205` sweep

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
must be remeasured on the corrected instrument build. Boundary 60 was
not accepted; at that stage the pre-limiter boundary remained the
provisional 192 and capability advertising was still blocked.

Corrected instrument image
`5cc8265d81421d6c132c594b68d1dd68ae95d53a8181c18311f66f78f474a046`
(DSP profile `a205`) passed the source-leg portion of S9 on the same
card: scene volume `100 -> 0 -> 100` sounded, muted and restored Paula
and AX independently; prefactor `50 -> 0 -> 50` reduced and restored
both sources. This confirms both stereo prefactor algorithms and the
post-mixer scene volume on hardware. EQ and raw-capture isolation remain
covered by the subsequent release-candidate session.

### Corrected `a205` AX-only sweep

The corrected instrument image used baseline Paula 0 / AX 254 and the
one-control coherent AHI stimulus:

    python3 util/analyze_audio_saturation.py \
        --levels 32,48,64,73,80,89,97,111,127 \
        cap_a205_ax_0032.raw cap_a205_ax_0048.raw \
        cap_a205_ax_0064.raw cap_a205_ax_0073.raw \
        cap_a205_ax_0080.raw cap_a205_ax_0089.raw \
        cap_a205_ax_0097.raw cap_a205_ax_0111.raw \
        cap_a205_ax_0127.raw

Levels 32, 48, 64, 73 and 80 were CLEAN. Level 89 was the first
SATURATED step: THD rose from approximately 0.42% at 80 to 0.73% at 89,
then monotonically to approximately 3.08% at 127. No capture hit rail
or contained a persistent flat-top. The corrected AX-only ceiling is
80 and the onset interval is `(80, 89]`; the 3/4 policy suggests
boundary 60.

The no-cable `paula-cross` isolation capture detected the direct
left-only Paula fundamental at 998.567 Hz on capture channel 2:
-1.63 dBFS versus -102.5 dBFS on channel 1, approximately 101 dB of
isolation, with no rail samples. This qualifies channel 1 for the
returned-output sweep through the active line output -> auxiliary-input left.

### Corrected `a205` Paula cross-channel sweep

The active line-output right RCA was cross-connected to auxiliary-input
left. Capture channel 1 held the returned output; channel 2 remained the
simultaneous direct-source reference:

    python3 util/analyze_audio_saturation.py --auto-tone \
        --channel 1 --reference-channel 2 \
        --levels 16,24,32,40,48,56,64,73,80,89,97 \
        cap_a205_paula_probe_0016.raw cap_a205_paula_probe_0024.raw \
        cap_a205_paula_probe_0032.raw cap_a205_paula_probe_0040.raw \
        cap_a205_paula_probe_0048.raw cap_a205_paula_probe_0056.raw \
        cap_a205_paula_probe_0064.raw cap_a205_paula_probe_0073.raw \
        cap_a205_paula_probe_0080.raw cap_a205_paula_probe_0089.raw \
        cap_a205_paula_probe_0097.raw

The direct reference stayed at -1.61 dBFS and 0.975-0.980% THD across
all scene levels. Returned-output THD stayed within 0.18 percentage
points of that floor through level 48, rose 0.564 points above it at
56, then increased monotonically to a 4.509-point rise at 97. No
capture hit rail or developed a persistent flat-top. With the standard
0.5% criterion applied to THD rise above the simultaneous source floor,
the Paula ceiling is 48 and onset lies in `(48, 56]`; the 3/4 policy
would suggest boundary 36.

The intervals do not match, disproving an unweighted mixer sum on this
R1 card. The adopted persisted calibration is Paula ceiling 48 / AX
ceiling 80: Paula weight $80/48=1.667$ and a derived AX-equivalent
boundary of 60 under the then-current 3/4 policy (the limiter-era
boundary for the same pair is 160). ZZTop exposes both measured
ceilings; scene Save persists them as `audio_ceiling_paula` /
`audio_ceiling_ax`.

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
| S6 | Save round-trip through reboot (AE6) | Set operator baseline and measured ceilings (e.g. 48/80), Save, cold boot | Baseline, calibration and scenes persist; state reports both ceilings and the derived boundary |
| S7 | Leftover `ZZ9K_MIX_LEVELS` no effect (AE2) | Set `ENV:ZZ9K_MIX_LEVELS` to an extreme pair, keep CFG audio keys present, boot | Scene applies; the env var changes nothing in either driver or firmware |
| S8 | Non-AX entry disabled (R18) | On a ZZ9000 without the AX daughterboard, open ZZTop | The Audio button is present but disabled (greyed), not hidden; all other ZZTop features unaffected |
| S9 | Master scene and calibration cover both legs | Play Paula only, then AX only; exercise volume/prefactor and request a weighted over-boundary pair | Both sources follow scene controls; gain reduction uses the persisted per-leg ceilings; RCA capture stays pre-mix |

S8 needs non-AX hardware (or the AX-absent probe path); if the session
only has an AX card, record it as not exercised rather than passed.

## Historical pre-limiter gate and current qualification

The earlier calibrated hardware gate passed on one R1 card:

- Paula 36 safe / 37 reduced;
- AX 60 safe / 61 reduced;
- mixed 18/30 safe / 19/30 reduced;
- measured ceilings 48/80 and the **then-current** derived boundary 60
  survived Save and power-cycle.

The production limiter later replaced that 3/4 policy with the
weighted sum of both per-leg ceilings. In this candidate, 48/80
becomes the uncalibrated fallback (boundary 160, parity baseline
36/72). It was measured on one R1 card and is not a cross-revision
guarantee. Existing saved calibration and baseline keys override it.
For issue #117, record continuous output while dragging LPF/EQ to
distinguish parameter transients from input distortion; safeloaded
live LPF and a reduced edit rate improve clicks but do not prove
click-free audio on hardware. More representative returned-output
and source-reference measurements are still required before calling
the fallback clean on every board.

The matched release advertises `SDK_CAP_AUDIO_CONTROL |
SDK_CAP_AUDIO_METERING` and `SDK_SERVICE_FLAG_AUDIO_CONTROL`. ZZTop,
AHI and MHI gate on those capabilities, so older firmware keeps its
documented fallback.

## Matched-set release coordination

- The drivers' `sdk/SDK_REF` pins public SDK commit `eb0ddaa`
  (`fix(caps): register calibrated audio helpers`). Fresh
  runners clone that exact ABI/service contract; ZZTop/AHI/MHI builds
  fail clearly if staged headers lack the audio control surface.
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
