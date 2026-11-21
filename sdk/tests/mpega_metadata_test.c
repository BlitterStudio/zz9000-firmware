/*
 * Source guards for the experimental ZZ9000-backed mpega.library surface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ExpectedFunction {
  const char *fd_line;
  const char *proto_decl;
  const char *inline_token;
} ExpectedFunction;

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

static int expect_contains(const char *label, const char *source,
                           const char *needle)
{
  if (strstr(source, needle)) {
    return 1;
  }

  printf("%s: missing %s\n", label, needle);
  return 0;
}

/* Scans the function lines of an .fd file (the lines with an argument list).
 * Returns the position of `line` among them, or -1, and counts them in
 * *count. The vector offset of a function is 30 + 6 * position. */
static int fd_scan(const char *fd, const char *line, int *count)
{
  const char *cursor = fd;
  int position = 0;
  int found = -1;

  while (*cursor) {
    const char *end = strchr(cursor, '\n');
    size_t length = end ? (size_t)(end - cursor) : strlen(cursor);

    if (cursor[0] != '*' && cursor[0] != '#' && memchr(cursor, '(', length)) {
      if (line && found < 0 && strlen(line) == length &&
          strncmp(cursor, line, length) == 0) {
        found = position;
      }
      position++;
    }
    if (!end) {
      break;
    }
    cursor = end + 1;
  }

  *count = position;
  return found;
}

