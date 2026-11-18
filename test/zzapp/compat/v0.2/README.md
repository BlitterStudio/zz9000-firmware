<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# API 0.2 loader fixtures

`hello.elf`, `console.elf`, `pixels.elf`, `fault.elf`, `tone.elf`,
`filter.elf`, and `echo.elf` are the API 0.2 templates
`sdk/zzapp/templates/{hello,console,pixels,fault,tone,filter,echo}.c` as they
stood at firmware commit `94037ca6`. The SDK has since moved to
[BlitterStudio/aar](https://github.com/BlitterStudio/aar); the path above and
the command below belong to the firmware tree at that commit.

They were exported with `git archive 94037ca6` and built from that exported
tree with the `zz9000-app-sdk:ci` image:

```text
docker run --rm -v "$PWD":/src -w /src zz9000-app-sdk:ci \
  make -C sdk/zzapp check
```

The loader must keep accepting these API 0.2 images while API 0.3 adds dynamic
heap slots and user-mode veneers.

Each ELF has a companion `.license` file. The template sources are MIT. The
linked image also contains PIC newlib (BSD-style notices in the sysroot) and
libgcc (`GPL-3.0-or-later WITH GCC-exception-3.1`). The GCC Runtime Library
Exception permits conveying the combined binary with the MIT app.
