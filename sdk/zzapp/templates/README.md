# ZZApp templates

All templates are MIT-licensed `zz_main` programs built as PIE ELF files by
`make -C sdk/zzapp`. Set `ZZAPP_PIC_SYSROOT` to the output of
`build_pic_newlib.sh`, then run `make -C sdk/zzapp check-relocs` before copying
an ELF to the Amiga. Stock GNU ld `.rel.dyn` output is accepted directly by the
runtime.

- `hello.c`: writes one diagnostic line through `zz_log`.
- `pixels.c`: opens a display surface and handles input.
- `fault.c`: deliberately faults for lifecycle testing.
- `zzmembench.c`: allocator benchmark.
- `mgl_conformance.c`: MiniGL conformance client.
- `filter.c`: file-in/file-out Shell template. It maps an input through the
  ZZRun file proxy, computes SHA-256 with the card service, and writes the
  32-byte digest to its output path. Invoke it as `filter input output`.
- `tone.c`: opens the normal stereo S16LE `zzapp_audio` ring and produces a
  440 Hz square wave. It is route-agnostic: ZZRun's default card-fabric route
  and its `AUDIO=AHI` route use the same app binary.
- `echo.c`: echoes each fixed 32-byte client message using `zz_msg_poll` and
  `zz_msg_send`; use it with the `zz9k.library` app-client smoke tool.

Do not include `zzapp_proto.h` in an app. The public API is `zzapp.h`.
