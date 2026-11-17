# Licensing

The firmware is licensed under GPL-3.0-or-later; the full text is in
[LICENSE](LICENSE). This file covers three things: how that licence applies to
programs that use the card, how programs may call the SDK's Amiga libraries,
and which SDK files are under MIT instead.

## Programs that use the card

The firmware licence does not cover programs that use the ZZ9000 through its
published interfaces. That is normal use of the card, not a derived work of the
firmware. Such a program can use any licence, including a closed one. The
published interfaces are:

- the documented mailbox and register interface, for Amiga programs and drivers
  that talk to the firmware directly;
- the app API table, for ARM apps that link only `libzzapp`. GL reached through
  `zz_gl_bind` is part of that table.

## Programs that call zz9k.library or mpega.library

`zz9k.library` and `mpega.library` are GPL-3.0-or-later. As an additional
permission under section 7 of the GPL, the copyright holder of both libraries
allows any program to open them with `OpenLibrary()` and call their public
functions through the MIT headers and fd files listed below, whatever that
program's licence. Calling the libraries this way does not make the program
subject to the GPL.

The permission covers calling the libraries. Copying or building in their source
or object code, including `zz9k_host.o`, stays under the GPL.

Neither section changes the licence of any file. A program that copies or
links GPL code stays bound by the GPL. That includes the library sources,
`zz9k_host.o`, firmware objects, `libzzm_rt`, and the GL front-end objects.

## MIT files

These files are under the MIT licence ([sdk/LICENSES/MIT.txt](sdk/LICENSES/MIT.txt)),
so a program under any licence can compile them in:

- `sdk/amiga/fd/`: the fd files for `zz9k.library` and `mpega.library`.
- `sdk/amiga/include/`: the `clib`, `proto`, `inline`, `pragmas`, `libraries`,
  and `zz9k` headers, including the header-only helpers such as
  `zz9k/event_wait.h`.
- `sdk/include/zz9k/`: the public SDK headers, such as `zz9k/image.h` and
  `zz9k/abi.h`. Two files here stay GPL: `module.h` and `service_registry.h`
  describe firmware modules, not the client side.
- `sdk/host/include/zz9k/`: `host.h`, `request.h`, and `reply.h`. The
  `zz9k_host.o` implementation behind `host.h` stays GPL.
- `sdk/examples/amiga-*`: the Amiga example programs.
- `sdk/zzapp/`: the ARM app side, which was already MIT: `libzzapp` (`src/`),
  `include/zzapp.h` and `include/zzapp_note.h`, `crt0.S`, `zzapp.ld`, the build
  helpers (`Makefile`, `mk/`, `cmake/`, `docker/`, `build_pic_newlib.sh`), the
  template sources, and the host runner sources. The `tools/zzapp-check`
  checker stays GPL; running it on an app does not affect the app's licence.

Each of these source files carries `SPDX-License-Identifier: MIT`.

## Everything else

The rest of the repository is GPL-3.0-or-later unless a file says otherwise.
That covers the firmware, `zz9k.library` and `mpega.library` themselves, the
DataTypes, and the tools. The GL front end that the firmware compiles is
`PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later`. Third-party code keeps its
own licence, for example `sdk/tools/lha-unix/`.
