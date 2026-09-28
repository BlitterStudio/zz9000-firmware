/* libnix printf format probe: prints the exact specifier shapes the
 * ZZPlay exit path uses. If this gurus where the (v)printf("%llu")
 * line runs, libnix's printf mishandles the %ll length modifier on
 * m68k and every %llu in a tool binary is a crash waiting for EOF.
 * Plain run first (direct printf), then the va_list (vprintf) form.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>

static void info(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  (void)vprintf(format, args);
  va_end(args);
}

int main(void)
{
  printf("fmttest: plain %%lu            -> %lu\n", 1234567UL);
  printf("fmttest: plain %%lu.u          -> %lu.%03lu\n", 12UL, 345UL);
  printf("fmttest: plain %%llu (64-bit)  -> %llu\n",
         12345678901ULL);
  printf("fmttest: plain %%c            -> %c\n", 'P');
  info("fmttest: vprintf %%lu          -> %lu\n", 7654321UL);
  info("fmttest: vprintf %%llu         -> %llu\n", 12345678901ULL);
  info("fmttest: vprintf 16-arg line  -> %lu %lu %lu %lu %lu %lu "
       "%lu %lu %ld %ld %c %lu %lu %lu %lu %lu\n",
       1UL, 2UL, 3UL, 4UL, 5UL, 6UL, 7UL, 8UL, -9L, -10L, 'F',
       11UL, 12UL, 13UL, 14UL, 15UL);
  printf("fmttest: done\n");
  return 0;
}
