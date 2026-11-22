# Licensing

The firmware is licensed under GPL-3.0-or-later; the full text is in
[LICENSE](LICENSE). Some SDK files are under MIT instead, and programs that use
the card or call the SDK's Amiga libraries can use any licence, as described
below.

## Programs that use the card

The firmware licence does not cover programs that use the ZZ9000 through its
published interfaces. That is normal use of the card, not a derived work of the
firmware. Such a program can use any licence, including a closed one. The
published interfaces are:

- the documented mailbox and register interface, for Amiga programs and drivers
  that talk to the firmware directly;
- the app API table, for ARM apps that link only `libaar`. GL reached through
  `aar_gl_bind` is part of that table.

## Programs that call zz9k.library or mpega.library

`zz9k.library` and `mpega.library` are GPL-3.0-or-later. As an additional
permission under section 7 of the GPL, the copyright holder of both libraries
allows any program to open them with `OpenLibrary()` and call their public
functions through the MIT headers and fd files listed below, whatever that
program's licence. Calling the libraries this way does not make the program
subject to the GPL.

The permission covers calling the libraries. Copying or building in their source
or object code, including `zz9k_host.o`, stays under the GPL.

`aar.library` is GPL-3.0-or-later as linked, and the same permission applies to
it: a program may open it with `OpenLibrary()` and call its public functions
through the MIT headers and fd file of the aar repository, whatever that
program's licence. Copying or building in its source or object code stays
under the licence of each file (see below).

Neither section changes the licence of any file. A program that copies or
links GPL code stays bound by the GPL. That includes the library sources,
`zz9k_host.o`, firmware objects, `libzzm_rt`, and the GL front-end objects.

## MIT files

These files are under the MIT licence ([sdk/LICENSES/MIT.txt](sdk/LICENSES/MIT.txt)),
so a program under any licence can compile them in. The SDK package ships the
MIT text as `Developer/LICENSE-MIT.txt`, the GPL text as
`Developer/LICENSE-GPL-3.0.txt`, and this file as `Docs/LICENSING.guide`.

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

Each of these source files carries `SPDX-License-Identifier: MIT`.

The ARM app SDK is not in this repository any more. It lives in
[BlitterStudio/aar](https://github.com/BlitterStudio/aar), where every file is
MIT, and this repository pins the commit it builds against in `sdk/AAR_REF`.
The fetched copy in `sdk/aar/` is not part of this repository.

`aar.library` is built here from two sources: the portable core, the AmigaOS
shell and the image probe in `client/src/` of that repository (MIT, with the
`client/include` headers and `client/fd/aar_lib.fd` that programs compile
against), and `sdk/amiga/src/aar_zz9000.[ch]` of this repository, the ZZ9000
platform layer, which includes the private wire `zzapp_proto.h` and stays
GPL-3.0-or-later. The linked `Libs/aar.library` binary is therefore
GPL-3.0-or-later. A program that opens it with `OpenLibrary()` and calls its
vectors through the MIT headers is covered by the same additional permission
as `zz9k.library` (above): it can use any licence. The packaged SDK
ships the aar headers and fd file under the MIT licence
(`Developer/LICENSE-MIT.txt`).

`aar-run`, the reference launcher, and `libaarserve.a`, the serving loop it is
built on, are MIT. The SDK build compiles them from `client/run` and
`client/serve` of the pinned aar tree and links nothing from this repository:
they call `aar.library` through its vectors and do not link the GPL platform
layer. The package ships `C/aar-run` and `Developer/Lib/libaarserve.a` with the
MIT text in `Developer/LICENSE-MIT.txt`.

## Everything else

The rest of the repository is GPL-3.0-or-later unless a file says otherwise.
That covers the firmware, `zz9k.library` and `mpega.library` themselves, the
DataTypes, and the tools. The GL front end that the firmware compiles is
`PolyForm-Noncommercial-1.0.0 OR GPL-3.0-or-later`. Third-party code keeps its
own licence, for example `sdk/tools/lha-unix/`. `sdk/tools/libpt4/`, the
ProTracker 4.1 replayer used by the voice engine tests and `zz9k-voiceplay`,
is under the zlib licence, and so are the test modules in
`test/audio/fixtures/pt4/`; `sdk/tools/libpt4/ORIGIN.md` names the upstream
commit.
