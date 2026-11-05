# ZZApp templates

All templates are MIT-licensed `zz_main` programs built by `make -C sdk/zzapp`.

- `hello.c`: log-ring smoke test.
- `pixels.c`: display/input example.
- `fault.c`: fault lifecycle test.
- `zzmembench.c`: allocator benchmark.
- `mgl_conformance.c`: MiniGL conformance client.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  ZZRun file proxy, computes SHA-256 with the card service, and writes the
  32-byte digest to its output path. Invoke it as `filter input output`.

Every resulting ELF is included in `check-relocs`; stock GNU ld `.rel.dyn`
output is accepted directly by the runtime.
