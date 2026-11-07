<!--
SPDX-FileCopyrightText: 2026 Dimitris Panokostas <midwan@gmail.com>
SPDX-License-Identifier: MIT
-->

# API 0.1 loader fixtures

`hello.elf`, `pixels.elf`, and `fault.elf` are the API 0.1 templates
`sdk/zzapp/templates/{hello,pixels,fault}.c` as they stood at firmware
commit `4b4e3198` (`Keep the opt-in loader file check out of test
auto-discovery`), before the API 0.2 merge.

They were linked with Arm GNU Toolchain 13.2.rel1 (13.2.1 20231009) and
PIC newlib 4.3.0.20230120, then rewritten by that commit's `rela_dyn.py`.
The allocated relocation section is `SHT_RELA` (`.rela.dyn`), not the
`SHT_REL` output stock GNU ld emits today. The loader must keep accepting
that RELA form.

Each ELF has a companion `.license` file. The app sources at `4b4e3198`
are MIT. The linked image also contains PIC newlib (BSD-style notices in
the sysroot) and libgcc from the Arm GNU toolchain
(`GPL-3.0-or-later WITH GCC-exception-3.1`). The GCC Runtime Library
Exception allows the combined binary to be conveyed with the MIT app.
newlib's tree contains additional permissive notices beyond a single
BSD-3-Clause grant.
