/*
 * Floating-point environment probe. Prints FPSCR as the app sees it on
 * entry and checks the two denormal behaviours the developer guide's
 * bit-identical maths section relies on: a VFP add keeps a denormal result,
 * and a NEON add flushes it to zero. Exit status 0 means both held and
 * FPSCR was 0 on entry. ARM only: it reads FPSCR with VMRS.
 *
 * SPDX-License-Identifier: MIT
 */
#include "zzapp.h"
#include "zzapp_note.h"

#include <arm_neon.h>

ZZAPP_NOTE(0, 0)

static uint32_t read_fpscr(void)
{
    uint32_t v;

    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}

static char *put_str(char *out, const char *s)
{
    while (*s)
        *out++ = *s++;
    return out;
}

static char *put_hex(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4)
        *out++ = digits[(v >> shift) & 0xfu];
    return out;
}

static int report(const char *what, uint32_t value, int ok)
{
    char line[64];
    char *end = put_str(line, "fpenv: ");

    end = put_str(end, what);
    end = put_hex(end, value);
    end = put_str(end, ok ? " ok\n" : " FAIL\n");
    zz_log(line, (uint32_t)(end - line));
    return ok;
}

static uint32_t bits_of(float f)
{
    union {
        float f;
        uint32_t u;
    } v;

    v.f = f;
    return v.u;
}

int zz_main(int argc, char **argv)
{
    /* Read FPSCR before any floating-point work in the app. */
    uint32_t entry = read_fpscr();
    union {
        uint32_t u;
        float f;
    } tiny;
    volatile float denormal;
    volatile float vfp_sum;
    float32x4_t quad;
    float neon_sum;
    int ok = 1;

    (void)argc;
    (void)argv;

    tiny.u = 1u; /* the smallest positive denormal */
    denormal = tiny.f;
    vfp_sum = denormal + denormal;
    quad = vaddq_f32(vdupq_n_f32(denormal), vdupq_n_f32(denormal));
    neon_sum = vgetq_lane_f32(quad, 0);

    ok &= report("entry fpscr=", entry, entry == 0u);
    ok &= report("vfp denormal sum=", bits_of(vfp_sum), bits_of(vfp_sum) == 2u);
    ok &= report("neon denormal sum=", bits_of(neon_sum), bits_of(neon_sum) == 0u);
    return ok ? 0 : 1;
}
