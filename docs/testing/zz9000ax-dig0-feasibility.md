# ZZ9000AX DIG0 feasibility gate

Date: 2026-07-28

Decision: **EXCLUDED — `NOT_PACKAGED_FEASIBILITY_FAIL`**

DIG0-only multiplexing is not a runtime mode in the characterization image.
U9 is not applicable. I2S and TDM8 remain the admitted transports.

## Gate contract

A DIG0 mode could be admitted only with a complete design that:

- carries two independent signed 16-bit channels at exactly 48,000 samples
  per second per channel on the existing `SDATA_OUT0` wire;
- preserves channel identity across reset and dropped or duplicated frames;
- restores conventional 48 kHz stereo before the existing receiver/DMA path;
- supports contemporaneous AHIRecord monitoring;
- leaves ordinary 48 kHz playback normal before and after capture; and
- fits the ADAU1701 DSP instruction budget and the FPGA timing/resource
  budget.

An incomplete bench probe is not evidence for this gate.

## Codec facts

The authoritative codec source is the Analog Devices
[ADAU1701 Rev. C data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ADAU1701.pdf).

- The serial output supports I2S, left/right-justified formats, and
  8-channel TDM. A serial slot carries at most 24 programmable data bits;
  extra bits are truncated (Serial Data Input/Output Ports, pp. 45–46).
- TDM8 can operate as a master at 48 kHz and 96 kHz, but the serial mode
  does not create another physical data wire (Table 63).
- At 48 kHz the DSP has 1024 instruction cycles per sample. A 96 kHz core
  mode has only 512 instruction cycles per sample (Programming, p. 29, and
  DSP Core Control Register).
- The normal U1 SigmaStudio graph already reaches its 1024-cycle resource
  boundary: adding the ADC/pattern selector and another input algorithm
  produced SigmaStudio's `Not Enough DSP resources for this algorithm`
  compiler error. The production processing/monitor graph therefore cannot
  simply be moved to the 512-cycle 96 kHz core.

## Candidate A: pack both channels into one 48 kHz DIG0 slot

Required payload per frame:

| Item | Bits |
|---|---:|
| Left signed PCM | 16 |
| Right signed PCM | 16 |
| Minimum payload | 32 |
| DIG0 serial-slot capacity | 24 |
| Deficit before framing | 8 |

This candidate necessarily truncates or compresses at least eight channel
bits before adding any phase or recovery marker. It fails the full-fidelity
signed-16 requirement and is excluded.

Using two 12-bit samples, one 16-bit sample plus a reduced second sample, or
lossy compression does not satisfy the gate.

## Candidate B: alternate tagged channels at 96 kHz

A 96 kHz, 24-bit DIG0 slot has enough aggregate bandwidth for one signed
16-bit sample plus an 8-bit left/right marker in each transport frame. A
tagged alternating format could therefore make reset and dropped-frame
recovery self-synchronizing.

It still fails the complete-design gate for two independent reasons:

1. Producing alternating 48 kHz left/right samples requires a 96 kHz DSP
   schedule (including any required decimation). That cuts the available
   program from 1024 to 512 instructions, while the required production
   processing and monitor graph already reaches the 48 kHz resource
   boundary.
2. The codec's capture clocks currently also clock the playback path. A
   96 kHz output mode changes the clocks seen by the Xilinx transmitter,
   receiver, and formatters. No independent 48 kHz playback clock or proven
   asynchronous rate adapter exists in this design.

The second point is visible in the checked-in topology:

| Local source | Connection |
|---|---|
| `audio_clock.v` | `mclk_out = bclk_in` |
| `audio_clock.v` | `bclk_out = clkgen[2]` |
| `zz9000_project.tcl` | `audio_clock_0/mclk_out` drives both audio formatters plus the I2S receiver and transmitter `aud_mclk` inputs |
| `zz9000_project.tcl` | `audio_clock_0/bclk_out` drives the receiver/transmitter `sclk_in` inputs and the external clock port |
| `zz9000_project.tcl` | codec `I2SO_LRCLK` drives both `audio_clock_0/lrclk_in` and `i2s_transmitter_0/lrclk_in` |

Changing codec framing to 96 kHz therefore changes playback timing at the
same time. Preserving 48 kHz playback would require a separately generated
and constrained clock, an explicit clock-domain crossing, buffering/rate
adaptation, reset behavior, and proof that AHIRecord monitoring remains
contemporaneous. None of those choices has a complete implementation or
simulation contract.

## Binary outcome

| Gate item | 48 kHz packed DIG0 | 96 kHz tagged DIG0 |
|---|---|---|
| Two lossless signed-16 channels | Fail: 32 bits do not fit in 24 | Feasible in aggregate |
| Self-synchronizing identity | No marker budget | Feasible with an 8-bit tag |
| Reset/drop recovery | Not specifiable losslessly | Specifiable |
| 48 kHz samples per channel | Fail without precision loss | Requires 96 kHz DSP scheduling/decimation |
| DSP instruction budget | Not applicable after payload failure | Fail for the required production graph |
| Ordinary 48 kHz playback | Unchanged | Fail without a new clock/rate adapter |
| AHIRecord monitoring | Incomplete | Incomplete without that adapter |
| Gate result | Excluded | Excluded |

Because no DIG0 design satisfies every row, the mode remains excluded under
`NOT_PACKAGED_FEASIBILITY_FAIL`. This is a completed local feasibility
result, not a failed hardware transport test, and it does not block the
I2S/TDM8 bridge or the consolidated characterization session.
