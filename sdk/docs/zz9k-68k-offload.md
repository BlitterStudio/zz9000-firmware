# ZZ9000 68k offload experiments

The SDK contains an experimental 68k runner contract, an offload comparison
model, and a benchmark tool. The runner interprets a small 68k instruction
subset in caller-supplied memory. The model compares that work with native
execution and a purpose-built ARM service. `zz9k-m68kbench` measures the
native checksum and copy loops, then accepts supplied offload timings.

These components do not send work to the ZZ9000, define a mailbox request, or
run an AmigaOS process on the ARM. The runner is currently used by its contract
test only.

## 68k runner

`tools/zz9k-m68k-runner.h` defines the runner state and
`tools/zz9k-m68k-runner.c` implements it. Call
`zz9k_m68k_state_init()` to clear a `ZZ9KM68KState`, configure the state and
memory, then call `zz9k_m68k_run()`.

| Input or result | Meaning |
| --- | --- |
| `state.d[8]` | Data registers `D0` through `D7` |
| `state.a[8]` | Address registers `A0` through `A7` |
| `state.pc` | Offset of the next instruction in `memory` |
| `state.sr` | Status register; the runner updates condition-code flags for supported instructions |
| `memory`, `memory_size` | Mutable byte array and its size. Instruction fetches, data accesses, and absolute-long addresses use offsets in this array. Multi-byte values are big-endian. |
| `instruction_limit` | Maximum completed instructions. A limit of zero returns `ZZ9K_M68K_STATUS_LIMIT` before an instruction fetch. |
| `result` | Status, completed instruction count, and fault details. It may be `NULL`. |

Set `A7` to an in-range stack offset before executing `RTS`. The runner reads a
big-endian return address from `memory[A7]`, increments `A7` by four, assigns
that address to `PC`, and returns `ZZ9K_M68K_STATUS_RTS`.

### Supported instructions

| Instruction | Supported form |
| --- | --- |
| `MOVEQ` | `MOVEQ #imm,Dn` |
| `ADDQ.L` | `ADDQ.L #n,Dn`, with 68k quick values 1 through 8 |
| `SUBQ.L` | `SUBQ.L #n,Dn`, with 68k quick values 1 through 8 |
| `ADD.B` | `ADD.B (An)+,Dn` |
| `MOVE.B` | `MOVE.B (An)+,(Am)+` |
| `DBRA` | `DBRA Dn,disp16` |
| `MOVE.L` | `MOVE.L Dn,(xxx).L` and `MOVE.L (xxx).L,Dn` |
| `RTS` | Stack return through `A7` |

The runner returns these status values:

| Status | Value | Condition |
| --- | ---: | --- |
| `ZZ9K_M68K_STATUS_RTS` | 0 | `RTS` completed. |
| `ZZ9K_M68K_STATUS_LIMIT` | 1 | The instruction limit was reached. |
| `ZZ9K_M68K_STATUS_ILLEGAL` | 2 | The opcode is outside the supported subset. `fault_pc` and `opcode` identify it. |
| `ZZ9K_M68K_STATUS_OOB` | 3 | An instruction fetch, extension word, stack access, or data access was outside `memory`. |
| `ZZ9K_M68K_STATUS_BAD_ARGUMENT` | 4 | `state` or `memory` was `NULL`. |

The contract tests include two loops. The checksum fixture starts with `A0` at
the input, `D1` set to `length - 1`, and `D0` as the accumulator. It uses
`ADD.B (A0)+,D0` and `DBRA`; the checksum is the low byte of `D0`. The copy
fixture uses `A0` as the source, `A1` as the destination, and
`MOVE.B (A0)+,(A1)+` with `DBRA`. Both fixtures also check the instruction
limit and the postincremented address registers.

## Comparing execution choices

`tools/zz9k-m68k-offload-model.h` calculates total time in microseconds for
three choices:

| Choice | Total cost |
| --- | --- |
| Native 68k | `native_m68k_micros` |
| Runner | `mailbox_micros + transfer_micros + arm_runner_micros` |
| ARM service | `mailbox_micros + transfer_micros + arm_service_micros` |

A zero native measurement produces
`ZZ9K_M68K_OFFLOAD_CHOICE_UNMEASURED`. A zero runner or service compute time
means that path is unmeasured and excludes it from selection. The model starts
with native execution, selects an ARM service only when its total is lower,
and selects the runner only when its total is lower still. Equal runner and
service totals select the service. The `bytes` field is used for the KiB/s
calculation and does not affect choice.

## Measuring a workload

Build the `zz9k_m68kbench` target. The resulting `zz9k-m68kbench` tool times
its checksum and byte-copy loops, prints microseconds per operation and KiB/s,
and prints a model row for each workload.

Run it without arguments for 128 iterations of 4096 bytes. The first two
arguments are clamped to 1 through 4096 iterations and 1 through 65536 bytes.
The remaining values are supplied measurements in microseconds.

```text
zz9k-m68kbench [iterations] [bytes] [mailbox_us] [transfer_us] [runner_us] [service_us]
```

For example, enter matching measurements for a 4096-byte candidate:

```text
zz9k-m68kbench 128 4096 6000 120 900 300
```

Leave the offload arguments at zero when those measurements are unavailable.
The output remains a native baseline; both offload paths report zero total time
and are excluded from the choice.

## Limits and status

The runner has no support for AmigaOS traps, library calls, interrupts,
supervisor or user emulation, self-modifying-code cache invalidation,
custom-chip access, or direct access to the host address space. It also has no
mailbox or transfer implementation. Treat it as a contract test and comparison
prototype, not as a transparent offload facility.
