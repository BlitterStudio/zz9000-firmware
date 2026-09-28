/* dos.library file-write probe: exercises exactly what the ZZPlay
 * --trace path does, one step at a time with console markers between
 * them, to T: and then RAM:. If the machine dies, the last printed
 * marker names the call that killed it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <proto/dos.h>
#include <stdio.h>

static int probe(const char *name, const char *path)
{
  BPTR fh;
  LONG wrote;

  printf("%s: Open(MODE_NEWFILE)...\n", name);
  fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
  if (!fh) {
    printf("%s: Open failed (non-fatal)\n", name);
    return 0;
  }
  printf("%s: Open ok, Write header...\n", name);
  wrote = Write(fh, (APTR) "# probe header\n", 15);
  printf("%s: header Write -> %ld\n", name, (long)wrote);
  printf("%s: Write line 1...\n", name);
  wrote = Write(fh, (APTR) "F 1 t=0 v=0 m=0 dr=0 d=P dec=1\n", 31);
  printf("%s: line 1 Write -> %ld\n", name, (long)wrote);
  printf("%s: Close...\n", name);
  Close(fh);
  printf("%s: done\n", name);
  return 0;
}

int main(void)
{
  probe("T:", "T:fmttest2.trace");
  probe("RAM:", "RAM:fmttest2.trace");
  printf("fmttest2: all steps passed\n");
  return 0;
}
