/*
 * Source guard for the DataTypes object creation probe.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
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

int main(int argc, char **argv)
{
  char *source;
  int ok;

  if (argc != 2) {
    printf("usage: %s <tools/zz9k-dtprobe.c>\n", argv[0]);
    return 2;
  }

  source = read_file(argv[1]);
  if (!source) {
    printf("failed to read %s\n", argv[1]);
    return 2;
  }

  ok = 1;
  ok &= expect_contains(
      source, "Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio");
  ok &= expect_contains(source, "SPDX-License-Identifier: GPL-3.0-or-later");
  ok &= expect_contains(source, "#include <clib/alib_protos.h>");
  ok &= expect_contains(source, "#include <datatypes/pictureclass.h>");
  ok &= expect_contains(source, "#include <exec/memory.h>");
  ok &= expect_contains(source, "#include <graphics/gfx.h>");
  ok &= expect_contains(source, "#include <graphics/gfxbase.h>");
  ok &= expect_contains(source, "#include <intuition/intuition.h>");
  ok &= expect_contains(source, "#include <proto/graphics.h>");
  ok &= expect_contains(source, "#include <proto/intuition.h>");
  ok &= expect_contains(source, "#include <intuition/gadgetclass.h>");
  ok &= expect_contains(source, "#include <intuition/screens.h>");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"datatypes.library\", 39)");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"intuition.library\", 39)");
  ok &= expect_contains(source, "OpenLibrary((CONST_STRPTR)\"graphics.library\", 39)");
  ok &= expect_contains(source, "ObtainDataType");
  ok &= expect_contains(source, "ReleaseDataType");
  ok &= expect_contains(source, "Lock(path, ACCESS_READ)");
  ok &= expect_contains(source, "UnLock(lock)");
  ok &= expect_contains(source, "dtn_Header");
  ok &= expect_contains(source, "dth_Name");
  ok &= expect_contains(source, "dth_BaseName");
  ok &= expect_contains(source, "\"datatypes/%s.datatype\"");
  ok &= expect_contains(source, "zz9k_dtprobe_try_named_open");
  ok &= expect_contains(source, "\"dos.library\"");
  ok &= expect_contains(source, "\"intuition.library\"");
  ok &= expect_contains(source, "\"picture.datatype\"");
  ok &= expect_contains(source, "\"SYS:Classes/DataTypes/%s.datatype\"");
  ok &= expect_contains(source, "\"CLASSES:DataTypes/%s.datatype\"");
  ok &= expect_contains(source, "NewDTObject");
  ok &= expect_contains(source, "DTA_SourceType");
  ok &= expect_contains(source, "DTST_FILE");
  ok &= expect_contains(source, "DTA_GroupID");
  ok &= expect_contains(source, "GID_PICTURE");
  ok &= expect_contains(source, "IoErr()");
  ok &= expect_contains(source, "DTA_NominalHoriz");
  ok &= expect_contains(source, "DTA_NominalVert");
  ok &= expect_contains(source, "DTA_TotalHoriz");
  ok &= expect_contains(source, "DTA_TotalVert");
  ok &= expect_contains(source, "DTA_ObjName");
  ok &= expect_contains(source, "DTA_Methods");
  ok &= expect_contains(source, "PDTA_BitMapHeader");
  ok &= expect_contains(source, "struct BitMapHeader");
  ok &= expect_contains(source, "bmh_Width");
  ok &= expect_contains(source, "bmh_Height");
  ok &= expect_contains(source, "bmh_Depth");
  ok &= expect_contains(source, "bmh_Masking");
  ok &= expect_contains(source, "bmh_Transparent");
  ok &= expect_contains(source, "bmh_Compression");
  ok &= expect_contains(source, "bmh_PageWidth");
  ok &= expect_contains(source, "bmh_PageHeight");
  ok &= expect_contains(source, "PDTA_ModeID");
  ok &= expect_contains(source, "PDTA_SourceMode");
  ok &= expect_contains(source, "PDTA_DestMode");
  ok &= expect_contains(source, "PDTA_NumColors");
  ok &= expect_contains(source, "PDTA_NumAlloc");
  ok &= expect_contains(source, "PDTA_AlphaChannel");
  ok &= expect_contains(source, "PDTA_ColorRegisters");
  ok &= expect_contains(source, "PDTA_CRegs");
  ok &= expect_contains(source, "PDTA_GRegs");
  ok &= expect_contains(source, "PDTA_ColorTable");
  ok &= expect_contains(source, "PDTA_ColorTable2");
  ok &= expect_contains(source, "PDTM_WRITEPIXELARRAY");
  ok &= expect_contains(source, "PDTM_READPIXELARRAY");
  ok &= expect_contains(source, "PDTM_OBTAINPIXELARRAY");
  ok &= expect_contains(source, "PDTM_SCALE");
  ok &= expect_contains(source, "--client");
  ok &= expect_contains(source, "--read-pixels");
  ok &= expect_contains(source, "--screen-remap");
  ok &= expect_contains(source, "--layout");
  ok &= expect_contains(source, "--color-tables");
  ok &= expect_contains(source, "--draw-window");
  ok &= expect_contains(source, "PDTA_Screen");
  ok &= expect_contains(source, "PDTA_Remap, TRUE");
  ok &= expect_contains(source, "LockPubScreen(0)");
  ok &= expect_contains(source, "UnlockPubScreen(0, screen)");
  ok &= expect_contains(source, "struct gpLayout");
  ok &= expect_contains(source, "DTM_PROCLAYOUT");
  ok &= expect_contains(source, "gpl_GInfo");
  ok &= expect_contains(source, "DoMethodA(object, (Msg)&layout)");
  ok &= expect_contains(source, "zz9k_dtprobe_run_layout");
  ok &= expect_contains(source, "zz9k_dtprobe_print_color_attrs");
  ok &= expect_contains(source, "SetDTAttrs(");
  ok &= expect_contains(source, "AddDTObject(window, 0, object, -1L)");
  ok &= expect_contains(source, "RefreshDTObjectA(object, window, 0, 0)");
  ok &= expect_contains(source, "RemoveDTObject(window, object)");
  ok &= expect_contains(source, "OpenWindowTags");
  ok &= expect_contains(source, "WA_Borderless");
  ok &= expect_contains(source, "WA_IDCMP, IDCMP_REFRESHWINDOW");
  ok &= expect_contains(source, "SetRast(window->RPort, background_pen)");
  ok &= expect_contains(source, "ReadPixel(rast_port");
  ok &= expect_contains(source, "CloseWindow(window)");
  ok &= expect_contains(source, "zz9k_dtprobe_attach_draw_object");
  ok &= expect_contains(source, "zz9k_dtprobe_refresh_once");
  ok &= expect_contains(source, "zz9k_dtprobe_print_draw_window");
  ok &= expect_contains(source, "ZZ9K_DTPROBE_DRAW_MAX_WIDTH");
  ok &= expect_contains(source, "PDTM_READPIXELARRAY");
  ok &= expect_contains(source, "struct pdtBlitPixelArray");
  ok &= expect_contains(source, "pbpa_PixelData");
  ok &= expect_contains(source, "pbpa_PixelFormat");
  ok &= expect_contains(source, "pbpa_PixelArrayMod");
  ok &= expect_contains(source, "pbpa_Left");
  ok &= expect_contains(source, "pbpa_Top");
  ok &= expect_contains(source, "PBPAFMT_RGB");
  ok &= expect_contains(source, "PBPAFMT_RGBA");
  ok &= expect_contains(source, "PBPAFMT_ARGB");
  ok &= expect_contains(source, "PBPAFMT_LUT8");
  ok &= expect_contains(source, "PBPAFMT_GREY8");
  ok &= expect_contains(source, "AllocMem(row_bytes, MEMF_PUBLIC)");
  ok &= expect_contains(source, "FreeMem(row, row_bytes)");
  ok &= expect_contains(source, "DoMethodA(object, (Msg)&pixels)");
  ok &= expect_contains(source, "zz9k_dtprobe_fnv1a_update");
  ok &= expect_contains(source, "zz9k_dtprobe_print_pixel_reads");
  ok &= expect_contains(source, "zz9k_dtprobe_print_pixel_format_read");
  ok &= expect_contains(source, "alpha_min");
  ok &= expect_contains(source, "alpha_max");
  ok &= expect_contains(source, "alpha_nonopaque");
  ok &= expect_contains(source, "PDTA_BitMap");
  ok &= expect_contains(source, "PDTA_ClassBitMap");
  ok &= expect_contains(source, "PDTA_DestBitMap");
  ok &= expect_contains(source, "PDTA_MaskPlane");
  ok &= expect_contains(source, "zz9k_dtprobe_print_picture_attrs");
  ok &= expect_contains(source, "zz9k_dtprobe_print_methods");
  ok &= expect_contains(source, "zz9k_dtprobe_print_client_attrs");
  ok &= expect_contains(source, "DisposeDTObject");
  ok &= expect_contains(source, "CloseLibrary(DataTypesBase)");

  free(source);
  return ok ? 0 : 1;
}
