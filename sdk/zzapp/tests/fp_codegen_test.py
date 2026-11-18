#!/usr/bin/env python3
"""Check the code generation claims in the developer guide's bit-identical
maths section against the installed arm-none-eabi-gcc. No pytest.

Set CROSS to the toolchain prefix (default arm-none-eabi-). A claim that the
compiler stops honouring fails here, so a toolchain bump cannot change float
results unnoticed.
"""
import os
import re
import subprocess
import sys
import tempfile

CC = os.environ.get("CROSS", "arm-none-eabi-") + "gcc"
# The SDK's float flags from mk/zzapp.mk.
SDK = ["-mcpu=cortex-a9", "-marm", "-mfpu=neon", "-mfloat-abi=hard", "-fPIE",
       "-O2", "-fno-strict-aliasing", "-std=c11"]

KERNELS = r"""
#include <math.h>
float muladd_f(float a, float b, float c) { return a * b + c; }
double muladd_d(double a, double b, double c) { return a * b + c; }
float fused_f(float a, float b, float c) { return __builtin_fmaf(a, b, c); }
void add_arr(float *restrict d, const float *restrict a,
             const float *restrict b, int n)
{
    int i;
    for (i = 0; i < n; ++i)
        d[i] = a[i] + b[i];
}
float dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < n; ++i)
        s += a[i] * b[i];
    return s;
}
float folded(void) { return sinf(0.5f); }
"""

NEON_MLA = """#include <arm_neon.h>
float32x4_t f(float32x4_t a, float32x4_t b, float32x4_t c)
{ return vmlaq_f32(a, b, c); }
"""

NEON_FMA = """#include <arm_neon.h>
float32x4_t f(float32x4_t a, float32x4_t b, float32x4_t c)
{ return vfmaq_f32(a, b, c); }
"""


def compile_c(source, flags):
    with tempfile.TemporaryDirectory() as root:
        src = os.path.join(root, "t.c")
        with open(src, "w", encoding="ascii") as fh:
            fh.write(source)
        proc = subprocess.run([CC] + flags + ["-S", "-o", "-", src],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True, check=False)
    return proc.returncode, proc.stdout, proc.stderr


def function(asm, name):
    match = re.search(r"^%s:\n(.*?)\t\.size\t%s," % (name, name), asm,
                      re.S | re.M)
    if not match:
        raise SystemExit("function %s not found in assembly" % name)
    return match.group(1)


def check(cond, what):
    if not cond:
        raise SystemExit("fp codegen: %s" % what)


def main():
    rc, asm, err = compile_c(KERNELS, SDK)
    check(rc == 0, "kernel compile failed: " + err)

    for name in ("muladd_f", "muladd_d"):
        body = function(asm, name)
        check("vfma" not in body and "vfnma" not in body,
              "%s uses a fused multiply-add" % name)
        check("vmla" in body or "vmul" in body, "%s has no VFP multiply" % name)
    check("fmaf" in function(asm, "fused_f"),
          "__builtin_fmaf is not a libm call: the target gained FMA")
    for contract in ("off", "fast"):
        rc2, asm2, _ = compile_c(KERNELS, SDK + ["-ffp-contract=" + contract])
        check(rc2 == 0, "compile with -ffp-contract=%s" % contract)
        check(function(asm2, "muladd_f") == function(asm, "muladd_f"),
              "-ffp-contract=%s changes muladd_f" % contract)

    o3 = [f for f in SDK if f != "-O2"] + ["-O3"]
    rc, asm3, err = compile_c(KERNELS, o3)
    check(rc == 0, "-O3 compile failed: " + err)
    for name in ("add_arr", "dot"):
        body = function(asm3, name)
        check(not re.search(r"\tv(add|mla|mul)\.f32\tq", body),
              "-O3 moved %s onto NEON q registers without fast-math" % name)
    rc, asm4, err = compile_c(KERNELS, o3 + ["-ffast-math"])
    check(rc == 0, "-ffast-math compile failed: " + err)
    check(re.search(r"\tvadd\.f32\tq", function(asm4, "add_arr")),
          "control: -ffast-math no longer vectorises add_arr, so the check "
          "above proves nothing")

    check(re.search(r"vldr\.32\ts0, \.L", function(asm, "folded")),
          "sinf(0.5f) is no longer folded")
    rc, asm5, err = compile_c(KERNELS, SDK + ["-fno-builtin"])
    check(rc == 0 and "sinf" in function(asm5, "folded"),
          "-fno-builtin does not call sinf")

    rc, asm6, err = compile_c(NEON_MLA, SDK)
    check(rc == 0 and re.search(r"\tvmla\.f32\tq", asm6),
          "vmlaq_f32 is not a NEON multiply-accumulate")
    check("vfma" not in asm6, "vmlaq_f32 became a fused operation")
    rc, _, err = compile_c(NEON_FMA, SDK)
    check(rc != 0 and "vfmaq_f32" in err,
          "vfmaq_f32 compiles with -mfpu=neon: the target gained FMA")

    sys.stdout.write("fp codegen: ok\n")


if __name__ == "__main__":
    main()
