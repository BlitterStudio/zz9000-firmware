#!/usr/bin/env python3
"""Feed link maps to zzapp-check --no-libc. No pytest.

The maps are cut down from real GNU ld output: the archive member section
and a memory map line, with the archive paths a build produces.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CHECK = os.path.join(ROOT, "tools", "zzapp-check")

HEAD = """Archive member included to satisfy reference by file (symbol)

"""

FREESTANDING = HEAD + """build/freestanding/libzzapp.a(zzapp.o)
                              build/freestanding/templates/freestanding.o (zz_log)
build/freestanding/libzzapp.a(freestanding.o)
                              build/freestanding/templates/freestanding.o (memcpy)
/opt/arm/lib/gcc/arm-none-eabi/15.3.1/libgcc.a(_udivsi3.o)
                              build/freestanding/templates/freestanding.o (__aeabi_uidiv)

Linker script and memory map

 .text.zz_log   0x00000100       0x20 build/freestanding/libzzapp.a(zzapp.o)
"""

CMAKE_OBJECTS = HEAD + """/opt/arm/lib/gcc/arm-none-eabi/15.3.1/libgcc.a(_udivsi3.o)
                              CMakeFiles/app.dir/templates/freestanding.c.obj (__aeabi_uidiv)

Linker script and memory map

 .text.zz_log   0x00000100       0x20 CMakeFiles/app.dir/src/zzapp.c.obj
"""

NEWLIB = FREESTANDING.replace(
    "\nLinker script",
    "/opt/zzapp-pic-newlib/arm-none-eabi/lib/libc.a(lib_a-malloc.o)\n"
    "                              build/hello/templates/hello.o (malloc)\n"
    "\nLinker script")

LIBM_ONLY = FREESTANDING.replace(
    "\nLinker script",
    "libm.a(lib_a-sf_sin.o)\n"
    "                              build/hello/templates/hello.o (sinf)\n"
    "\nLinker script")

WINDOWS_PATH = FREESTANDING.replace(
    "\nLinker script",
    "C:\\\\sysroot\\\\arm-none-eabi\\\\lib\\\\libnosys.a(_exit.o)\n"
    "                              (_exit)\n"
    "\nLinker script")


def run(text):
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "app.map")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        return subprocess.run([sys.executable, CHECK, "--no-libc", path],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              check=False)


def expect(name, text, ok, needle=b""):
    proc = run(text)
    out = proc.stdout + proc.stderr
    if (proc.returncode == 0) != ok or needle not in out:
        sys.stderr.write(out.decode("utf-8", "replace"))
        raise SystemExit("%s: exit %d, wanted %s containing %r"
                         % (name, proc.returncode,
                            "success" if ok else "failure", needle))


def main():
    expect("freestanding map", FREESTANDING, True)
    expect("cmake map, objects only", CMAKE_OBJECTS, True)
    expect("newlib member", NEWLIB, False, b"libc.a(lib_a-malloc.o)")
    expect("libm member", LIBM_ONLY, False, b"libm.a(lib_a-sf_sin.o)")
    expect("libnosys member", WINDOWS_PATH, False, b"libnosys.a")
    expect("not a link map", "Memory Configuration\n", False, b"not a GNU ld link map")
    proc = subprocess.run([sys.executable, CHECK, "--no-libc"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)
    if proc.returncode == 0:
        raise SystemExit("missing map argument accepted")
    print("zzapp-check --no-libc: ok")


if __name__ == "__main__":
    main()
