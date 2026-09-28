# ZZ9000 68k Offload Exploration

This is a research track for running selected 68k code on the ZZ9000 ARM side.
The first SDK slice is deliberately small: it defines and tests a sandboxed
function-runner contract, backed by a minimal interpreter. It is not a promise
of transparent Workbench or CLI process execution.

The near-term target is opt-in code:

- synthetic 68k functions used as ABI tests
- pure compute routines with explicit input/output memory windows
- library entry points that can be wrapped with known registers, stack, and
  memory lifetime

The first runner lives in `tools/zz9k-m68k-runner.c`. It executes from a caller
provided big-endian memory window with explicit `D0-D7`, `A0-A7`, `PC`, and
`SR` state. The current instruction subset is only large enough to pin the
contract:

- `MOVEQ #imm,Dn`
- `ADDQ.L #n,Dn`
- `SUBQ.L #n,Dn`
- `ADD.B (An)+,Dn`
- `MOVE.B (An)+,(Am)+`
- `DBRA Dn,disp16`
- `MOVE.L Dn,(xxx).L`
- `MOVE.L (xxx).L,Dn`
- `RTS`
- illegal-instruction and out-of-bounds reporting
- instruction-limit timeout reporting

This lets tests cover the hard boundary concerns before a real JIT is chosen:
register state, CCR updates, stack return behavior, big-endian memory access,
fault reporting, and bounded execution. A later Emu68-derived, Amiberry-derived,
or custom JIT should be able to replace the execution engine behind the same
contract.

The first real loop fixture is a byte checksum: `A0` points at an input buffer,
`D1` holds `length - 1`, `ADD.B (A0)+,D0` accumulates each byte, and `DBRA`
terminates the loop. The result is the low byte of `D0`; the test also checks
that `A0` advanced by the input length and that instruction-limit timeouts stop
inside the loop without losing state.

The second fixture is a byte copy loop: `A0` points at the source buffer, `A1`
points at the destination buffer, `MOVE.B (A0)+,(A1)+` copies one byte per
iteration, and `DBRA` terminates the loop. This pins both source and
destination postincrement behavior before using the runner for any benchmark
comparison.

The comparison gate lives in `tools/zz9k-m68k-offload-model.h`. It takes
measured microsecond costs for the same workload:

- native 68k execution
- mailbox and input/output transfer overhead
- ARM-side execution through the 68k runner
- ARM-side execution through a purpose-built service

Zero offload compute costs mean "not measured", not "free". Native 68k wins
when mailbox overhead dominates. A purpose-built ARM service wins ties against
the generic 68k runner, because the runner is only worth expanding when it
beats both native 68k and the simpler service-specific implementation.

The first data collector is `zz9k-m68kbench`, packaged in `C/`. It measures
native checksum and byte-copy loops on the current CPU and prints per-workload
microseconds plus KiB/s. Optional positional arguments let a hardware session
paste in matching offload measurements:

```text
zz9k-m68kbench [iterations] [bytes] [mailbox_us] [transfer_us] [runner_us] [service_us]
```

For example, after measuring a 4096-byte service candidate, run:

```text
zz9k-m68kbench 128 4096 6000 120 900 300
```

The tool reports a model choice for the checksum and copy rows. Until real
runner/service timings exist, leave the optional offload timings at zero; the
output then acts as the native 68k baseline to compare later measurements
against.

Out of scope for this slice:

- AmigaOS traps, library calls, interrupts, and supervisor/user emulation
- self-modifying-code cache invalidation
- transparent process offload
- custom-chip access or direct host address-space access
