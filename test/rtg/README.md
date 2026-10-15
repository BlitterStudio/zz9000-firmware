# RTG regression harness

This host-side harness compiles the production `gfx.c` and checks RTG drawing
operations against independent reference implementations on deterministic
buffers. It also checks the pitch contract used by the drawing code.

## Run

Run the checks from the repository root:

```sh
make -C test/rtg test
```

Run the host micro-benchmark with:

```sh
make -C test/rtg bench
```

`test` runs the regression executable and `pitch_contract_test.py`. `bench`
runs the same executable with `--bench`.

The host build compiles only the scalar paths in `gfx.c`. To run the same
checks on the NEON paths, built for the Cortex-A9 with the firmware's CPU
flags and run under `qemu-arm` in Docker:

```sh
test/rtg/neon_run.sh
```

## Benchmark limits

Compare benchmark results only between firmware revisions on the same host.
Validate any performance change on a card before treating it as hardware
performance.