int main(int argc, char **argv)
{
  static const ExpectedFunction expected[] = {
    {"MPEGA_open(filename,ctrl)(a0,a1)",
     "MPEGA_STREAM *MPEGA_open(char *stream_name, MPEGA_CTRL *ctrl);",
     "jsr -30(a6)"},
    {"MPEGA_close(mpds)(a0)",
     "void MPEGA_close(MPEGA_STREAM *mpds);", "jsr -36(a6)"},
    {"MPEGA_decode_frame(mpds,pcm)(a0,a1)",
     "LONG MPEGA_decode_frame(MPEGA_STREAM *mpds, "
     "WORD *pcm[MPEGA_MAX_CHANNELS]);",
     "jsr -42(a6)"},
    {"MPEGA_seek(mpds,ms_time_position)(a0,d0)",
     "LONG MPEGA_seek(MPEGA_STREAM *mpds, ULONG ms_time_position);",
     "jsr -48(a6)"},
    {"MPEGA_time(mpds,ms_time_position)(a0,a1)",
     "LONG MPEGA_time(MPEGA_STREAM *mpds, ULONG *ms_time_position);",
     "jsr -54(a6)"},
    {"MPEGA_find_sync(buffer,buffer_size)(a0,d0)",
     "LONG MPEGA_find_sync(BYTE *buffer, LONG buffer_size);",
     "jsr -60(a6)"},
    {"MPEGA_scale(mpds,scale_percent)(a0,d0)",
     "LONG MPEGA_scale(MPEGA_STREAM *mpds, LONG scale_percent);",
     "jsr -66(a6)"},
    {"MPGA_decode_to(mpds,out)(a0,a1)",
     "LONG MPGA_decode_to(MPEGA_STREAM *mpds, struct MpgaOut *out);",
     "jsr -72(a6)"}
  };
  char *fd;
  char *fd_alias;
  char *library;
  char *clib;
  char *proto;
  char *inline_header;
  char *pragmas;
  size_t i;
  int ok;

  if (argc != 8) {
    printf("usage: %s <mpega_lib.fd> <mpega.fd> <libraries/mpega.h> "
           "<clib/mpega_protos.h> <proto/mpega.h> <inline/mpega.h> "
           "<pragmas/mpega_pragmas.h>\n",
           argv[0]);
    return 2;
  }

  fd = read_file(argv[1]);
  fd_alias = read_file(argv[2]);
  library = read_file(argv[3]);
  clib = read_file(argv[4]);
  proto = read_file(argv[5]);
  inline_header = read_file(argv[6]);
  pragmas = read_file(argv[7]);
  if (!fd || !fd_alias || !library || !clib || !proto ||
      !inline_header || !pragmas) {
    printf("failed to read metadata inputs\n");
    free(fd);
    free(fd_alias);
    free(library);
    free(clib);
    free(proto);
    free(inline_header);
    free(pragmas);
    return 2;
  }

  ok = 1;
  ok &= expect_contains("fd", fd, "##base _MPEGABase");
  ok &= expect_contains("fd", fd, "##bias 30");
  ok &= expect_contains("fd", fd, "##public");
  ok &= expect_contains("fd", fd, "##end");
  ok &= expect_contains("fd_alias", fd_alias, "##base _MPEGABase");
  ok &= expect_contains("fd_alias", fd_alias, "##bias 30");
  ok &= expect_contains("fd_alias", fd_alias, "##public");
  ok &= expect_contains("fd_alias", fd_alias, "##end");
  ok &= expect_contains("library", library, "#define MPEGA_VERSION 2");
  ok &= expect_contains("library", library, "#define MPEGA_MAX_CHANNELS 2");
  ok &= expect_contains("library", library, "#define MPEGA_PCM_SIZE     1152");
  ok &= expect_contains("library", library, "typedef struct MPEGA_CTRL");
  ok &= expect_contains("library", library, "struct Hook *bs_access");
  ok &= expect_contains("library", library, "typedef struct MPEGA_STREAM");
  ok &= expect_contains("library", library, "#define MPEGA_ERR_BASE     0");
  ok &= expect_contains("library", library,
                        "#define MPEGA_ERR_EOF      (MPEGA_ERR_BASE - 1)");
  ok &= expect_contains("library", library,
                        "#define MPEGA_ERR_BADFRAME (MPEGA_ERR_BASE - 2)");
  ok &= expect_contains("library", library,
                        "#define MPEGA_ERR_MEM      (MPEGA_ERR_BASE - 3)");
  ok &= expect_contains("library", library,
                        "#define MPEGA_ERR_NO_SYNC  (MPEGA_ERR_BASE - 4)");
  ok &= expect_contains("library", library,
                        "#define MPEGA_ERR_BADVALUE (MPEGA_ERR_BASE - 5)");
  ok &= expect_contains("clib", clib, "#ifndef CLIB_MPEGA_PROTOS_H");
  ok &= expect_contains("clib", clib, "#include <libraries/mpega.h>");
  ok &= expect_contains("proto", proto, "extern struct Library *MPEGABase;");
  ok &= expect_contains("proto", proto, "#include <clib/mpega_protos.h>");
  ok &= expect_contains("proto", proto, "#include <pragmas/mpega_pragmas.h>");
  ok &= expect_contains("proto", proto, "#include <inline/mpega.h>");
  ok &= expect_contains("inline", inline_header, "MPEGA_INLINE_CLOBBERS");
  ok &= expect_contains("pragmas", pragmas, "#ifndef PRAGMAS_MPEGA_PRAGMAS_H");
  ok &= expect_contains("pragmas", pragmas, "#include <clib/mpega_protos.h>");

  for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    ok &= expect_contains("fd", fd, expected[i].fd_line);
    ok &= expect_contains("fd_alias", fd_alias, expected[i].fd_line);
    ok &= expect_contains("clib", clib, expected[i].proto_decl);
    ok &= expect_contains("inline", inline_header, expected[i].inline_token);
  }
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_open         01E 9802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_close        024 801");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_decode_frame 02A 9802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_seek         030 0802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_time         036 9802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_find_sync    03C 0802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPEGA_scale        042 0802");
  ok &= expect_contains("pragmas", pragmas,
                        "#pragma libcall MPEGABase MPGA_decode_to    048 9802");
  ok &= expect_contains("library", library, "struct MpgaOut {");
  ok &= expect_contains("library", library, "#define MPGA_FMT_S16  0");
  ok &= expect_contains("library", library, "#define MPGA_FMT_S16I 1");
  ok &= expect_contains("library", library, "#define MPGA_FMT_S8   2");
  ok &= expect_contains("library", library, "#define MPGA_FMT_S8D  3");
  ok &= expect_contains("library", library, "#define MPGA_FMT_S14  4");

  /* Each function sits at the vector its place in the .fd gives. */
  for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    char jsr[32];
    int count;
    int alias_count;

    snprintf(jsr, sizeof(jsr), "jsr -%u(a6)", (unsigned)(30 + 6 * i));
    if (fd_scan(fd, expected[i].fd_line, &count) != (int)i ||
        fd_scan(fd_alias, expected[i].fd_line, &alias_count) != (int)i ||
        strcmp(jsr, expected[i].inline_token) != 0) {
      printf("vector of %s is not -%u\n", expected[i].fd_line,
             (unsigned)(30 + 6 * i));
      ok = 0;
    }
    /* Eight functions, and nothing after MPGA_decode_to. */
    if (count != 8 || alias_count != 8) {
      printf("the .fd files do not hold eight functions\n");
      ok = 0;
    }
  }

  free(fd);
  free(fd_alias);
  free(library);
  free(clib);
  free(proto);
  free(inline_header);
  free(pragmas);
  return ok ? 0 : 1;
}
