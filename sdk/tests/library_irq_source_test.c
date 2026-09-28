/*
 * Source guard for resident zz9k.library completion IRQ waiting.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path)
{
  FILE *file;
  long length;
  char *data;

  file = fopen(path, "rb");
  if (!file) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  length = ftell(file);
  if (length < 0) {
    fclose(file);
    return 0;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }

  data = (char *)malloc((size_t)length + 1U);
  if (!data) {
    fclose(file);
    return 0;
  }
  if (fread(data, 1U, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return 0;
  }

  data[length] = '\0';
  fclose(file);
  return data;
}

static int expect_contains(const char *source, const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("missing %s\n", needle);
  return 0;
}

static int expect_not_contains(const char *source, const char *needle)
{
  if (!strstr(source, needle)) {
    return 1;
  }

  printf("unexpected %s\n", needle);
  return 0;
}

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <zz9k_library_resident.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(source, "struct Interrupt irq");
  ok &= expect_contains(source, "zz9k_lib_irq_isr");
  ok &= expect_contains(source, "AddIntServer");
  ok &= expect_contains(source, "RemIntServer");
  ok &= expect_contains(source, "INTB_EXTER");
  ok &= expect_contains(source, "INTB_PORTS");
  ok &= expect_contains(source, "ZZ9K_INT2");
  ok &= expect_not_contains(source, "ZZ9K_SDK_INT2");
  ok &= expect_contains(source, "zz9k_interrupt_status");
  ok &= expect_contains(source, "ZZ9K_INTERRUPT_SDK");
  ok &= expect_contains(source, "zz9k_completion_irq_ack");
  ok &= expect_contains(source, "Signal(base->irq_task");
  ok &= expect_contains(source, "ZZ9KSetEventWaiter");
  ok &= expect_contains(source, "zz9k_completion_irq_enable(base->core.ctx, 1)");
  ok &= expect_contains(source, "zz9k_completion_irq_enable(base->core.ctx, 0)");
  ok &= expect_contains(source, "TR_ADDREQUEST");
  ok &= expect_contains(source, "Wait(");
  ok &= expect_contains(source, "SIGBREAKF_CTRL_C");
  ok &= expect_contains(source, "zz9k_lib_dispatcher_proc");
  ok &= expect_contains(source, "zz9k_lib_dispatcher_ensure");
  ok &= expect_contains(source, "zz9k_lib_dispatcher_stop");
  ok &= expect_contains(source, "CreateNewProcTags");
  ok &= expect_contains(source, "NP_Entry");
  ok &= expect_contains(source, "struct DosLibrary *DOSBase");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"dos.library\"");
  ok &= expect_contains(source, "CloseLibrary((struct Library *)DOSBase)");
  ok &= expect_contains(source, "dispatcher_process");
  ok &= expect_contains(source, "dispatcher_signal_mask");
  ok &= expect_contains(source, "ZZ9KPoll(&base->core");
  ok &= expect_contains(source, "zz9k_lib_dispatcher_ensure(base)");

  free(source);
  return ok ? 0 : 1;
}
