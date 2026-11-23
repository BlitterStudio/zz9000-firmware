/* Host check of the ZZM_LOAD_BENCH report format (one tick is one us). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "zzm_load_bench.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d\n", __FILE__, __LINE__); } } while (0)

int main(void)
{
    char line[256];
    const char *expect =
        "ZZM-BENCH mod=MPEG2 open=12us read=1480us bss=0us dflush=0us "
        "iflush=0us init=251340us fail=0us steps=5 t=5000ms";

    CHECK(zzm_load_bench_format(line, sizeof(line), 1U, 0U) == 0);
    zzm_load_bench_record(1U, "MPEG2", ZZM_LOAD_BENCH_OPEN, 12U);
    zzm_load_bench_record(1U, "MPEG2", ZZM_LOAD_BENCH_READ_HASH, 1480U);
    zzm_load_bench_record(1U, "MPEG2", ZZM_LOAD_BENCH_READ_HASH, 900U);
    zzm_load_bench_record(1U, "MPEG2", ZZM_LOAD_BENCH_INIT, 251340U);
    zzm_load_bench_record(1U, "MPEG2", ZZM_LOAD_BENCH_INIT, 7U);
    CHECK(zzm_load_bench_format(line, sizeof(line), 1U, 5000U) ==
          (int)strlen(expect));
    CHECK(strcmp(line, expect) == 0);
    if (strcmp(line, expect) != 0)
        printf("got: %s\n", line);
    /* Out-of-range ids are ignored rather than corrupting the tables. */
    zzm_load_bench_record(ZZM_LOAD_BENCH_MODULES, "X", ZZM_LOAD_BENCH_OPEN, 1U);
    zzm_load_bench_record(0U, "X", ZZM_LOAD_BENCH_PHASES, 1U);
    CHECK(zzm_load_bench_format(line, sizeof(line), 0U, 0U) == 0);
    return failures != 0;
}
