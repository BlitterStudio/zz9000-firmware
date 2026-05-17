# RTG Regression Harness

This host-side harness compiles the production `gfx.c` and compares RTG drawing
operations against independent reference implementations on deterministic test
buffers.

Run correctness tests:

```sh
make -C test/rtg test
```

Run repeatable host micro-benchmarks:

```sh
make -C test/rtg bench
```

The benchmarks are for comparing one firmware revision to the previous revision
on the same host. Hardware validation still needs on-card testing before a
performance commit is considered proven.
