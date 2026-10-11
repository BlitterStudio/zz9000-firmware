/*
 * ZZ9000 picture DataType class.
 *
 * Copyright (C) 2024-2026, Dimitris Panokostas / BlitterStudio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k-image-window.h"
#include "zz9k/caps.h"
#include "zz9k/image.h"
#include "zz9k/shared.h"
#include "zz9k/surface.h"
#include <SDI_compiler.h>
#include <clib/alib_protos.h>
#include <datatypes/datatypesclass.h>
#include <datatypes/pictureclass.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <proto/datatypes.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <stdint.h>
#include <string.h>

#define ZZ9K_PICTURE_DATATYPE_NAME "zz9k-picture.datatype"
#define ZZ9K_PICTURE_DATATYPE_VERSION 42
#define ZZ9K_PICTURE_DATATYPE_REVISION 152
#define ZZ9K_PICTURE_DATATYPE_ID_STRING \
  "$VER: zz9k-picture.datatype 42.152 (8.10.2026) ZZ9000 SDK"
#define ZZ9K_PICTURE_BUILD_MARKER \
  "metadata: build 2026-10-08 memory-direct-staging-v152"
#define ZZ9K_PICTURE_OBJECT_NAME_BYTES 128U
#define ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE 64U
#define ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL 3U
#define ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL 4U
#define ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL 4U
#define ZZ9K_PICTURE_TILE_MAX_ROWS 32U
#define ZZ9K_PICTURE_TILE_TARGET_BYTES (128UL * 1024UL)
#define ZZ9K_PICTURE_RGB888_TILE_MAX_ROWS 256U
#define ZZ9K_PICTURE_RGB888_TILE_TARGET_BYTES (768UL * 1024UL)
#define ZZ9K_PICTURE_DIRECT_DATATYPE_SURFACE_MAX_BYTES \
  (512UL * 1024UL)
#define ZZ9K_PICTURE_STAGING_BYTES (256UL * 1024UL)
#define ZZ9K_PICTURE_READ_CHUNK_BYTES (256UL * 1024UL)
#define ZZ9K_PICTURE_Z2_STAGING_BYTES (24UL * 1024UL)
#define ZZ9K_PICTURE_Z2_TILE_TARGET_BYTES (32UL * 1024UL)
#define ZZ9K_PICTURE_MAX_SURFACE_BYTES (16UL * 1024UL * 1024UL)
#define ZZ9K_PICTURE_RENDER_HARDWARE 1
#define ZZ9K_PICTURE_FORCE_DATATYPE_V47_TRUECOLOR 0
#define ZZ9K_PICTURE_FORCE_DATATYPE_V47_DIRECT 0
#define ZZ9K_PICTURE_FORCE_DATATYPE_V43_WRITEPIXELS 0
#define ZZ9K_PICTURE_DYNAMIC_DATATYPE_FEATURES 1
#define ZZ9K_PICTURE_ENABLE_DATATYPE_V47_DIRECT 0
#define ZZ9K_PICTURE_ENABLE_JPEG_DATATYPE_V47_RGB_DIRECT 0
#define ZZ9K_PICTURE_FORCE_REFERENCE_V43_WRITEPIXELS 1
#define ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS 0
#define ZZ9K_PICTURE_TRACE_ENABLED 0
/* Gates the GM_RENDER traces separately from the master switch above; both
 * must be on. See zz9k_picture_trace_render() for why render is special. */
#define ZZ9K_PICTURE_RENDER_TRACE_ENABLED 0
#define ZZ9K_PICTURE_TRACE_RESET_ENABLED 0
#define ZZ9K_PICTURE_SOURCE_TRACE_ENABLED 0
#define ZZ9K_PICTURE_TRACE_STREAM_CHUNKS 0
#define ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE 0
#define ZZ9K_PICTURE_DATATYPE_TRACE_CHUNKS 0U
#define ZZ9K_PICTURE_USE_LAYER_CLIPS 0
#define ZZ9K_PICTURE_RENDER_MODE_ENV_PATH "ENV:ZZ9K_PICTURE_RENDER_MODE"
#define ZZ9K_PICTURE_TRACE_PATH "T:zz9k-picture.datatype.log"
#define ZZ9K_PICTURE_TRACE_PERSIST_PATH "SYS:zz9k-picture.datatype.log"
#define ZZ9K_PICTURE_SOURCE_FILE 1U
#define ZZ9K_PICTURE_SOURCE_MEMORY 2U
#define ZZ9K_PICTURE_PNG_PALETTE_MAX_ENTRIES 256U
/* Wider than uint8_t so palette index 255 never collides with the
 * "no transparent entry" sentinel. */
#define ZZ9K_PICTURE_PNG_TRANSPARENT_NONE 0x0100U
#define ZZ9K_PICTURE_PNG_TRANSPARENT_MASK_BYTES 32U
/* Open-addressed reverse map from decoded RGB triples back to PNG palette
 * indices. 512 slots keep the load factor at or below 0.5 for a full
 * 256-entry palette so lookups stay near one probe. */
#define ZZ9K_PICTURE_LUT8_MAP_SLOTS 512U
#define ZZ9K_PICTURE_LUT8_MAP_HASH_MULTIPLIER 2654435761UL

#define ZZ9K_PICTURE_RENDER_TRACE_SOURCE_NOT_READY (1UL << 0)
#define ZZ9K_PICTURE_RENDER_TRACE_SCREEN_REJECTED (1UL << 1)
#define ZZ9K_PICTURE_RENDER_TRACE_BEGIN (1UL << 2)
#define ZZ9K_PICTURE_RENDER_TRACE_AREA_FAILED (1UL << 3)
#define ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_FAILED (1UL << 4)
#define ZZ9K_PICTURE_RENDER_TRACE_VISIBLE_FAILED (1UL << 5)
#define ZZ9K_PICTURE_RENDER_TRACE_DAMAGE_FAILED (1UL << 6)
#define ZZ9K_PICTURE_RENDER_TRACE_SCREEN_INFO_FAILED (1UL << 11)
#define ZZ9K_PICTURE_RENDER_TRACE_SCREEN_BOUNDS_FAILED (1UL << 12)
#define ZZ9K_PICTURE_RENDER_TRACE_AREA_READY (1UL << 13)
#define ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_READY (1UL << 14)
#define ZZ9K_PICTURE_RENDER_TRACE_VISIBLE_READY (1UL << 15)
#define ZZ9K_PICTURE_RENDER_TRACE_DAMAGE_READY (1UL << 16)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_OFF (1UL << 21)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_DECODE (1UL << 22)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_PROBE (1UL << 23)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_SUBCLASS (1UL << 25)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_SUPER (1UL << 26)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_SCREEN_COMPLETE (1UL << 27)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_AREA_COMPLETE (1UL << 28)
#define ZZ9K_PICTURE_RENDER_TRACE_MODE_DRAW_COMPLETE (1UL << 29)

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *DataTypesBase;
struct Library *PictureBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *UtilityBase;
static ZZ9KCaps zz9k_picture_cached_caps;
static ZZ9KServiceInfo zz9k_picture_cached_image_service;
static ZZ9KContext *zz9k_picture_cached_ctx;
static struct SignalSemaphore zz9k_picture_decode_semaphore;
static uint8_t zz9k_picture_cached_caps_ready;
static uint8_t zz9k_picture_cached_image_service_ready;
static uint8_t zz9k_picture_decode_semaphore_ready;
static uint8_t zz9k_picture_render_mode_ready;

typedef enum ZZ9KPictureCodec {
  ZZ9K_PICTURE_CODEC_UNKNOWN = 0,
  ZZ9K_PICTURE_CODEC_JPEG,
  ZZ9K_PICTURE_CODEC_PNG,
  ZZ9K_PICTURE_CODEC_WEBP
} ZZ9KPictureCodec;

typedef enum ZZ9KPictureRenderMode {
  ZZ9K_PICTURE_RENDER_MODE_DATATYPE = 0,
  ZZ9K_PICTURE_RENDER_MODE_OFF,
  ZZ9K_PICTURE_RENDER_MODE_SMALLOFF,
  ZZ9K_PICTURE_RENDER_MODE_V43SMALL,
  ZZ9K_PICTURE_RENDER_MODE_REFERENCE,
  ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT,
  ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE,
  ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT,
  ZZ9K_PICTURE_RENDER_MODE_DECODE,
  ZZ9K_PICTURE_RENDER_MODE_SUBCLASS,
  ZZ9K_PICTURE_RENDER_MODE_SUPER,
  ZZ9K_PICTURE_RENDER_MODE_SCREEN,
  ZZ9K_PICTURE_RENDER_MODE_AREA,
  ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY,
  ZZ9K_PICTURE_RENDER_MODE_DRAWFIT,
  ZZ9K_PICTURE_RENDER_MODE_DRAWCENTER,
  ZZ9K_PICTURE_RENDER_MODE_DRAWTRACE,
  ZZ9K_PICTURE_RENDER_MODE_DRAW,
  ZZ9K_PICTURE_RENDER_MODE_PROBE
} ZZ9KPictureRenderMode;

/* Guarded by zz9k_picture_render_mode_ready; see zz9k_picture_render_mode(). */
static ZZ9KPictureRenderMode zz9k_picture_cached_render_mode;

/* PLTE/tRNS metadata captured from indexed (color type 3) PNG files. It
 * drives the PNGdt-compatible LUT8 output path: decoded truecolor tiles are
 * mapped back to the file's own palette indices and published as a depth-8
 * picture with a transparent colour, which is the contract MUI's picture
 * pipeline handles quickly. */
typedef struct ZZ9KPicturePngPalette {
  uint8_t rgb[ZZ9K_PICTURE_PNG_PALETTE_MAX_ENTRIES * 3U];
  uint8_t transparent_mask[ZZ9K_PICTURE_PNG_TRANSPARENT_MASK_BYTES];
  uint16_t count;
  uint16_t transparent_index;
  uint8_t partial_alpha;
  uint8_t present;
} ZZ9KPicturePngPalette;

typedef struct ZZ9KPictureInstance {
  ZZ9KPictureCodec codec;
  char object_name[ZZ9K_PICTURE_OBJECT_NAME_BYTES];
  uint32_t width;
  uint32_t height;
  uint32_t framebuffer_width;
  uint32_t framebuffer_height;
  uint32_t source_handle;
  uint8_t *reference_pixels;
  uint32_t render_trace_mask;
  uint32_t reference_pixel_bytes;
  ZZ9KContext *ctx;
  uint8_t source_ready;
  uint8_t decode_attempted;
  uint8_t render_attrs_ready;
  uint8_t datatype_sync_sent;
  uint8_t png_alpha_known;
  uint8_t png_has_alpha;
  uint8_t flatten_png_alpha;
  uint8_t png_interlaced;
  ZZ9KPicturePngPalette png_palette;
} ZZ9KPictureInstance;

typedef struct ZZ9KPictureDatatypeTarget {
  Object *object;
  Class *cl;
  struct pdtBlitPixelArray *direct_pixels;
  struct BitMap *legacy_bitmap;
  struct BitMap *legacy_line_bitmap;
  ZZ9KPictureInstance *instance;
  uint8_t *scratch_pixels;
  uint32_t width;
  uint32_t height;
  uint32_t scratch_pitch;
  uint32_t output_format;
  uint32_t output_bpp;
  uint32_t tile_rows;
  uint32_t tiles_written;
  const ZZ9KPicturePngPalette *lut8_palette;
  uint32_t *lut8_map_keys;
  uint8_t *lut8_map_vals;
  uint8_t direct;
  uint8_t legacy_has_alpha;
  uint8_t png_lut8;
} ZZ9KPictureDatatypeTarget;

typedef struct ZZ9KPictureStreamInput {
  ZZ9KSharedBuffer staging;
  uint8_t *read_scratch;
  uint32_t read_scratch_bytes;
} ZZ9KPictureStreamInput;

typedef struct ZZ9KPictureSource {
  BPTR file;
  const uint8_t *memory;
  uint32_t size;
  uint32_t position;
  uint8_t type;
} ZZ9KPictureSource;

typedef struct ZZ9KPictureDatatypeBase {
  struct ClassLibrary class_library;
  BPTR segment;
  UBYTE class_added;
} ZZ9KPictureDatatypeBase;

static ZZ9KPictureDatatypeBase *zz9k_picture_datatype_open(
    REG(a6, ZZ9KPictureDatatypeBase *base));
static BPTR zz9k_picture_datatype_close(
    REG(a6, ZZ9KPictureDatatypeBase *base));
static BPTR zz9k_picture_datatype_expunge(
    REG(a6, ZZ9KPictureDatatypeBase *base));
static ULONG zz9k_picture_datatype_null(void);
static Class *zz9k_picture_datatype_get_class(
    REG(a6, ZZ9KPictureDatatypeBase *base));
static ZZ9KPictureDatatypeBase *zz9k_picture_datatype_init(
    REG(a0, BPTR segment));
static ULONG zz9k_picture_datatype_dispatch(REG(a0, struct Hook *hook),
                                            REG(a2, Object *object),
                                            REG(a1, Msg msg));

static const APTR zz9k_picture_datatype_vectors[] = {
  (APTR)zz9k_picture_datatype_open,
  (APTR)zz9k_picture_datatype_close,
  (APTR)zz9k_picture_datatype_expunge,
  (APTR)zz9k_picture_datatype_null,
  (APTR)zz9k_picture_datatype_get_class,
  (APTR)-1
};

static const struct Resident zz9k_picture_datatype_romtag
    __attribute__((used)) = {
  RTC_MATCHWORD,
  (struct Resident *)&zz9k_picture_datatype_romtag,
  (APTR)(&zz9k_picture_datatype_romtag + 1),
  0,
  ZZ9K_PICTURE_DATATYPE_VERSION,
  NT_LIBRARY,
  0,
  (char *)ZZ9K_PICTURE_DATATYPE_NAME,
  (char *)ZZ9K_PICTURE_DATATYPE_ID_STRING,
  (APTR)zz9k_picture_datatype_init
};

static uint16_t zz9k_picture_read_be16(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static uint32_t zz9k_picture_read_be32(const uint8_t *bytes)
{
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void zz9k_picture_trace_path(const char *path, const char *message)
{
  BPTR file;

  if (!DOSBase || !path || !message) {
    return;
  }

  file = Open((CONST_STRPTR)path, MODE_OLDFILE);
  if (file) {
    (void)Seek(file, 0, OFFSET_END);
  } else {
    file = Open((CONST_STRPTR)path, MODE_NEWFILE);
  }
  if (!file) {
    return;
  }

  (void)Write(file, (CONST_APTR)message, (LONG)strlen(message));
  (void)Write(file, (CONST_APTR)"\n", 1);
  Close(file);
}

static void zz9k_picture_trace_reset_path(const char *path)
{
  BPTR file;

  if (!DOSBase || !path) {
    return;
  }

  file = Open((CONST_STRPTR)path, MODE_NEWFILE);
  if (!file) {
    return;
  }

  (void)Write(file, (CONST_APTR)ZZ9K_PICTURE_BUILD_MARKER,
              (LONG)strlen(ZZ9K_PICTURE_BUILD_MARKER));
  (void)Write(file, (CONST_APTR)"\n", 1);
  Close(file);
}

static void zz9k_picture_trace_source(const char *message)
{
  if (ZZ9K_PICTURE_SOURCE_TRACE_ENABLED) {
    zz9k_picture_trace_path(ZZ9K_PICTURE_TRACE_PATH, message);
    zz9k_picture_trace_path(ZZ9K_PICTURE_TRACE_PERSIST_PATH, message);
  } else {
    (void)message;
  }
}

static void zz9k_picture_trace_source_hex(const char *label, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  char buffer[96];
  uint32_t i;
  uint32_t pos;

  if (!ZZ9K_PICTURE_SOURCE_TRACE_ENABLED) {
    (void)label;
    (void)value;
    return;
  }
  if (!label) {
    return;
  }

  pos = 0U;
  while (label[pos] != '\0' && pos < (uint32_t)(sizeof(buffer) - 12U)) {
    buffer[pos] = label[pos];
    pos++;
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = ' ';
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = '0';
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = 'x';
  }
  for (i = 0U; i < 8U && pos < (uint32_t)(sizeof(buffer) - 1U); i++) {
    buffer[pos++] = hex[(value >> (28U - (i * 4U))) & 0x0fU];
  }
  buffer[pos] = '\0';
  zz9k_picture_trace_source(buffer);
}

static void zz9k_picture_trace_reset(void)
{
  if (ZZ9K_PICTURE_TRACE_RESET_ENABLED) {
    zz9k_picture_trace_reset_path(ZZ9K_PICTURE_TRACE_PATH);
    zz9k_picture_trace_reset_path(ZZ9K_PICTURE_TRACE_PERSIST_PATH);
  }
  if (ZZ9K_PICTURE_SOURCE_TRACE_ENABLED) {
    zz9k_picture_trace_source(ZZ9K_PICTURE_BUILD_MARKER);
  }
}

static void zz9k_picture_trace(const char *message)
{
  if (ZZ9K_PICTURE_TRACE_ENABLED) {
    zz9k_picture_trace_path(ZZ9K_PICTURE_TRACE_PATH, message);
    zz9k_picture_trace_path(ZZ9K_PICTURE_TRACE_PERSIST_PATH, message);
  } else {
    (void)message;
  }
}

/* Trace sink for the GM_RENDER path only.
 *
 * The sinks are dos.library Open/Write/Close, and GM_RENDER runs between
 * ObtainGIRPort() and ReleaseGIRPort(), i.e. with the window's layer locked.
 * Tracing there sleeps on a DOS packet reply while holding that lock, which
 * deadlocks Intuition during a window resize - the very failure this class is
 * usually being traced to diagnose. So turning on ZZ9K_PICTURE_TRACE_ENABLED
 * alone stays safe during a resize; opt into render tracing deliberately with
 * ZZ9K_PICTURE_RENDER_TRACE_ENABLED, and expect resize to hang while it is on.
 *
 * Layout and metadata tracing are unaffected: those run in contexts where
 * blocking is legitimate. */
static void zz9k_picture_trace_render(const char *message)
{
  if (ZZ9K_PICTURE_RENDER_TRACE_ENABLED) {
    zz9k_picture_trace(message);
  } else {
    (void)message;
  }
}

static void zz9k_picture_trace_hex(const char *label, uint32_t value)
{
  static const char hex[] = "0123456789ABCDEF";
  char buffer[96];
  uint32_t i;
  uint32_t pos;

  if (!ZZ9K_PICTURE_TRACE_ENABLED) {
    (void)label;
    (void)value;
    return;
  }

  if (!label) {
    return;
  }

  pos = 0U;
  while (label[pos] != '\0' && pos < (uint32_t)(sizeof(buffer) - 12U)) {
    buffer[pos] = label[pos];
    pos++;
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = ' ';
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = '0';
  }
  if (pos < (uint32_t)(sizeof(buffer) - 1U)) {
    buffer[pos++] = 'x';
  }
  for (i = 0U; i < 8U && pos < (uint32_t)(sizeof(buffer) - 1U); i++) {
    buffer[pos++] = hex[(value >> (28U - (i * 4U))) & 0x0fU];
  }
  buffer[pos] = '\0';
  zz9k_picture_trace(buffer);
}

static void zz9k_picture_trace_u32(const char *label, uint32_t value)
{
  zz9k_picture_trace_hex(label, value);
}

static void zz9k_picture_trace_render_once(ZZ9KPictureInstance *instance,
                                           uint32_t flag,
                                           const char *message)
{
  if (!instance || !message || (instance->render_trace_mask & flag) != 0U) {
    return;
  }

  instance->render_trace_mask |= flag;
  zz9k_picture_trace_render(message);
}

static char zz9k_picture_ascii_lower(char value)
{
  if (value >= 'A' && value <= 'Z') {
    return (char)(value + ('a' - 'A'));
  }
  return value;
}

static int zz9k_picture_ascii_space(char value)
{
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

static int zz9k_picture_render_mode_matches(const char *value,
                                            LONG length,
                                            const char *token)
{
  LONG start;
  LONG end;
  LONG token_length;
  LONG i;

  if (!value || !token || length <= 0) {
    return 0;
  }

  start = 0;
  end = length;
  while (start < end && zz9k_picture_ascii_space(value[start])) {
    start++;
  }
  while (end > start && zz9k_picture_ascii_space(value[end - 1])) {
    end--;
  }

  token_length = (LONG)strlen(token);
  if ((end - start) != token_length) {
    return 0;
  }
  for (i = 0; i < token_length; i++) {
    if (zz9k_picture_ascii_lower(value[start + i]) != token[i]) {
      return 0;
    }
  }
  return 1;
}

static int zz9k_picture_legacy_framebuffer_mode(const char *value,
                                                LONG length)
{
  return zz9k_picture_render_mode_matches(value, length, "fill") ||
         zz9k_picture_render_mode_matches(value, length, "fill1super") ||
         zz9k_picture_render_mode_matches(
             value, length, "surfacefill1super") ||
         zz9k_picture_render_mode_matches(value, length, "scale") ||
         zz9k_picture_render_mode_matches(value, length, "scale1") ||
         zz9k_picture_render_mode_matches(value, length, "scale1super") ||
         zz9k_picture_render_mode_matches(value, length, "scale2") ||
         zz9k_picture_render_mode_matches(value, length, "scale4") ||
         zz9k_picture_render_mode_matches(value, length, "scale8");
}

static ZZ9KPictureRenderMode zz9k_picture_render_mode_from_env(
    const char *value,
    LONG length)
{
  if (zz9k_picture_render_mode_matches(value, length, "datatype") ||
      zz9k_picture_render_mode_matches(value, length, "auto")) {
    return ZZ9K_PICTURE_RENDER_MODE_DATATYPE;
  }
  /* These undocumented framebuffer diagnostics issued synchronous SDK
   * mailbox calls from GM_RENDER while Intuition held the window layer.
   * Keep old ENV settings safe and useful by selecting the validated
   * DataType pixel path instead. */
  if (zz9k_picture_legacy_framebuffer_mode(value, length)) {
    return ZZ9K_PICTURE_RENDER_MODE_DATATYPE;
  }
  if (zz9k_picture_render_mode_matches(value, length, "off")) {
    return ZZ9K_PICTURE_RENDER_MODE_OFF;
  }
  if (zz9k_picture_render_mode_matches(value, length, "smalloff")) {
    return ZZ9K_PICTURE_RENDER_MODE_SMALLOFF;
  }
  if (zz9k_picture_render_mode_matches(value, length, "v43small")) {
    return ZZ9K_PICTURE_RENDER_MODE_V43SMALL;
  }
  if (zz9k_picture_render_mode_matches(value, length, "reference")) {
    return ZZ9K_PICTURE_RENDER_MODE_REFERENCE;
  }
  if (zz9k_picture_render_mode_matches(value, length, "referencenolayout")) {
    return ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT;
  }
  if (zz9k_picture_render_mode_matches(value, length, "alphareference")) {
    return ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE;
  }
  if (zz9k_picture_render_mode_matches(
          value, length, "alphareferencenolayout")) {
    return ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT;
  }
  if (zz9k_picture_render_mode_matches(value, length, "decode")) {
    return ZZ9K_PICTURE_RENDER_MODE_DECODE;
  }
  if (zz9k_picture_render_mode_matches(value, length, "subclass")) {
    return ZZ9K_PICTURE_RENDER_MODE_SUBCLASS;
  }
  if (zz9k_picture_render_mode_matches(value, length, "super")) {
    return ZZ9K_PICTURE_RENDER_MODE_SUPER;
  }
  if (zz9k_picture_render_mode_matches(value, length, "screen")) {
    return ZZ9K_PICTURE_RENDER_MODE_SCREEN;
  }
  if (zz9k_picture_render_mode_matches(value, length, "area")) {
    return ZZ9K_PICTURE_RENDER_MODE_AREA;
  }
  if (zz9k_picture_render_mode_matches(value, length, "drawcopy")) {
    return ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY;
  }
  if (zz9k_picture_render_mode_matches(value, length, "drawfit")) {
    return ZZ9K_PICTURE_RENDER_MODE_DRAWFIT;
  }
  if (zz9k_picture_render_mode_matches(value, length, "drawcenter")) {
    return ZZ9K_PICTURE_RENDER_MODE_DRAWCENTER;
  }
  if (zz9k_picture_render_mode_matches(value, length, "drawtrace")) {
    return ZZ9K_PICTURE_RENDER_MODE_DRAWTRACE;
  }
  if (zz9k_picture_render_mode_matches(value, length, "draw")) {
    return ZZ9K_PICTURE_RENDER_MODE_DRAW;
  }
  if (zz9k_picture_render_mode_matches(value, length, "probe")) {
    return ZZ9K_PICTURE_RENDER_MODE_PROBE;
  }
  return ZZ9K_PICTURE_RENDER_MODE_OFF;
}

static int zz9k_picture_read_render_mode_env(
    ZZ9KPictureRenderMode *render_mode)
{
  char value[32];
  BPTR file;
  LONG length;

  if (!render_mode || !DOSBase) {
    return 0;
  }

  file = Open((CONST_STRPTR)ZZ9K_PICTURE_RENDER_MODE_ENV_PATH, MODE_OLDFILE);
  if (!file) {
    return 0;
  }

  length = Read(file, value, (LONG)(sizeof(value) - 1U));
  Close(file);
  if (length <= 0) {
    return 0;
  }
  value[length] = '\0';
  *render_mode = zz9k_picture_render_mode_from_env(value, length);
  return 1;
}

static int zz9k_picture_forced_render_mode_allows_env(
    ZZ9KPictureRenderMode render_mode)
{
  return render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT;
}

/* The ENV override is a diagnostic knob, so it is read once and cached.
 * GM_RENDER runs between ObtainGIRPort()/ReleaseGIRPort(), i.e. with the
 * window's layer locked, and Open() on ENV: sleeps waiting for a DOS packet
 * reply. Re-reading it per render slept while holding that lock, which
 * deadlocks Intuition during a window resize. The first call comes from
 * OM_NEW, which is a safe context for DOS I/O. */
static ZZ9KPictureRenderMode zz9k_picture_render_mode(void)
{
  ZZ9KPictureRenderMode render_mode;

  if (zz9k_picture_render_mode_ready) {
    return zz9k_picture_cached_render_mode;
  }

  render_mode = ZZ9K_PICTURE_RENDER_MODE_DATATYPE;
  (void)zz9k_picture_read_render_mode_env(&render_mode);
  zz9k_picture_cached_render_mode = render_mode;
  zz9k_picture_render_mode_ready = 1U;
  return render_mode;
}

static void zz9k_picture_source_reset(ZZ9KPictureSource *source)
{
  if (!source) {
    return;
  }
  memset(source, 0, sizeof(*source));
}

static int zz9k_picture_source_init_file(ZZ9KPictureSource *source,
                                         BPTR file)
{
  if (!source || !file) {
    return 0;
  }

  zz9k_picture_source_reset(source);
  source->file = file;
  source->type = ZZ9K_PICTURE_SOURCE_FILE;
  zz9k_picture_trace("metadata: source file");
  zz9k_picture_trace_source("metadata: source file");
  return 1;
}

static int zz9k_picture_source_init_memory(ZZ9KPictureSource *source,
                                           APTR address,
                                           ULONG size)
{
  if (!source || !address || size == 0UL || size > 0x7ffffffeUL) {
    return 0;
  }

  zz9k_picture_source_reset(source);
  source->memory = (const uint8_t *)address;
  source->size = (uint32_t)size;
  source->position = 0U;
  source->type = ZZ9K_PICTURE_SOURCE_MEMORY;
  zz9k_picture_trace("metadata: source memory");
  zz9k_picture_trace_source("metadata: source memory");
  zz9k_picture_trace_source_hex("metadata: source size", (uint32_t)size);
  return 1;
}

static int zz9k_picture_get_source(Object *object,
                                   ZZ9KPictureSource *source)
{
  ULONG source_type;

  if (!object || !source) {
    return 0;
  }

  zz9k_picture_source_reset(source);
  source_type = DTST_FILE;
  (void)GetDTAttrs(object, DTA_SourceType, (ULONG)&source_type, TAG_END);
  zz9k_picture_trace_source_hex("metadata: source type", source_type);
  if (source_type == DTST_MEMORY) {
    APTR source_address;
    ULONG source_size;

    source_address = 0;
    source_size = 0UL;
    if (GetDTAttrs(object,
                   DTA_SourceAddress, (ULONG)&source_address,
                   DTA_SourceSize, (ULONG)&source_size,
                   TAG_END) >= 2 &&
        zz9k_picture_source_init_memory(
            source, source_address, source_size)) {
      return 1;
    }
    zz9k_picture_trace("decode: source unavailable");
    zz9k_picture_trace_source("decode: source unavailable");
    return 0;
  }

  if (source_type == DTST_FILE) {
    BPTR file;

    file = 0;
    if (GetDTAttrs(object, DTA_Handle, (ULONG)&file, TAG_END) >= 1 &&
        zz9k_picture_source_init_file(source, file)) {
      return 1;
    }
  }

  zz9k_picture_trace("decode: source unavailable");
  zz9k_picture_trace_source("decode: source unavailable");
  return 0;
}

static LONG zz9k_picture_source_seek(ZZ9KPictureSource *source,
                                     LONG offset,
                                     LONG mode)
{
  if (!source) {
    return -1;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_FILE) {
    return source->file ? Seek(source->file, offset, mode) : -1;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_MEMORY) {
    int64_t base;
    int64_t target;

    if (!source->memory) {
      return -1;
    }
    if (mode == OFFSET_BEGINNING) {
      base = 0;
    } else if (mode == OFFSET_CURRENT) {
      base = (int64_t)source->position;
    } else if (mode == OFFSET_END) {
      base = (int64_t)source->size;
    } else {
      return -1;
    }
    target = base + (int64_t)offset;
    if (target < 0 || target > (int64_t)source->size ||
        target > (int64_t)0x7ffffffeUL) {
      return -1;
    }
    source->position = (uint32_t)target;
    return (LONG)target;
  }
  return -1;
}

static LONG zz9k_picture_source_read(ZZ9KPictureSource *source,
                                     uint8_t *dst,
                                     uint32_t length)
{
  if (!source || (!dst && length != 0U) || length > 0x7ffffffeUL) {
    return -1;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_FILE) {
    return source->file ? Read(source->file, dst, (LONG)length) : -1;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_MEMORY) {
    uint32_t available;
    uint32_t copy;

    if (!source->memory || source->position > source->size) {
      return -1;
    }
    available = source->size - source->position;
    copy = length;
    if (copy > available) {
      copy = available;
    }
    if (copy != 0U) {
      uint32_t i;

      for (i = 0U; i < copy; i++) {
        dst[i] = source->memory[source->position + i];
      }
      source->position += copy;
    }
    return (LONG)copy;
  }
  return -1;
}

static int zz9k_picture_source_size(ZZ9KPictureSource *source,
                                    uint32_t *size)
{
  if (!source || !size) {
    return 0;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_MEMORY) {
    if (source->size == 0U) {
      return 0;
    }
    *size = source->size;
    return 1;
  }
  if (source->type == ZZ9K_PICTURE_SOURCE_FILE) {
    LONG original_pos;
    LONG end_pos;

    original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
    if (original_pos < 0) {
      return 0;
    }
    end_pos = zz9k_picture_source_seek(source, 0, OFFSET_END);
    (void)zz9k_picture_source_seek(source, original_pos, OFFSET_BEGINNING);
    if (end_pos <= 0 || end_pos > (LONG)0x7ffffffeUL) {
      return 0;
    }
    *size = (uint32_t)end_pos;
    return 1;
  }
  return 0;
}

static int zz9k_picture_read_exact(ZZ9KPictureSource *source,
                                   uint8_t *dst,
                                   uint32_t length)
{
  return source && dst &&
         zz9k_picture_source_read(source, dst, length) == (LONG)length;
}

static int zz9k_picture_skip_bytes(ZZ9KPictureSource *source,
                                   uint32_t length)
{
  return source &&
         zz9k_picture_source_seek(source, (LONG)length,
                                  OFFSET_CURRENT) >= 0;
}

static int zz9k_picture_jpeg_sof_marker(uint8_t marker)
{
  switch (marker) {
  case 0xc0:
  case 0xc1:
  case 0xc2:
  case 0xc3:
  case 0xc5:
  case 0xc6:
  case 0xc7:
  case 0xc9:
  case 0xca:
  case 0xcb:
  case 0xcd:
  case 0xce:
  case 0xcf:
    return 1;
  default:
    return 0;
  }
}

static int zz9k_picture_jpeg_standalone_marker(uint8_t marker)
{
  return marker == 0x01U || marker == 0xd8U || marker == 0xd9U ||
         (marker >= 0xd0U && marker <= 0xd7U);
}

static int zz9k_picture_read_jpeg_dimensions(ZZ9KPictureSource *source,
                                             uint32_t *out_width,
                                             uint32_t *out_height)
{
  uint8_t bytes[6];

  if (!source || !zz9k_picture_read_exact(source, bytes, 2U) ||
      bytes[0] != 0xffU || bytes[1] != 0xd8U) {
    return 0;
  }

  for (;;) {
    uint8_t marker;
    uint16_t segment_length;
    uint32_t payload_length;

    do {
      if (!zz9k_picture_read_exact(source, bytes, 1U)) {
        return 0;
      }
    } while (bytes[0] == 0xffU);

    marker = bytes[0];
    if (zz9k_picture_jpeg_standalone_marker(marker)) {
      continue;
    }
    if (marker == 0xdaU || marker == 0xd9U) {
      return 0;
    }
    if (!zz9k_picture_read_exact(source, bytes, 2U)) {
      return 0;
    }

    segment_length = zz9k_picture_read_be16(bytes);
    if (segment_length < 2U) {
      return 0;
    }
    payload_length = (uint32_t)segment_length - 2U;

    if (zz9k_picture_jpeg_sof_marker(marker)) {
      uint32_t width;
      uint32_t height;

      if (payload_length < 6U ||
          !zz9k_picture_read_exact(source, bytes, 6U)) {
        return 0;
      }
      height = zz9k_picture_read_be16(&bytes[1]);
      width = zz9k_picture_read_be16(&bytes[3]);
      if (width == 0U || height == 0U) {
        return 0;
      }
      *out_width = width;
      *out_height = height;
      return 1;
    }

    if (!zz9k_picture_skip_bytes(source, payload_length)) {
      return 0;
    }
  }
}

static int zz9k_picture_read_png_metadata(ZZ9KPictureSource *source,
                                          uint32_t *out_width,
                                          uint32_t *out_height,
                                          int *has_alpha,
                                          uint8_t *out_interlace,
                                          ZZ9KPicturePngPalette *palette)
{
  static const uint8_t signature[8] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
  };
  uint8_t trns[ZZ9K_PICTURE_PNG_PALETTE_MAX_ENTRIES];
  uint8_t header[33];
  uint8_t chunk[8];
  uint32_t length;
  uint32_t width;
  uint32_t height;
  uint8_t color_type;

  if (palette) {
    memset(palette, 0, sizeof(*palette));
    palette->transparent_index = ZZ9K_PICTURE_PNG_TRANSPARENT_NONE;
  }
  if (!source ||
      !zz9k_picture_read_exact(source, header, (uint32_t)sizeof(header))) {
    return 0;
  }
  if (memcmp(header, signature, sizeof(signature)) != 0 ||
      zz9k_picture_read_be32(&header[8]) != 13U ||
      memcmp(&header[12], "IHDR", 4U) != 0) {
    return 0;
  }

  width = zz9k_picture_read_be32(&header[16]);
  height = zz9k_picture_read_be32(&header[20]);
  if (width == 0U || height == 0U) {
    return 0;
  }

  *out_width = width;
  *out_height = height;
  if (!has_alpha) {
    return 1;
  }

  color_type = header[25];
  if (out_interlace) {
    *out_interlace = header[28] != 0U ? 1U : 0U;
  }
  *has_alpha = (color_type & 4U) != 0U;
  if (*has_alpha) {
    return 1;
  }
  for (;;) {
    if (!zz9k_picture_read_exact(source, chunk, 8U)) {
      return 0;
    }
    length = zz9k_picture_read_be32(chunk);
    if (palette && color_type == 3U &&
        memcmp(&chunk[4], "PLTE", 4U) == 0 &&
        length >= 3U && length <= 768U && (length % 3U) == 0U) {
      if (!zz9k_picture_read_exact(source, palette->rgb, length) ||
          !zz9k_picture_skip_bytes(source, 4U)) {
        return 0;
      }
      palette->count = (uint16_t)(length / 3U);
      palette->present = 1U;
      continue;
    }
    if (memcmp(&chunk[4], "tRNS", 4U) == 0) {
      if (palette && palette->present &&
          length >= 1U && length <= ZZ9K_PICTURE_PNG_PALETTE_MAX_ENTRIES) {
        uint32_t index;

        if (!zz9k_picture_read_exact(source, trns, length) ||
            !zz9k_picture_skip_bytes(source, 4U)) {
          return 0;
        }
        palette->transparent_index = ZZ9K_PICTURE_PNG_TRANSPARENT_NONE;
        for (index = 0U; index < length; index++) {
          if (trns[index] == 0U) {
            palette->transparent_mask[index >> 3] |=
                (uint8_t)(1U << (index & 7U));
            if (palette->transparent_index ==
                ZZ9K_PICTURE_PNG_TRANSPARENT_NONE) {
              palette->transparent_index = (uint16_t)index;
            }
          } else if (trns[index] != 255U) {
            /* Partial alpha cannot be represented by a single transparent
             * colour; the caller keeps the per-pixel alpha path. */
            palette->partial_alpha = 1U;
          }
        }
      }
      *has_alpha = 1;
      return 1;
    }
    if (memcmp(&chunk[4], "IDAT", 4U) == 0 ||
        memcmp(&chunk[4], "IEND", 4U) == 0) {
      return 1;
    }
    if (length > 0x7ffffff0UL ||
        !zz9k_picture_skip_bytes(source, length + 4U)) {
      return 0;
    }
  }
}

static int zz9k_picture_read_png_dimensions(ZZ9KPictureSource *source,
                                            uint32_t *out_width,
                                            uint32_t *out_height)
{
  return zz9k_picture_read_png_metadata(source, out_width, out_height,
                                        0, 0, 0);
}

static int zz9k_picture_read_png_metadata_with_alpha(
    ZZ9KPictureSource *source,
    uint32_t *out_width,
    uint32_t *out_height,
    int *has_alpha,
    uint8_t *out_interlace,
    ZZ9KPicturePngPalette *palette)
{
  return zz9k_picture_read_png_metadata(source, out_width, out_height,
                                        has_alpha, out_interlace, palette);
}

static int zz9k_picture_seek_begin(ZZ9KPictureSource *source);
static void zz9k_picture_restore_pos(ZZ9KPictureSource *source, LONG pos);

static int zz9k_picture_read_png_alpha_flag(ZZ9KPictureSource *source,
                                            int *has_alpha)
{
  uint32_t width;
  uint32_t height;

  if (!has_alpha) {
    return 0;
  }

  width = 0U;
  height = 0U;
  return zz9k_picture_read_png_metadata_with_alpha(
      source, &width, &height, has_alpha, 0, 0);
}

static int zz9k_picture_png_has_alpha(ZZ9KPictureSource *source,
                                      int *has_alpha)
{
  LONG original_pos;
  int ok;

  if (!source || !has_alpha) {
    return 0;
  }

  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    return 0;
  }
  ok = zz9k_picture_read_png_alpha_flag(source, has_alpha);
  zz9k_picture_restore_pos(source, original_pos);
  return ok;
}

static int zz9k_picture_seek_begin(ZZ9KPictureSource *source)
{
  return source &&
         zz9k_picture_source_seek(source, 0, OFFSET_BEGINNING) >= 0;
}

static void zz9k_picture_restore_pos(ZZ9KPictureSource *source, LONG pos)
{
  if (source && pos >= 0) {
    (void)zz9k_picture_source_seek(source, pos, OFFSET_BEGINNING);
  }
}

static int zz9k_picture_file_size(ZZ9KPictureSource *source, uint32_t *size)
{
  return zz9k_picture_source_size(source, size);
}

static int zz9k_picture_read_webp_metadata(
    ZZ9KPictureSource *source,
    uint32_t *out_width,
    uint32_t *out_height,
    int *has_alpha)
{
  uint8_t header[ZZ9K_WEBP_HEADER_MIN_BYTES];
  ZZ9KWebPHeader parsed;
  ZZ9KWebPParseStatus status;

  if (!source || !out_width || !out_height) {
    return 0;
  }

  if (!zz9k_picture_read_exact(source, header, (uint32_t)sizeof(header))) {
    return 0;
  }

  status = zz9k_webp_parse_header(header, (uint32_t)sizeof(header), &parsed);
  if (status != ZZ9K_WEBP_PARSE_READY) {
    return 0;
  }

  *out_width = parsed.width;
  *out_height = parsed.height;
  if (has_alpha) {
    *has_alpha = parsed.has_alpha ? 1 : 0;
  }
  return 1;
}

static int zz9k_picture_read_dimensions(ZZ9KPictureSource *source,
                                        ZZ9KPictureCodec *codec,
                                        uint32_t *width,
                                        uint32_t *height,
                                        int *png_has_alpha,
                                        uint8_t *png_interlace,
                                        ZZ9KPicturePngPalette *palette)
{
  LONG original_pos;

  if (!source || !codec || !width || !height) {
    return 0;
  }

  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    return 0;
  }
  if (zz9k_picture_read_jpeg_dimensions(source, width, height)) {
    zz9k_picture_restore_pos(source, original_pos);
    *codec = ZZ9K_PICTURE_CODEC_JPEG;
    if (png_has_alpha) {
      *png_has_alpha = 0;
    }
    return 1;
  }

  if (!zz9k_picture_seek_begin(source)) {
    zz9k_picture_restore_pos(source, original_pos);
    return 0;
  }
  if (zz9k_picture_read_png_metadata_with_alpha(
          source, width, height, png_has_alpha, png_interlace, palette)) {
    zz9k_picture_restore_pos(source, original_pos);
    *codec = ZZ9K_PICTURE_CODEC_PNG;
    return 1;
  }

  if (!zz9k_picture_seek_begin(source)) {
    zz9k_picture_restore_pos(source, original_pos);
    return 0;
  }
  {
    int webp_has_alpha = 0;
    if (zz9k_picture_read_webp_metadata(
            source, width, height, &webp_has_alpha)) {
      zz9k_picture_restore_pos(source, original_pos);
      *codec = ZZ9K_PICTURE_CODEC_WEBP;
      if (png_has_alpha) {
        *png_has_alpha = webp_has_alpha;
      }
      if (png_interlace) {
        *png_interlace = 0U;
      }
      return 1;
    }
  }

  zz9k_picture_restore_pos(source, original_pos);
  return 0;
}

static const char *zz9k_picture_object_name(ZZ9KPictureCodec codec)
{
  switch (codec) {
  case ZZ9K_PICTURE_CODEC_JPEG:
    return "ZZ9000 JPEG";
  case ZZ9K_PICTURE_CODEC_PNG:
    return "ZZ9000 PNG";
  case ZZ9K_PICTURE_CODEC_WEBP:
    return "ZZ9000 WebP";
  default:
    return "ZZ9000 Picture";
  }
}

static const char *zz9k_picture_instance_object_name(
    const ZZ9KPictureInstance *instance)
{
  if (instance && instance->object_name[0] != '\0') {
    return instance->object_name;
  }
  return zz9k_picture_object_name(
      instance ? instance->codec : ZZ9K_PICTURE_CODEC_UNKNOWN);
}

static const char *zz9k_picture_leaf_object_name(const char *name)
{
  const char *leaf;

  if (!name || name[0] == '\0') {
    return 0;
  }

  leaf = name;
  while (*name != '\0') {
    if (*name == '/' || *name == ':') {
      leaf = name + 1;
    }
    name++;
  }
  return leaf && leaf[0] != '\0' ? leaf : 0;
}

static void zz9k_picture_copy_object_name(ZZ9KPictureInstance *instance,
                                          const char *name)
{
  const char *leaf;
  size_t length;

  if (!instance) {
    return;
  }

  instance->object_name[0] = '\0';
  leaf = zz9k_picture_leaf_object_name(name);
  if (!leaf) {
    return;
  }

  length = strlen(leaf);
  if (length >= ZZ9K_PICTURE_OBJECT_NAME_BYTES) {
    length = ZZ9K_PICTURE_OBJECT_NAME_BYTES - 1U;
  }
  memcpy(instance->object_name, leaf, length);
  instance->object_name[length] = '\0';
  zz9k_picture_trace("metadata: source object name");
}

static void zz9k_picture_capture_screen_alpha_policy(
    ZZ9KPictureInstance *instance,
    const struct TagItem *attrs)
{
  struct Screen *screen;
  ULONG screen_depth;

  if (!instance || !attrs) {
    return;
  }

  screen = (struct Screen *)GetTagData(
      PDTA_Screen, 0UL, (struct TagItem *)attrs);
  if (!screen || !screen->RastPort.BitMap) {
    return;
  }

  screen_depth = GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH);
  if (screen_depth <= 16U) {
    instance->flatten_png_alpha = 1U;
    zz9k_picture_trace_source(
        "metadata: png alpha flattened for low-depth screen");
  }
}

static void zz9k_picture_capture_object_name(
    Object *object,
    ZZ9KPictureInstance *instance)
{
  STRPTR source_name;

  if (!object || !instance) {
    return;
  }

  source_name = 0;
  if (GetDTAttrs(
          object,
          DTA_Name, (ULONG)&source_name,
          TAG_END) >= 1) {
    zz9k_picture_copy_object_name(instance, (const char *)source_name);
  }
}

static void zz9k_picture_apply_object_name(
    Object *object,
    const ZZ9KPictureInstance *instance)
{
  if (!object || !instance) {
    return;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_ObjName, (ULONG)zz9k_picture_instance_object_name(instance),
      TAG_END);
  zz9k_picture_trace("metadata: source object name applied");
}

static void zz9k_picture_fill_placeholder_colors(Object *object)
{
  struct ColorRegister *colors;
  ULONG *cregs;

  colors = 0;
  cregs = 0;
  (void)GetDTAttrs(
      object,
      PDTA_ColorRegisters, (ULONG)&colors,
      PDTA_CRegs, (ULONG)&cregs,
      TAG_END);
  if (colors) {
    colors[0].red = 0;
    colors[0].green = 0;
    colors[0].blue = 0;
    colors[1].red = 255;
    colors[1].green = 255;
    colors[1].blue = 255;
  }
  if (cregs) {
    cregs[0] = 0x00000000UL;
    cregs[1] = 0x00000000UL;
    cregs[2] = 0x00000000UL;
    cregs[3] = 0xffffffffUL;
    cregs[4] = 0xffffffffUL;
    cregs[5] = 0xffffffffUL;
  }
}

static int zz9k_picture_accumulate_surface_bytes(uint32_t height,
                                                 uint32_t pitch,
                                                 uint32_t *length);

static UWORD zz9k_picture_superclass_version(void)
{
  return PictureBase ? PictureBase->lib_Version : 0;
}

static uint32_t zz9k_picture_pbpa_bytes_per_pixel(ULONG pixel_format)
{
  switch (pixel_format) {
  case PBPAFMT_RGB:
    return 3U;
  case PBPAFMT_RGBA:
  case PBPAFMT_ARGB:
    return 4U;
  case PBPAFMT_LUT8:
  case PBPAFMT_GREY8:
    return 1U;
  default:
    return 0U;
  }
}

static int zz9k_picture_mul_small_u32(uint32_t value,
                                      uint32_t multiplier,
                                      uint32_t *product)
{
  uint32_t total;
  uint32_t i;

  if (!product || value == 0U || multiplier == 0U) {
    return 0;
  }

  total = 0U;
  for (i = 0U; i < multiplier; i++) {
    if (total > (0xffffffffUL - value)) {
      return 0;
    }
    total += value;
  }

  *product = total;
  return total != 0U;
}

static int zz9k_picture_min_row_bytes(uint32_t width,
                                      uint32_t bytes_per_pixel,
                                      uint32_t *row_bytes)
{
  return zz9k_picture_mul_small_u32(width, bytes_per_pixel, row_bytes);
}

static void zz9k_picture_init_pixel_request(
    struct pdtBlitPixelArray *pixels,
    uint32_t width,
    uint32_t height,
    ULONG pixel_format)
{
  if (!pixels) {
    return;
  }
  memset(pixels, 0, sizeof(*pixels));
  pixels->pbpa_PixelFormat = pixel_format;
  pixels->pbpa_Width = width;
  pixels->pbpa_Height = height;
}

static int zz9k_picture_set_v43_pixel_attrs(Object *object,
                                            ZZ9KPictureCodec codec,
                                            uint32_t width,
                                            uint32_t height,
                                            ULONG remap,
                                            ULONG renders_all)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: invalid placeholder dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, renders_all,
      PDTA_Remap, remap,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: v43 pixel attrs set");
  return 1;
}

static int zz9k_picture_pixel_buffer_valid(
    const struct pdtBlitPixelArray *pixels,
    uint32_t width,
    uint32_t height,
    uint32_t *pixel_bytes)
{
  uint32_t bytes_per_pixel;
  uint32_t min_row_bytes;

  if (!pixels || !pixels->pbpa_PixelData ||
      pixels->pbpa_PixelArrayMod == 0UL ||
      pixels->pbpa_Width < width ||
      pixels->pbpa_Height < height) {
    return 0;
  }

  bytes_per_pixel =
      zz9k_picture_pbpa_bytes_per_pixel(pixels->pbpa_PixelFormat);
  if (!zz9k_picture_min_row_bytes(width, bytes_per_pixel, &min_row_bytes) ||
      pixels->pbpa_PixelArrayMod < min_row_bytes) {
    return 0;
  }

  return zz9k_picture_accumulate_surface_bytes(
      height, pixels->pbpa_PixelArrayMod, pixel_bytes);
}

static int zz9k_picture_pixel_buffer_format_usable(
    const struct pdtBlitPixelArray *pixels,
    ULONG requested_format)
{
  if (!pixels) {
    return 0;
  }
  if (requested_format == PBPAFMT_GREY8 &&
      pixels->pbpa_PixelFormat == PBPAFMT_LUT8) {
    zz9k_picture_trace("metadata: v47 grey8 lut8 buffer");
    return 1;
  }
  if (pixels->pbpa_PixelFormat != requested_format) {
    zz9k_picture_trace("metadata: v47 unsupported pixel format");
    return 0;
  }
  return 1;
}

static int zz9k_picture_direct_pixel_buffer_valid(
    const struct pdtBlitPixelArray *pixels,
    ULONG requested_format,
    uint32_t width,
    uint32_t height)
{
  uint32_t bytes_per_pixel;
  uint32_t min_row_bytes;

  if (!zz9k_picture_pixel_buffer_format_usable(pixels, requested_format) ||
      !pixels->pbpa_PixelData ||
      pixels->pbpa_PixelArrayMod == 0UL ||
      pixels->pbpa_Width < width ||
      pixels->pbpa_Height < height) {
    zz9k_picture_trace("metadata: v47 buffer invalid");
    return 0;
  }

  bytes_per_pixel =
      zz9k_picture_pbpa_bytes_per_pixel(pixels->pbpa_PixelFormat);
  if (bytes_per_pixel == 0U) {
    zz9k_picture_trace("metadata: v47 buffer invalid");
    return 0;
  }
  if (bytes_per_pixel == 1U) {
    min_row_bytes = width;
  } else if (!zz9k_picture_min_row_bytes(
                 width, bytes_per_pixel, &min_row_bytes)) {
    zz9k_picture_trace("metadata: v47 buffer invalid");
    return 0;
  }
  if (pixels->pbpa_PixelArrayMod < min_row_bytes) {
    zz9k_picture_trace("metadata: v47 buffer invalid");
    return 0;
  }

  zz9k_picture_trace("metadata: v47 buffer accepted");
  return 1;
}

static int zz9k_picture_prepare_lut8_palette(
    Object *object,
    ZZ9KPictureInstance *instance)
{
  struct ColorRegister *colors;
  ULONG *cregs;
  uint32_t i;

  if (!object || !instance) {
    zz9k_picture_trace("metadata: v47 lut8 palette skipped");
    return 0;
  }

  (void)SetDTAttrs(object, 0, 0, PDTA_NumColors, 256, TAG_END);
  colors = 0;
  cregs = 0;
  (void)GetDTAttrs(
      object,
      PDTA_ColorRegisters, (ULONG)&colors,
      PDTA_CRegs, (ULONG)&cregs,
      TAG_END);
  if (!colors || !cregs) {
    zz9k_picture_trace("metadata: v47 lut8 palette unavailable");
    return 0;
  }

  for (i = 0U; i < 256U; i++) {
    UBYTE level8;
    ULONG level32;

    level8 = (UBYTE)i;
    level32 = ((ULONG)level8 << 24) |
              ((ULONG)level8 << 16) |
              ((ULONG)level8 << 8) |
              (ULONG)level8;
    colors[i].red = level8;
    colors[i].green = level8;
    colors[i].blue = level8;
    cregs[(i * 3U) + 0U] = level32;
    cregs[(i * 3U) + 1U] = level32;
    cregs[(i * 3U) + 2U] = level32;
  }

  zz9k_picture_trace("metadata: v47 lut8 palette ready");
  return 1;
}

static int zz9k_picture_set_v47_direct_attrs(Object *object,
                                             ZZ9KPictureCodec codec,
                                             uint32_t width,
                                             uint32_t height)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: invalid placeholder dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, TRUE,
      PDTA_Remap, TRUE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: v47 direct attrs ready");
  return 1;
}

static int zz9k_picture_set_png_v47_direct_attrs(Object *object,
                                                 ZZ9KPictureCodec codec,
                                                 uint32_t width,
                                                 uint32_t height)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: invalid placeholder dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, FALSE,
      PDTA_Remap, FALSE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: png v47 direct attrs ready");
  return 1;
}

static int zz9k_picture_prepare_direct_pixel_header(
    Object *object,
    const struct pdtBlitPixelArray *pixels,
    uint32_t width,
    uint32_t height)
{
  struct BitMapHeader *header;
  UBYTE depth;

  if (!object || !pixels || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    return 0;
  }

  switch (pixels->pbpa_PixelFormat) {
  case PBPAFMT_RGB:
    depth = 24;
    break;
  case PBPAFMT_RGBA:
  case PBPAFMT_ARGB:
    depth = 32;
    break;
  case PBPAFMT_LUT8:
  case PBPAFMT_GREY8:
    depth = 8;
    break;
  default:
    return 0;
  }

  zz9k_picture_trace("metadata: before direct header");
  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: direct header unavailable");
    return 0;
  }

  header->bmh_Width = (UWORD)width;
  header->bmh_Height = (UWORD)height;
  header->bmh_Depth = depth;
  header->bmh_Masking =
      (pixels->pbpa_PixelFormat == PBPAFMT_RGBA ||
       pixels->pbpa_PixelFormat == PBPAFMT_ARGB) ?
      mskHasAlpha : mskNone;
  header->bmh_Compression = cmpNone;
  header->bmh_Pad = 0;
  header->bmh_XAspect = 1;
  header->bmh_YAspect = 1;
  header->bmh_PageWidth = (WORD)width;
  header->bmh_PageHeight = (WORD)height;
  zz9k_picture_trace_u32("metadata: direct header depth", (uint32_t)depth);
  zz9k_picture_trace("metadata: direct header ready");
  return 1;
}

static void zz9k_picture_trace_pixel_buffer(
    const struct pdtBlitPixelArray *pixels)
{
  if (!pixels) {
    zz9k_picture_trace("metadata: v47 pixel buffer missing");
    return;
  }

  zz9k_picture_trace_hex(
      "metadata: v47 pixeldata", (uint32_t)(ULONG)pixels->pbpa_PixelData);
  zz9k_picture_trace_u32(
      "metadata: v47 pixelformat", (uint32_t)pixels->pbpa_PixelFormat);
  zz9k_picture_trace_u32(
      "metadata: v47 pixelmod", (uint32_t)pixels->pbpa_PixelArrayMod);
  zz9k_picture_trace_u32(
      "metadata: v47 pixelwidth", (uint32_t)pixels->pbpa_Width);
  zz9k_picture_trace_u32(
      "metadata: v47 pixelheight", (uint32_t)pixels->pbpa_Height);
}

static int zz9k_picture_obtain_pixels_with_tag(
    Object *object,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height,
    ULONG pixel_format,
    uint32_t *pixel_bytes)
{
  ULONG result;

  if (pixel_format == PBPAFMT_LUT8 &&
      !zz9k_picture_prepare_lut8_palette(object, instance)) {
    return 0;
  }
  if (!zz9k_picture_set_v47_direct_attrs(
          object, codec, width, height)) {
    return 0;
  }

  zz9k_picture_init_pixel_request(pixels, width, height, pixel_format);
  result = SetDTAttrs(
      object, 0, 0,
      PDTA_ObtainPixelBuffer, (ULONG)pixels,
      TAG_END);
  zz9k_picture_trace_u32("metadata: v47 tag result", (uint32_t)result);
  zz9k_picture_trace_pixel_buffer(pixels);
  (void)pixel_bytes;
  return zz9k_picture_direct_pixel_buffer_valid(
      pixels, pixel_format, width, height);
}

static int zz9k_picture_obtain_pixels_with_method(
    Object *object,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height,
    ULONG pixel_format,
    uint32_t *pixel_bytes)
{
  struct pdtObtainPixelArray obtain;
  ULONG result;

  if (pixel_format == PBPAFMT_LUT8 &&
      !zz9k_picture_prepare_lut8_palette(object, instance)) {
    return 0;
  }
  if (!zz9k_picture_set_v47_direct_attrs(
          object, codec, width, height)) {
    return 0;
  }

  zz9k_picture_init_pixel_request(pixels, width, height, pixel_format);
  memset(&obtain, 0, sizeof(obtain));
  obtain.MethodID = PDTM_OBTAINPIXELARRAY;
  obtain.popa_PixelArray = pixels;
  obtain.popa_Flags = pixel_format == PBPAFMT_GREY8 ?
      POPAF_WRITEGREY8 : 0UL;
  (void)codec;
  result = DoMethodA(object, (Msg)&obtain);
  zz9k_picture_trace_u32("metadata: v47 method result", (uint32_t)result);
  zz9k_picture_trace_pixel_buffer(pixels);
  (void)pixel_bytes;
  return result != 0UL &&
         zz9k_picture_direct_pixel_buffer_valid(
             pixels, pixel_format, width, height);
}

static int zz9k_picture_obtain_direct_pixels(
    Object *object,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height,
    uint32_t *pixel_bytes)
{
  static const ULONG formats[] = {
    PBPAFMT_RGB, PBPAFMT_RGBA, PBPAFMT_ARGB, PBPAFMT_GREY8,
    PBPAFMT_LUT8
  };
  uint32_t i;

  if (!object || !pixels || !pixel_bytes) {
    return 0;
  }

  for (i = 0U; i < (uint32_t)(sizeof(formats) / sizeof(formats[0])); i++) {
    zz9k_picture_trace_u32(
        "metadata: v47 request format", (uint32_t)formats[i]);
    if (formats[i] == PBPAFMT_GREY8) {
      zz9k_picture_trace("metadata: obtain pixel buffer method");
      if (zz9k_picture_obtain_pixels_with_method(
              object, pixels, instance, codec, width, height, formats[i],
              pixel_bytes)) {
        if (!zz9k_picture_prepare_direct_pixel_header(
                object, pixels, width, height)) {
          zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
          return 0;
        }
        zz9k_picture_trace("metadata: obtained pixel buffer ready");
        return 1;
      }
      continue;
    }

    zz9k_picture_trace("metadata: obtain pixel buffer tag");
    if (zz9k_picture_obtain_pixels_with_tag(
            object, pixels, instance, codec, width, height, formats[i],
            pixel_bytes)) {
      if (!zz9k_picture_prepare_direct_pixel_header(
              object, pixels, width, height)) {
        zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
        return 0;
      }
      zz9k_picture_trace("metadata: obtained pixel buffer ready");
      return 1;
    }

    zz9k_picture_trace("metadata: obtain pixel buffer method");
    if (zz9k_picture_obtain_pixels_with_method(
            object, pixels, instance, codec, width, height, formats[i],
            pixel_bytes)) {
      if (!zz9k_picture_prepare_direct_pixel_header(
              object, pixels, width, height)) {
        zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
        return 0;
      }
      zz9k_picture_trace("metadata: obtained pixel buffer ready");
      return 1;
    }
  }

  zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
  return 0;
}

static int zz9k_picture_obtain_truecolor_direct_pixels(
    Object *object,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height,
    uint32_t *pixel_bytes)
{
  static const ULONG truecolor_formats[] = {
    PBPAFMT_RGB, PBPAFMT_RGBA, PBPAFMT_ARGB
  };
  uint32_t i;

  if (!object || !pixels || !pixel_bytes) {
    return 0;
  }

  for (i = 0U;
       i < (uint32_t)(sizeof(truecolor_formats) /
                      sizeof(truecolor_formats[0]));
       i++) {
    zz9k_picture_trace_u32(
        "metadata: v47 request format", (uint32_t)truecolor_formats[i]);
    zz9k_picture_trace("metadata: obtain pixel buffer tag");
    if (zz9k_picture_obtain_pixels_with_tag(
            object, pixels, instance, codec, width, height,
            truecolor_formats[i], pixel_bytes)) {
      if (!zz9k_picture_prepare_direct_pixel_header(
              object, pixels, width, height)) {
        zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
        return 0;
      }
      zz9k_picture_trace("metadata: obtained pixel buffer ready");
      return 1;
    }
    if (pixels->pbpa_PixelFormat == PBPAFMT_LUT8 ||
        pixels->pbpa_PixelFormat == PBPAFMT_GREY8) {
      zz9k_picture_trace("metadata: v47 indexed buffer rejected");
    }

    zz9k_picture_trace("metadata: obtain pixel buffer method");
    if (zz9k_picture_obtain_pixels_with_method(
            object, pixels, instance, codec, width, height,
            truecolor_formats[i], pixel_bytes)) {
      if (!zz9k_picture_prepare_direct_pixel_header(
              object, pixels, width, height)) {
        zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
        return 0;
      }
      zz9k_picture_trace("metadata: obtained pixel buffer ready");
      return 1;
    }
    if (pixels->pbpa_PixelFormat == PBPAFMT_LUT8 ||
        pixels->pbpa_PixelFormat == PBPAFMT_GREY8) {
      zz9k_picture_trace("metadata: v47 indexed buffer rejected");
    }
  }

  zz9k_picture_trace("metadata: obtained pixel buffer unavailable");
  return 0;
}

#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
static int zz9k_picture_alpha_pixel_format_to_surface_format(
    ULONG pixel_format,
    uint32_t *surface_format)
{
  if (!surface_format) {
    return 0;
  }
  if (pixel_format == PBPAFMT_RGBA) {
    *surface_format = ZZ9K_SURFACE_FORMAT_RGBA8888;
    return 1;
  }
  if (pixel_format == PBPAFMT_ARGB) {
    *surface_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
    return 1;
  }
  return 0;
}

static int zz9k_picture_alpha_surface_format_to_pbpa(
    uint32_t surface_format,
    ULONG *pixel_format)
{
  if (!pixel_format) {
    return 0;
  }
  if (surface_format == ZZ9K_SURFACE_FORMAT_RGBA8888) {
    *pixel_format = PBPAFMT_RGBA;
    return 1;
  }
  if (surface_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
    *pixel_format = PBPAFMT_ARGB;
    return 1;
  }
  return 0;
}

static int zz9k_picture_mark_alpha_header(Object *object);
static int zz9k_picture_set_reference_v43_attrs(Object *object,
                                                ZZ9KPictureCodec codec,
                                                uint32_t width,
                                                uint32_t height);
static int zz9k_picture_prepare_png_datatype_v43(
    Object *object,
    const ZZ9KPictureInstance *instance,
    int has_alpha);

static int zz9k_picture_obtain_png_direct_pixels(
    Object *object,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureInstance *instance,
    uint32_t *surface_format)
{
  static const ULONG alpha_formats[] = {
    PBPAFMT_RGBA, PBPAFMT_ARGB
  };
  uint32_t pixel_bytes;
  uint32_t i;

  if (!object || !pixels || !instance || !surface_format) {
    return 0;
  }

  pixel_bytes = 0U;
  for (i = 0U;
       i < (uint32_t)(sizeof(alpha_formats) / sizeof(alpha_formats[0]));
       i++) {
    ULONG result;
    struct pdtObtainPixelArray obtain;

    zz9k_picture_trace_u32(
        "metadata: png v47 request format", (uint32_t)alpha_formats[i]);
    zz9k_picture_trace("metadata: obtain pixel buffer tag");
    if (!zz9k_picture_set_png_v47_direct_attrs(
            object, instance->codec, instance->width, instance->height)) {
      return 0;
    }
    zz9k_picture_init_pixel_request(
        pixels, instance->width, instance->height, alpha_formats[i]);
    result = SetDTAttrs(
        object, 0, 0,
        PDTA_ObtainPixelBuffer, (ULONG)pixels,
        TAG_END);
    zz9k_picture_trace_u32(
        "metadata: png v47 tag result", (uint32_t)result);
    zz9k_picture_trace_pixel_buffer(pixels);
    if (zz9k_picture_direct_pixel_buffer_valid(
            pixels, alpha_formats[i], instance->width, instance->height) &&
        zz9k_picture_alpha_pixel_format_to_surface_format(
            pixels->pbpa_PixelFormat, surface_format) &&
        zz9k_picture_prepare_direct_pixel_header(
            object, pixels, instance->width, instance->height) &&
        zz9k_picture_mark_alpha_header(object)) {
      zz9k_picture_trace("metadata: png v47 direct pixels ready");
      return 1;
    }

    zz9k_picture_trace("metadata: obtain pixel buffer method");
    if (!zz9k_picture_set_png_v47_direct_attrs(
            object, instance->codec, instance->width, instance->height)) {
      return 0;
    }
    zz9k_picture_init_pixel_request(
        pixels, instance->width, instance->height, alpha_formats[i]);
    memset(&obtain, 0, sizeof(obtain));
    obtain.MethodID = PDTM_OBTAINPIXELARRAY;
    obtain.popa_PixelArray = pixels;
    obtain.popa_Flags = 0UL;
    result = DoMethodA(object, (Msg)&obtain);
    zz9k_picture_trace_u32(
        "metadata: png v47 method result", (uint32_t)result);
    zz9k_picture_trace_pixel_buffer(pixels);
    if (result != 0UL &&
        zz9k_picture_direct_pixel_buffer_valid(
            pixels, alpha_formats[i], instance->width, instance->height) &&
        zz9k_picture_alpha_pixel_format_to_surface_format(
            pixels->pbpa_PixelFormat, surface_format) &&
        zz9k_picture_prepare_direct_pixel_header(
            object, pixels, instance->width, instance->height) &&
        zz9k_picture_mark_alpha_header(object)) {
      zz9k_picture_trace("metadata: png v47 direct pixels ready");
      return 1;
    }
  }

  (void)pixel_bytes;
  zz9k_picture_trace("metadata: png v47 direct pixels unavailable");
  return 0;
}
#endif

static int zz9k_picture_set_placeholder_header(Object *object,
                                               uint32_t width,
                                               uint32_t height,
                                               UBYTE depth,
                                               UWORD num_colors)
{
  struct BitMapHeader *header;

  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: invalid placeholder dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  header = 0;
  if (num_colors != 0) {
    (void)SetDTAttrs(object, 0, 0, PDTA_NumColors, num_colors, TAG_END);
  }
  (void)GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END);
  if (!header) {
    zz9k_picture_trace("metadata: placeholder header unavailable");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  memset(header, 0, sizeof(*header));
  header->bmh_Width = (UWORD)width;
  header->bmh_Height = (UWORD)height;
  header->bmh_Depth = depth;
  header->bmh_XAspect = 1;
  header->bmh_YAspect = 1;
  header->bmh_PageWidth = (WORD)width;
  header->bmh_PageHeight = (WORD)height;
  if (num_colors != 0) {
    zz9k_picture_fill_placeholder_colors(object);
  }
  return 1;
}

static uint8_t zz9k_picture_legacy_lut8_component(uint8_t value)
{
  return (uint8_t)(((uint32_t)value * 5U + 127U) / 255U);
}

static ULONG zz9k_picture_legacy_lut8_creg(uint8_t value)
{
  return ((ULONG)value << 24) |
         ((ULONG)value << 16) |
         ((ULONG)value << 8) |
         (ULONG)value;
}

static int zz9k_picture_prepare_legacy_lut8_palette(
    Object *object,
    ZZ9KPictureInstance *instance)
{
  struct ColorRegister *colors;
  ULONG *cregs;
  uint32_t index;

  if (!object || !instance) {
    return 0;
  }

  (void)SetDTAttrs(object, 0, 0, PDTA_NumColors, 256, TAG_END);
  colors = 0;
  cregs = 0;
  (void)GetDTAttrs(
      object,
      PDTA_ColorRegisters, (ULONG)&colors,
      PDTA_CRegs, (ULONG)&cregs,
      TAG_END);
  if (!colors || !cregs) {
    zz9k_picture_trace(
        "metadata: datatype legacy lut8 palette unavailable");
    return 0;
  }

  for (index = 0U; index < 256U; index++) {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (index == 0U || index > 216U) {
      red = 0U;
      green = 0U;
      blue = 0U;
    } else {
      uint32_t entry;

      entry = index - 1U;
      red = (uint8_t)((entry / 36U) * 51U);
      green = (uint8_t)(((entry / 6U) % 6U) * 51U);
      blue = (uint8_t)((entry % 6U) * 51U);
    }

    colors[index].red = red;
    colors[index].green = green;
    colors[index].blue = blue;
    cregs[(index * 3U) + 0U] =
        zz9k_picture_legacy_lut8_creg(red);
    cregs[(index * 3U) + 1U] =
        zz9k_picture_legacy_lut8_creg(green);
    cregs[(index * 3U) + 2U] =
        zz9k_picture_legacy_lut8_creg(blue);
  }

  zz9k_picture_trace("metadata: datatype legacy lut8 palette ready");
  return 1;
}

static int zz9k_picture_prepare_legacy_bitmap(
    Object *object,
    ZZ9KPictureInstance *instance,
    int has_alpha,
    struct BitMap **bitmap_out,
    struct BitMap **line_bitmap_out)
{
  struct BitMapHeader *header;
  struct BitMap *bitmap;
  struct BitMap *line_bitmap;

  if (!GfxBase || !object || !instance || !bitmap_out ||
      !line_bitmap_out || instance->width == 0U ||
      instance->height == 0U) {
    return 0;
  }
  *bitmap_out = 0;
  *line_bitmap_out = 0;

  if (!zz9k_picture_set_placeholder_header(
          object, instance->width, instance->height, 8, 256) ||
      !zz9k_picture_prepare_legacy_lut8_palette(object, instance)) {
    return 0;
  }

  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: datatype legacy header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Depth = 8;
  header->bmh_Masking = has_alpha ? mskHasTransparentColor : mskNone;
  header->bmh_Transparent = 0;
  header->bmh_Compression = cmpNone;

  bitmap = AllocBitMap(
      instance->width, instance->height, 8,
      BMF_CLEAR | BMF_DISPLAYABLE, 0);
  if (!bitmap) {
    zz9k_picture_trace("metadata: datatype legacy bitmap alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  line_bitmap = AllocBitMap(instance->width, 1, 8, BMF_CLEAR, 0);
  if (!line_bitmap) {
    FreeBitMap(bitmap);
    zz9k_picture_trace("metadata: datatype legacy line bitmap alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  *bitmap_out = bitmap;
  *line_bitmap_out = line_bitmap;
  zz9k_picture_trace("metadata: datatype legacy bitmap ready");
  return 1;
}

static int zz9k_picture_publish_legacy_bitmap(
    Object *object,
    const ZZ9KPictureInstance *instance,
    struct BitMap *bitmap)
{
  if (!object || !instance || !bitmap) {
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, instance->width,
      DTA_NominalVert, instance->height,
      DTA_TotalHoriz, instance->width,
      DTA_TotalVert, instance->height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_ModeID, 0,
      PDTA_BitMap, (ULONG)bitmap,
      PDTA_FreeSourceBitMap, TRUE,
      PDTA_Remap, TRUE,
      PDTA_AlphaChannel, FALSE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),
      TAG_END);
  zz9k_picture_trace("metadata: datatype legacy bitmap published");
  return 1;
}

static int zz9k_picture_set_placeholder_bitmap(Object *object,
                                               ZZ9KPictureCodec codec,
                                               uint32_t width,
                                               uint32_t height)
{
  struct BitMap *bitmap;

  if (!GfxBase ||
      !zz9k_picture_set_placeholder_header(object, width, height, 1, 2)) {
    return 0;
  }

  bitmap = AllocBitMap(width, height, 1, BMF_CLEAR | BMF_DISPLAYABLE, 0);
  if (!bitmap) {
    zz9k_picture_trace("metadata: placeholder bitmap alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_ModeID, LORES_KEY,
      PDTA_Remap, TRUE,
      PDTA_BitMap, (ULONG)bitmap,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: placeholder bitmap allocated");
  return 1;
}

static int zz9k_picture_set_placeholder_obtained_pixels(
    Object *object,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  struct pdtBlitPixelArray pixels;
  uint32_t pixel_bytes;

  if (!zz9k_picture_set_v43_pixel_attrs(
          object, codec, width, height, TRUE, FALSE)) {
    return 0;
  }

  if (!zz9k_picture_obtain_direct_pixels(
          object, &pixels, 0, codec, width, height, &pixel_bytes)) {
    return 0;
  }

  memset(pixels.pbpa_PixelData, 0, pixel_bytes);
  zz9k_picture_trace("metadata: obtained pixel buffer cleared");
  return 1;
}

static int zz9k_picture_set_placeholder_pixels(Object *object,
                                               ZZ9KPictureCodec codec,
                                               uint32_t width,
                                               uint32_t height)
{
  struct pdtBlitPixelArray pixels;
  uint8_t *pixel_data;
  ULONG row_bytes;
  ULONG pixel_bytes;
  uint32_t pixel_bytes32;
  ULONG result;

  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: invalid placeholder dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  zz9k_picture_trace("metadata: before v43 placeholder attrs");
  if (!zz9k_picture_set_v43_pixel_attrs(
          object, codec, width, height, TRUE, FALSE)) {
    return 0;
  }
  zz9k_picture_trace("metadata: v43 placeholder attrs set");

  zz9k_picture_trace("metadata: before placeholder pixel sizing");
  row_bytes = (ULONG)width * ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
  pixel_bytes32 = 0U;
  if (row_bytes == 0UL ||
      !zz9k_picture_accumulate_surface_bytes(
          height, (uint32_t)row_bytes, &pixel_bytes32)) {
    zz9k_picture_trace("metadata: placeholder pixel size overflow");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }
  pixel_bytes = (ULONG)pixel_bytes32;
  zz9k_picture_trace("metadata: before placeholder pixel alloc");
  pixel_data = (uint8_t *)AllocMem(pixel_bytes, MEMF_PUBLIC | MEMF_CLEAR);
  if (!pixel_data) {
    zz9k_picture_trace("metadata: placeholder pixel alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }
  zz9k_picture_trace("metadata: placeholder pixel alloc ok");

  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = pixel_data;
  pixels.pbpa_PixelFormat = PBPAFMT_RGB;
  pixels.pbpa_PixelArrayMod = row_bytes;
  pixels.pbpa_Left = 0;
  pixels.pbpa_Top = 0;
  pixels.pbpa_Width = width;
  pixels.pbpa_Height = height;
  zz9k_picture_trace("metadata: placeholder pixel descriptor ready");

  zz9k_picture_trace("metadata: before placeholder pixel write");
  result = DoMethodA(object, (Msg)&pixels);
  FreeMem(pixel_data, pixel_bytes);
  if (!result) {
    zz9k_picture_trace("metadata: placeholder pixel write failed");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: placeholder pixels written");
  return 1;
}

static int zz9k_picture_set_placeholder_best(Object *object,
                                             ZZ9KPictureCodec codec,
                                             uint32_t width,
                                             uint32_t height)
{
  UWORD version;

  version = zz9k_picture_superclass_version();
  if (version >= 47U) {
    zz9k_picture_trace("metadata: picture.datatype v47+ path");
    if (zz9k_picture_set_placeholder_obtained_pixels(
            object, codec, width, height)) {
      return 1;
    }
  }
  if (version >= 43U) {
    zz9k_picture_trace("metadata: picture.datatype v43 path");
    return zz9k_picture_set_placeholder_pixels(object, codec, width, height);
  }

  zz9k_picture_trace("metadata: picture.datatype legacy bitmap path");
  return zz9k_picture_set_placeholder_bitmap(object, codec, width, height);
}

static int zz9k_picture_set_capped_placeholder_best(Object *object,
                                                    ZZ9KPictureCodec codec,
                                                    uint32_t width,
                                                    uint32_t height)
{
  if (width > ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE) {
    width = ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE;
  }
  if (height > ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE) {
    height = ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE;
  }
  zz9k_picture_trace("metadata: datatype placeholder capped");
  return zz9k_picture_set_placeholder_best(object, codec, width, height);
}

static int zz9k_picture_reference_mode(ZZ9KPictureRenderMode render_mode)
{
  return render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT;
}

static int zz9k_picture_alpha_reference_mode(
    ZZ9KPictureRenderMode render_mode)
{
  return render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT;
}

static int zz9k_picture_reference_nolayout_mode(
    ZZ9KPictureRenderMode render_mode)
{
  return render_mode == ZZ9K_PICTURE_RENDER_MODE_REFERENCE_NOLAYOUT ||
         render_mode == ZZ9K_PICTURE_RENDER_MODE_ALPHA_REFERENCE_NOLAYOUT;
}

static int zz9k_picture_set_reference_attrs(Object *object,
                                            ZZ9KPictureCodec codec,
                                            uint32_t width,
                                            uint32_t height)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, FALSE,
      PDTA_Remap, TRUE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: reference attrs ready");
  return 1;
}

static int zz9k_picture_prepare_v43_reference_header(Object *object,
                                                     uint32_t width,
                                                     uint32_t height)
{
  struct BitMapHeader *header;

  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 reference before header");
  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: v43 reference header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Width = (UWORD)width;
  header->bmh_Height = (UWORD)height;
  header->bmh_Depth = 24;
  header->bmh_Masking = mskNone;
  header->bmh_Compression = cmpNone;
  header->bmh_Pad = 0;
  header->bmh_XAspect = 1;
  header->bmh_YAspect = 1;
  header->bmh_PageWidth = (WORD)width;
  header->bmh_PageHeight = (WORD)height;
  zz9k_picture_trace("metadata: v43 reference header ready");
  return 1;
}

static int zz9k_picture_prepare_v43_alpha_reference_header(Object *object,
                                                           uint32_t width,
                                                           uint32_t height)
{
  struct BitMapHeader *header;

  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 alpha reference before header");
  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: v43 alpha reference header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Width = (UWORD)width;
  header->bmh_Height = (UWORD)height;
  header->bmh_Depth = 32;
  header->bmh_Masking = mskHasAlpha;
  header->bmh_Compression = cmpNone;
  header->bmh_Pad = 0;
  header->bmh_XAspect = 1;
  header->bmh_YAspect = 1;
  header->bmh_PageWidth = (WORD)width;
  header->bmh_PageHeight = (WORD)height;
  zz9k_picture_trace("metadata: v43 alpha reference header ready");
  return 1;
}

static int zz9k_picture_mark_alpha_header(Object *object)
{
  struct BitMapHeader *header;

  if (!object) {
    return 0;
  }

  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: datatype alpha header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Depth = 32;
  header->bmh_Masking = mskHasAlpha;
  (void)SetDTAttrs(
      object, 0, 0,
      PDTA_AlphaChannel, TRUE,
      TAG_END);
  zz9k_picture_trace("metadata: datatype alpha header ready");
  return 1;
}

static int zz9k_picture_clear_alpha_header(Object *object)
{
  struct BitMapHeader *header;

  if (!object) {
    return 0;
  }

  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: datatype alpha header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Depth = 24;
  header->bmh_Masking = mskNone;
  (void)SetDTAttrs(
      object, 0, 0,
      PDTA_AlphaChannel, FALSE,
      TAG_END);
  zz9k_picture_trace("metadata: datatype alpha header cleared");
  return 1;
}

#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
static uint8_t zz9k_picture_alpha_lut8_component(uint8_t value)
{
  return (uint8_t)(((uint32_t)value * 5U + 127U) / 255U);
}

static ULONG zz9k_picture_alpha_lut8_creg(uint8_t value)
{
  return ((ULONG)value << 24) |
         ((ULONG)value << 16) |
         ((ULONG)value << 8) |
         (ULONG)value;
}

static int zz9k_picture_prepare_alpha_lut8_palette(Object *object,
                                                   ZZ9KPictureInstance *instance)
{
  struct ColorRegister *colors;
  ULONG *cregs;
  uint32_t index;

  if (!object || !instance) {
    return 0;
  }

  (void)SetDTAttrs(object, 0, 0, PDTA_NumColors, 256, TAG_END);
  colors = 0;
  cregs = 0;
  (void)GetDTAttrs(
      object,
      PDTA_ColorRegisters, (ULONG)&colors,
      PDTA_CRegs, (ULONG)&cregs,
      TAG_END);
  if (!colors || !cregs) {
    zz9k_picture_trace("metadata: png alpha lut8 palette unavailable");
    return 0;
  }

  for (index = 0U; index < 256U; index++) {
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (index == 0U || index > 216U) {
      red = 0U;
      green = 0U;
      blue = 0U;
    } else {
      uint32_t entry;

      entry = index - 1U;
      red = (uint8_t)((entry / 36U) * 51U);
      green = (uint8_t)(((entry / 6U) % 6U) * 51U);
      blue = (uint8_t)((entry % 6U) * 51U);
    }

    colors[index].red = red;
    colors[index].green = green;
    colors[index].blue = blue;
    cregs[(index * 3U) + 0U] = zz9k_picture_alpha_lut8_creg(red);
    cregs[(index * 3U) + 1U] = zz9k_picture_alpha_lut8_creg(green);
    cregs[(index * 3U) + 2U] = zz9k_picture_alpha_lut8_creg(blue);
  }

  zz9k_picture_trace("metadata: png alpha lut8 palette ready");
  return 1;
}

static int zz9k_picture_prepare_png_transparent_bitmap(
    Object *object,
    ZZ9KPictureInstance *instance,
    struct BitMap **bitmap_out)
{
  struct BitMapHeader *header;
  struct BitMap *bitmap;

  if (!object || !instance || !bitmap_out || instance->width == 0U ||
      instance->height == 0U) {
    return 0;
  }
  *bitmap_out = 0;

  if (!zz9k_picture_prepare_v43_reference_header(
          object, instance->width, instance->height)) {
    return 0;
  }
  if (!zz9k_picture_prepare_alpha_lut8_palette(object, instance)) {
    return 0;
  }

  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Depth = 8;
  header->bmh_Masking = mskHasTransparentColor;
  header->bmh_Transparent = 0;

  bitmap = AllocBitMap(instance->width, instance->height, 8,
                       BMF_CLEAR, 0);
  if (!bitmap) {
    zz9k_picture_trace("metadata: png bitmap alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  *bitmap_out = bitmap;
  zz9k_picture_trace("metadata: png transparent bitmap ready");
  return 1;
}

static int zz9k_picture_publish_png_transparent_bitmap(
    Object *object,
    const ZZ9KPictureInstance *instance,
    struct BitMap *bitmap)
{
  if (!object || !instance || !bitmap) {
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, instance->width,
      DTA_NominalVert, instance->height,
      DTA_TotalHoriz, instance->width,
      DTA_TotalVert, instance->height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_ModeID, 0,
      PDTA_BitMap, (ULONG)bitmap,
      PDTA_FreeSourceBitMap, TRUE,
      PDTA_Remap, TRUE,
      PDTA_AlphaChannel, FALSE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),
      TAG_END);
  zz9k_picture_trace("metadata: png transparent bitmap published");
  return 1;
}

static uint8_t zz9k_picture_rgba_to_transparent_lut8_index(
    volatile uint8_t *src,
    ULONG pixel_format)
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t alpha;
  uint8_t red_level;
  uint8_t green_level;
  uint8_t blue_level;

  if (!src) {
    return 0U;
  }

  if (pixel_format == PBPAFMT_RGBA) {
    red = src[0];
    green = src[1];
    blue = src[2];
    alpha = src[3];
  } else {
    alpha = src[0];
    red = src[1];
    green = src[2];
    blue = src[3];
  }

  if (alpha < 128U) {
    return 0U;
  }

  red_level = zz9k_picture_alpha_lut8_component(red);
  green_level = zz9k_picture_alpha_lut8_component(green);
  blue_level = zz9k_picture_alpha_lut8_component(blue);
  return (uint8_t)(1U + ((uint32_t)red_level * 36U) +
                   ((uint32_t)green_level * 6U) +
                   (uint32_t)blue_level);
}
#endif

static void zz9k_picture_fill_reference_row(uint8_t *dst,
                                            uint32_t width,
                                            uint32_t row)
{
  uint32_t col;
  uint8_t red;
  uint8_t green;

  if (!dst || width == 0U) {
    return;
  }

  red = 0U;
  green = (uint8_t)(row * 5U);
  for (col = 0U; col < width; col++) {
    if ((((row >> 5) ^ (col >> 5)) & 1U) != 0U) {
      dst[0] = red;
      dst[1] = 255U;
      dst[2] = green;
    } else {
      dst[0] = 32U;
      dst[1] = green;
      dst[2] = (uint8_t)(255U - red);
    }
    dst += ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    red = (uint8_t)(red + 3U);
  }
}

static void zz9k_picture_fill_reference_pattern(uint8_t *pixel_data,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t pitch)
{
  uint32_t row;
  uint8_t *row_ptr;

  if (!pixel_data || width == 0U || height == 0U || pitch == 0U) {
    return;
  }

  row_ptr = pixel_data;
  for (row = 0U; row < height; row++) {
    zz9k_picture_fill_reference_row(row_ptr, width, row);
    row_ptr += pitch;
  }
}

static void zz9k_picture_fill_alpha_reference_row(uint8_t *dst,
                                                  uint32_t width,
                                                  uint32_t row)
{
  uint32_t col;
  uint8_t red;
  uint8_t green;
  uint8_t alpha;

  if (!dst || width == 0U) {
    return;
  }

  red = 0U;
  green = (uint8_t)(row * 5U);
  for (col = 0U; col < width; col++) {
    if ((((row >> 5) ^ (col >> 5)) & 1U) != 0U) {
      dst[0] = red;
      dst[1] = 255U;
      dst[2] = green;
    } else {
      dst[0] = 32U;
      dst[1] = green;
      dst[2] = (uint8_t)(255U - red);
    }

    if (row < 32U && col < 32U) {
      alpha = 0U;
    } else if ((((row >> 4) ^ (col >> 4)) & 1U) != 0U) {
      alpha = 96U;
    } else {
      alpha = 255U;
    }
    dst[3] = alpha;
    dst += ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL;
    red = (uint8_t)(red + 3U);
  }
}

static void zz9k_picture_fill_alpha_reference_pattern(uint8_t *pixel_data,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t pitch)
{
  uint32_t row;
  uint8_t *row_ptr;

  if (!pixel_data || width == 0U || height == 0U || pitch == 0U) {
    return;
  }

  row_ptr = pixel_data;
  for (row = 0U; row < height; row++) {
    zz9k_picture_fill_alpha_reference_row(row_ptr, width, row);
    row_ptr += pitch;
  }
}

static int zz9k_picture_write_v43_rgb_buffer(Class *cl,
                                             Object *object,
                                             uint8_t *pixel_data,
                                             uint32_t left,
                                             uint32_t top,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t row_bytes)
{
  struct pdtBlitPixelArray pixels;

  if (!cl || !object || !pixel_data || width == 0U || height == 0U ||
      row_bytes == 0U) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = pixel_data;
  pixels.pbpa_PixelFormat = PBPAFMT_RGB;
  pixels.pbpa_PixelArrayMod = row_bytes;
  pixels.pbpa_Left = left;
  pixels.pbpa_Top = top;
  pixels.pbpa_Width = width;
  pixels.pbpa_Height = height;
  if (DoSuperMethodA(cl, object, (Msg)&pixels) == 0UL) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  return 1;
}

static int zz9k_picture_write_v43_rgba_buffer(Class *cl,
                                              Object *object,
                                              uint8_t *pixel_data,
                                              uint32_t left,
                                              uint32_t top,
                                              uint32_t width,
                                              uint32_t height,
                                              uint32_t row_bytes)
{
  struct pdtBlitPixelArray pixels;

  if (!cl || !object || !pixel_data || width == 0U || height == 0U ||
      row_bytes == 0U) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = pixel_data;
  pixels.pbpa_PixelFormat = PBPAFMT_RGBA;
  pixels.pbpa_PixelArrayMod = row_bytes;
  pixels.pbpa_Left = left;
  pixels.pbpa_Top = top;
  pixels.pbpa_Width = width;
  pixels.pbpa_Height = height;
  if (DoSuperMethodA(cl, object, (Msg)&pixels) == 0UL) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  return 1;
}

static int zz9k_picture_write_v43_reference_pattern(
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  uint8_t *pixel_data;
  uint32_t row_bytes;
  uint32_t pixel_bytes;

  if (!cl || !object || !instance || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  if (!zz9k_picture_min_row_bytes(
          width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL, &row_bytes) ||
      !zz9k_picture_accumulate_surface_bytes(
          height, row_bytes, &pixel_bytes) ||
      pixel_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    zz9k_picture_trace("metadata: reference pixel size invalid");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 reference before pixel alloc");
  pixel_data = (uint8_t *)AllocMem((ULONG)pixel_bytes, MEMF_PUBLIC);
  if (!pixel_data) {
    zz9k_picture_trace("metadata: reference pixel alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_fill_reference_pattern(pixel_data, width, height, row_bytes);
  zz9k_picture_trace("metadata: v43 reference full buffer ready");
  if (!zz9k_picture_write_v43_rgb_buffer(
          cl, object, pixel_data, 0U, 0U, width, height, row_bytes)) {
    zz9k_picture_trace("metadata: reference pixel write failed");
    FreeMem(pixel_data, (ULONG)pixel_bytes);
    return 0;
  }
  zz9k_picture_trace("metadata: v43 reference pixel write done");

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  instance->reference_pixels = pixel_data;
  instance->reference_pixel_bytes = pixel_bytes;
  instance->source_ready = 0;
  zz9k_picture_trace("metadata: v43 reference final attrs ready");
  return 1;
}

static int zz9k_picture_set_v43_reference_pattern(Class *cl,
                                                  Object *object,
                                                  ZZ9KPictureInstance *instance,
                                                  ZZ9KPictureCodec codec,
                                                  uint32_t width,
                                                  uint32_t height)
{
  zz9k_picture_trace("metadata: reference pattern begin");
  if (!zz9k_picture_prepare_v43_reference_header(object, width, height)) {
    return 0;
  }
  return zz9k_picture_write_v43_reference_pattern(
      cl, object, instance, codec, width, height);
}

static int zz9k_picture_set_reference_v43_attrs(Object *object,
                                                ZZ9KPictureCodec codec,
                                                uint32_t width,
                                                uint32_t height)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 reference before attrs");
  (void)SetDTAttrs(
      object, 0, 0,
      DTA_ErrorNumber, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      PDTA_ModeID, 0,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, FALSE,
      PDTA_Remap, TRUE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: v43 reference attrs ready");
  return 1;
}

static int zz9k_picture_set_alpha_reference_v43_attrs(
    Object *object,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  if (!object || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 alpha reference before attrs");
  (void)SetDTAttrs(
      object, 0, 0,
      DTA_ErrorNumber, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      PDTA_ModeID, 0,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, FALSE,
      PDTA_Remap, FALSE,
      PDTA_AlphaChannel, TRUE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  zz9k_picture_trace("metadata: v43 alpha reference attrs ready");
  return 1;
}

static int zz9k_picture_set_v43_attrs_reference_pattern(
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  zz9k_picture_trace("metadata: reference pattern begin");
  if (!zz9k_picture_prepare_v43_reference_header(object, width, height)) {
    return 0;
  }
  if (!zz9k_picture_set_reference_v43_attrs(
          object, codec, width, height)) {
    return 0;
  }
  return zz9k_picture_write_v43_reference_pattern(
      cl, object, instance, codec, width, height);
}

static int zz9k_picture_write_v43_alpha_reference_pattern(
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  uint8_t *pixel_data;
  uint32_t row_bytes;
  uint32_t pixel_bytes;

  if (!cl || !object || !instance || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  if (!zz9k_picture_min_row_bytes(
          width, ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL, &row_bytes) ||
      !zz9k_picture_accumulate_surface_bytes(
          height, row_bytes, &pixel_bytes) ||
      pixel_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    zz9k_picture_trace("metadata: reference pixel size invalid");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_trace("metadata: v43 alpha reference before pixel alloc");
  pixel_data = (uint8_t *)AllocMem((ULONG)pixel_bytes, MEMF_PUBLIC);
  if (!pixel_data) {
    zz9k_picture_trace("metadata: reference pixel alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_fill_alpha_reference_pattern(
      pixel_data, width, height, row_bytes);
  zz9k_picture_trace("metadata: v43 alpha reference full buffer ready");
  if (!zz9k_picture_write_v43_rgba_buffer(
          cl, object, pixel_data, 0U, 0U, width, height, row_bytes)) {
    zz9k_picture_trace("metadata: reference pixel write failed");
    FreeMem(pixel_data, (ULONG)pixel_bytes);
    return 0;
  }
  zz9k_picture_trace("metadata: v43 alpha reference pixel write done");

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  instance->reference_pixels = pixel_data;
  instance->reference_pixel_bytes = pixel_bytes;
  instance->source_ready = 0;
  zz9k_picture_trace("metadata: v43 alpha reference final attrs ready");
  return 1;
}

static int zz9k_picture_set_v43_alpha_reference_pattern(
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    ZZ9KPictureCodec codec,
    uint32_t width,
    uint32_t height)
{
  zz9k_picture_trace("metadata: reference pattern begin");
  if (!zz9k_picture_prepare_v43_alpha_reference_header(
          object, width, height)) {
    return 0;
  }
  if (!zz9k_picture_set_alpha_reference_v43_attrs(
          object, codec, width, height)) {
    return 0;
  }
  return zz9k_picture_write_v43_alpha_reference_pattern(
      cl, object, instance, codec, width, height);
}

static int zz9k_picture_prepare_datatype_v43(Object *object,
                                             ZZ9KPictureInstance *instance)
{
  if (!object || !instance || instance->width == 0U ||
      instance->height == 0U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: datatype v43 prepare begin");
  if (!zz9k_picture_prepare_v43_reference_header(
          object, instance->width, instance->height)) {
    return 0;
  }
  if (!zz9k_picture_set_reference_v43_attrs(
          object, instance->codec, instance->width, instance->height)) {
    return 0;
  }
  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG &&
      !zz9k_picture_mark_alpha_header(object)) {
    return 0;
  }
  zz9k_picture_trace("metadata: datatype v43 prepare ready");
  return 1;
}

/* The LUT8 path is only lossless when the palette survives the truecolour
 * round trip: a fully transparent entry sharing its RGB triple with an
 * opaque entry would render its pixels visibly after reverse mapping, so
 * such files stay on the per-pixel alpha path. */
static int zz9k_picture_png_palette_lut8_eligible(
    const ZZ9KPicturePngPalette *palette)
{
  uint32_t transparent_index;
  uint32_t opaque_index;

  if (!palette || !palette->present || palette->partial_alpha ||
      palette->count == 0U ||
      palette->count > ZZ9K_PICTURE_PNG_PALETTE_MAX_ENTRIES) {
    return 0;
  }
  for (transparent_index = 0U;
       transparent_index < (uint32_t)palette->count;
       transparent_index++) {
    if (((palette->transparent_mask[transparent_index >> 3] >>
          (transparent_index & 7U)) & 1U) == 0U) {
      continue;
    }
    for (opaque_index = 0U;
         opaque_index < (uint32_t)palette->count;
         opaque_index++) {
      if (((palette->transparent_mask[opaque_index >> 3] >>
            (opaque_index & 7U)) & 1U) != 0U) {
        continue;
      }
      if (memcmp(&palette->rgb[transparent_index * 3U],
                 &palette->rgb[opaque_index * 3U], 3U) == 0) {
        return 0;
      }
    }
  }
  return 1;
}

/* PNGdt-compatible contract for indexed PNGs: publish a depth-8 picture
 * carrying the file's own palette plus (when tRNS marks a fully transparent
 * entry) a transparent colour instead of per-pixel alpha. MUI's picture
 * pipeline converts such objects through its fast palette path instead of
 * software-compositing a 32-bit alpha surface (GitHub issue #73). */
static int zz9k_picture_prepare_png_palette_lut8_v43(
    Object *object,
    const ZZ9KPictureInstance *instance)
{
  const ZZ9KPicturePngPalette *palette;
  struct BitMapHeader *header;
  struct ColorRegister *colors;
  ULONG *cregs;
  uint32_t index;

  if (!object || !instance || instance->width == 0U ||
      instance->height == 0U ||
      instance->width > 65535U || instance->height > 65535U) {
    zz9k_picture_trace("metadata: png lut8 invalid palette");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  palette = &instance->png_palette;
  if (!zz9k_picture_png_palette_lut8_eligible(palette)) {
    zz9k_picture_trace("metadata: png lut8 invalid palette");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: png lut8 before header");
  header = 0;
  if (GetDTAttrs(object, PDTA_BitMapHeader, (ULONG)&header, TAG_END) < 1 ||
      !header) {
    zz9k_picture_trace("metadata: png lut8 header unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }

  header->bmh_Width = (UWORD)instance->width;
  header->bmh_Height = (UWORD)instance->height;
  header->bmh_Depth = 8;
  if (palette->transparent_index != ZZ9K_PICTURE_PNG_TRANSPARENT_NONE) {
    header->bmh_Masking = mskHasTransparentColor;
    header->bmh_Transparent = (UWORD)palette->transparent_index;
  } else {
    header->bmh_Masking = mskNone;
    header->bmh_Transparent = 0;
  }
  header->bmh_Compression = cmpNone;
  header->bmh_Pad = 0;
  header->bmh_XAspect = 1;
  header->bmh_YAspect = 1;
  header->bmh_PageWidth = (WORD)instance->width;
  header->bmh_PageHeight = (WORD)instance->height;

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_ErrorNumber, 0,
      DTA_NominalHoriz, instance->width,
      DTA_NominalVert, instance->height,
      PDTA_ModeID, 0,
      PDTA_SourceMode, PMODE_V43,
      PDTA_DestMode, PMODE_V43,
      PDTA_SubClassRendersAll, FALSE,
      PDTA_Remap, TRUE,
      PDTA_AlphaChannel, FALSE,
      PDTA_NumColors, (ULONG)palette->count,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),
      TAG_END);

  colors = 0;
  cregs = 0;
  (void)GetDTAttrs(
      object,
      PDTA_ColorRegisters, (ULONG)&colors,
      PDTA_CRegs, (ULONG)&cregs,
      TAG_END);
  if (!colors || !cregs) {
    zz9k_picture_trace("metadata: png lut8 palette unavailable");
    SetIoErr(ERROR_OBJECT_NOT_FOUND);
    return 0;
  }
  for (index = 0U; index < (uint32_t)palette->count; index++) {
    uint32_t red;
    uint32_t green;
    uint32_t blue;

    red = (uint32_t)palette->rgb[(index * 3U) + 0U];
    green = (uint32_t)palette->rgb[(index * 3U) + 1U];
    blue = (uint32_t)palette->rgb[(index * 3U) + 2U];
    colors[index].red = (UBYTE)red;
    colors[index].green = (UBYTE)green;
    colors[index].blue = (UBYTE)blue;
    cregs[(index * 3U) + 0U] = red * 0x01010101UL;
    cregs[(index * 3U) + 1U] = green * 0x01010101UL;
    cregs[(index * 3U) + 2U] = blue * 0x01010101UL;
  }

  zz9k_picture_trace("metadata: png lut8 ready");
  return 1;
}

static int zz9k_picture_prepare_png_datatype_v43(
    Object *object,
    const ZZ9KPictureInstance *instance,
    int has_alpha)
{
  if (!object || !instance || instance->width == 0U ||
      instance->height == 0U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  if (zz9k_picture_png_palette_lut8_eligible(&instance->png_palette)) {
    return zz9k_picture_prepare_png_palette_lut8_v43(object, instance);
  }

  zz9k_picture_trace("metadata: png datatype v43 prepare begin");
  if (has_alpha) {
    if (!zz9k_picture_prepare_v43_alpha_reference_header(
            object, instance->width, instance->height)) {
      return 0;
    }
    if (!zz9k_picture_set_alpha_reference_v43_attrs(
            object, instance->codec, instance->width, instance->height)) {
      return 0;
    }
    zz9k_picture_trace("metadata: png datatype v43 prepare ready");
    return 1;
  }
  if (!zz9k_picture_prepare_v43_reference_header(
          object, instance->width, instance->height)) {
    return 0;
  }
  if (!zz9k_picture_set_reference_v43_attrs(
          object, instance->codec, instance->width, instance->height)) {
    return 0;
  }
  if (!zz9k_picture_clear_alpha_header(object)) {
    return 0;
  }
  zz9k_picture_trace("metadata: png datatype v43 prepare ready");
  return 1;
}

static int zz9k_picture_finalize_datatype_v43_attrs(
    Object *object,
    const ZZ9KPictureInstance *instance)
{
  if (!object || !instance || instance->width == 0U ||
      instance->height == 0U) {
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, instance->width,
      DTA_NominalVert, instance->height,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),
      TAG_END);
  zz9k_picture_trace("metadata: datatype final attrs ready");
  zz9k_picture_trace_source("metadata: datatype final attrs ready");
  return 1;
}

static int zz9k_picture_prepare_datatype_v47_direct(
    Object *object,
    ZZ9KPictureInstance *instance,
    struct pdtBlitPixelArray *pixels,
    uint32_t *pixel_bytes)
{
  if (!object || !instance || !pixels || !pixel_bytes) {
    return 0;
  }

  zz9k_picture_trace("metadata: datatype v47 direct prepare begin");
  if (!zz9k_picture_obtain_direct_pixels(
          object, pixels, instance, instance->codec,
          instance->width, instance->height, pixel_bytes)) {
    zz9k_picture_trace("metadata: datatype v47 direct unavailable");
    return 0;
  }
  zz9k_picture_trace("metadata: datatype v47 direct prepare ready");
  return 1;
}

static int zz9k_picture_prepare_datatype_v47_truecolor(
    Object *object,
    ZZ9KPictureInstance *instance,
    struct pdtBlitPixelArray *pixels,
    uint32_t *pixel_bytes)
{
  if (!object || !instance || !pixels || !pixel_bytes) {
    return 0;
  }

  zz9k_picture_trace("metadata: datatype v47 truecolor prepare begin");
  if (!zz9k_picture_obtain_truecolor_direct_pixels(
          object, pixels, instance, instance->codec,
          instance->width, instance->height, pixel_bytes)) {
    zz9k_picture_trace("metadata: datatype v47 truecolor unavailable");
    return 0;
  }
  zz9k_picture_trace("metadata: datatype v47 truecolor prepare ready");
  return 1;
}

static int zz9k_picture_jpeg_v47_rgb_buffer_valid(
    const struct pdtBlitPixelArray *pixels,
    uint32_t width,
    uint32_t height)
{
  uint32_t min_row_bytes;

  if (!pixels || !pixels->pbpa_PixelData ||
      pixels->pbpa_PixelFormat != PBPAFMT_RGB ||
      pixels->pbpa_PixelArrayMod == 0UL) {
    zz9k_picture_trace("metadata: jpeg v47 rgb buffer invalid");
    return 0;
  }
  if ((pixels->pbpa_Width != 0UL && pixels->pbpa_Width < width) ||
      (pixels->pbpa_Height != 0UL && pixels->pbpa_Height < height)) {
    zz9k_picture_trace("metadata: jpeg v47 rgb dimensions invalid");
    return 0;
  }
  if (!zz9k_picture_min_row_bytes(
          width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL, &min_row_bytes) ||
      pixels->pbpa_PixelArrayMod < min_row_bytes) {
    zz9k_picture_trace("metadata: jpeg v47 rgb stride invalid");
    return 0;
  }

  zz9k_picture_trace("metadata: jpeg v47 rgb buffer accepted");
  return 1;
}

static int zz9k_picture_prepare_jpeg_datatype_v47_rgb_direct(
    Object *object,
    ZZ9KPictureInstance *instance,
    struct pdtBlitPixelArray *pixels)
{
  ULONG set_result;
  ULONG error;

  if (!object || !instance || !pixels ||
      instance->codec != ZZ9K_PICTURE_CODEC_JPEG ||
      instance->width == 0U || instance->height == 0U) {
    return 0;
  }

  zz9k_picture_trace("metadata: jpeg v47 rgb direct prepare begin");
  if (!zz9k_picture_prepare_v43_reference_header(
          object, instance->width, instance->height)) {
    return 0;
  }

  error = 0UL;
  zz9k_picture_init_pixel_request(
      pixels, instance->width, instance->height, PBPAFMT_RGB);
  pixels->pbpa_PixelData = 0;
  set_result = SetDTAttrs(
      object, 0, 0,
      DTA_ErrorNumber, (ULONG)&error,
      PDTA_SourceMode, PMODE_V43,
      PDTA_ModeID, 0,
      DTA_NominalHoriz, instance->width,
      DTA_NominalVert, instance->height,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(instance->codec),
      PDTA_ObtainPixelBuffer, (ULONG)pixels,
      PDTA_SubClassRendersAll, TRUE,
      TAG_END);
  zz9k_picture_trace_u32(
      "metadata: jpeg v47 rgb setattrs result", (uint32_t)set_result);
  zz9k_picture_trace_u32(
      "metadata: jpeg v47 rgb setattrs error", (uint32_t)error);
  zz9k_picture_trace_pixel_buffer(pixels);
  if (error != 0UL ||
      !zz9k_picture_jpeg_v47_rgb_buffer_valid(
          pixels, instance->width, instance->height)) {
    zz9k_picture_trace("metadata: jpeg v47 rgb direct unavailable");
    return 0;
  }

  zz9k_picture_trace("metadata: jpeg v47 rgb direct prepare ready");
  return 1;
}

static int zz9k_picture_set_reference_pattern(Object *object,
                                              ZZ9KPictureInstance *instance,
                                              ZZ9KPictureCodec codec,
                                              uint32_t width,
                                              uint32_t height)
{
  struct pdtBlitPixelArray pixels;
  uint8_t *pixel_data;
  uint32_t row_bytes;
  uint32_t pixel_bytes;
  ULONG method_result;

  if (!object || !instance || width == 0U || height == 0U ||
      width > 65535U || height > 65535U) {
    zz9k_picture_trace("metadata: reference invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  zz9k_picture_trace("metadata: reference pattern begin");
  zz9k_picture_trace("metadata: reference before attrs");
  if (!zz9k_picture_set_reference_attrs(object, codec, width, height)) {
    return 0;
  }
  zz9k_picture_trace("metadata: reference header skipped");

  if (!zz9k_picture_min_row_bytes(
          width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL, &row_bytes) ||
      !zz9k_picture_accumulate_surface_bytes(
          height, row_bytes, &pixel_bytes) ||
      pixel_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    zz9k_picture_trace("metadata: reference pixel size invalid");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_trace("metadata: reference before pixel alloc");
  pixel_data = (uint8_t *)AllocMem((ULONG)pixel_bytes, MEMF_PUBLIC);
  if (!pixel_data) {
    zz9k_picture_trace("metadata: reference pixel alloc failed");
    SetIoErr(ERROR_NO_FREE_STORE);
    return 0;
  }

  zz9k_picture_fill_reference_pattern(pixel_data, width, height, row_bytes);
  zz9k_picture_trace("metadata: reference pixels ready");
  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = pixel_data;
  pixels.pbpa_PixelFormat = PBPAFMT_RGB;
  pixels.pbpa_PixelArrayMod = row_bytes;
  pixels.pbpa_Left = 0;
  pixels.pbpa_Top = 0;
  pixels.pbpa_Width = width;
  pixels.pbpa_Height = height;

  zz9k_picture_trace("metadata: reference before pixel write");
  method_result = DoMethodA(object, (Msg)&pixels);
  if (method_result == 0UL) {
    zz9k_picture_trace("metadata: reference pixel write failed");
    FreeMem(pixel_data, (ULONG)pixel_bytes);
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  instance->reference_pixels = pixel_data;
  instance->reference_pixel_bytes = pixel_bytes;
  zz9k_picture_trace("metadata: reference pixels written");
  zz9k_picture_trace("metadata: reference pixels retained");
  return 1;
}

static int zz9k_picture_enable_hardware_render(Object *object,
                                               ZZ9KPictureCodec codec,
                                               uint32_t width,
                                               uint32_t height)
{
  if (!object || width == 0U || height == 0U) {
    return 0;
  }

  (void)SetDTAttrs(
      object, 0, 0,
      DTA_NominalHoriz, width,
      DTA_NominalVert, height,
      DTA_TotalHoriz, width,
      DTA_TotalVert, height,
      DTA_HorizUnit, 1,
      DTA_VertUnit, 1,
      PDTA_SourceMode, PMODE_V43,
      PDTA_SubClassRendersAll, TRUE,
      DTA_ObjName, (ULONG)zz9k_picture_object_name(codec),
      TAG_END);
  return 1;
}

static int zz9k_picture_set_object_dimensions(Object *object,
                                              ZZ9KPictureCodec codec,
                                              uint32_t width,
                                              uint32_t height)
{
  ZZ9KPictureRenderMode render_mode;

  if (!object || width == 0U || height == 0U) {
    zz9k_picture_trace("metadata: invalid dimensions");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }

  render_mode = zz9k_picture_render_mode();
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_OFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_SMALLOFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL) {
    if (width > ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE) {
      width = ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE;
    }
    if (height > ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE) {
      height = ZZ9K_PICTURE_SMALL_PLACEHOLDER_SIZE;
    }
    zz9k_picture_trace(
        render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL ?
        "metadata: v43 small placeholder active" :
        render_mode == ZZ9K_PICTURE_RENDER_MODE_OFF ?
        "metadata: off placeholder capped" :
        "metadata: small placeholder active");
  }

  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL) {
    return zz9k_picture_set_placeholder_pixels(object, codec, width, height);
  }

  return zz9k_picture_set_placeholder_best(object, codec, width, height);
}

static uint32_t zz9k_picture_image_codec(ZZ9KPictureCodec codec)
{
  switch (codec) {
  case ZZ9K_PICTURE_CODEC_JPEG:
    return ZZ9K_IMAGE_CODEC_JPEG;
  case ZZ9K_PICTURE_CODEC_PNG:
    return ZZ9K_IMAGE_CODEC_PNG;
  case ZZ9K_PICTURE_CODEC_WEBP:
    return ZZ9K_IMAGE_CODEC_WEBP;
  default:
    return 0U;
  }
}

static int zz9k_picture_open_cached_context(ZZ9KContext **ctx)
{
  ZZ9KContext *opened;
  int status;

  if (!ctx) {
    return ZZ9K_STATUS_BAD_REQUEST;
  }

  *ctx = 0;
  if (zz9k_picture_cached_ctx) {
    *ctx = zz9k_picture_cached_ctx;
    return ZZ9K_STATUS_OK;
  }

  opened = 0;
  status = zz9k_open(&opened);
  if (status != ZZ9K_STATUS_OK) {
    return status;
  }
  if (!opened) {
    return ZZ9K_STATUS_INTERNAL_ERROR;
  }

  zz9k_picture_cached_ctx = opened;
  *ctx = opened;
  return ZZ9K_STATUS_OK;
}

static void zz9k_picture_close_cached_context(void)
{
  if (zz9k_picture_cached_ctx) {
    zz9k_close(zz9k_picture_cached_ctx);
    zz9k_picture_cached_ctx = 0;
  }
  memset(&zz9k_picture_cached_caps, 0, sizeof(zz9k_picture_cached_caps));
  memset(&zz9k_picture_cached_image_service, 0,
         sizeof(zz9k_picture_cached_image_service));
  zz9k_picture_cached_caps_ready = 0U;
  zz9k_picture_cached_image_service_ready = 0U;
}

static int zz9k_picture_decode_lock_obtain(void)
{
  if (!zz9k_picture_decode_semaphore_ready) {
    return 0;
  }

  zz9k_picture_trace_source("decode: serialize obtain");
  ObtainSemaphore(&zz9k_picture_decode_semaphore);
  return 1;
}

static void zz9k_picture_decode_lock_release(void)
{
  if (!zz9k_picture_decode_semaphore_ready) {
    return;
  }

  ReleaseSemaphore(&zz9k_picture_decode_semaphore);
  zz9k_picture_trace_source("decode: serialize release");
}

static int zz9k_picture_query_cached_caps(ZZ9KContext *ctx,
                                          ZZ9KCaps *caps)
{
  if (!ctx || !caps) {
    return 0;
  }

  if (zz9k_picture_cached_caps_ready) {
    *caps = zz9k_picture_cached_caps;
    return 1;
  }

  memset(caps, 0, sizeof(*caps));
  if (zz9k_query_caps(ctx, caps) != ZZ9K_STATUS_OK) {
    return 0;
  }

  zz9k_picture_cached_caps = *caps;
  zz9k_picture_cached_caps_ready = 1U;
  return 1;
}

static int zz9k_picture_query_cached_image_service(
    ZZ9KContext *ctx,
    ZZ9KServiceInfo *service)
{
  if (!ctx || !service) {
    return 0;
  }

  if (zz9k_picture_cached_image_service_ready) {
    *service = zz9k_picture_cached_image_service;
    return 1;
  }

  memset(service, 0, sizeof(*service));
  if (zz9k_query_service(ctx, ZZ9K_SERVICE_IMAGE, service) !=
      ZZ9K_STATUS_OK) {
    return 0;
  }

  zz9k_picture_cached_image_service = *service;
  zz9k_picture_cached_image_service_ready = 1U;
  return 1;
}

static int zz9k_picture_require_caps(ZZ9KContext *ctx)
{
  ZZ9KCaps caps;
  uint32_t required;

  required = ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_SURFACES |
             ZZ9K_CAP_IMAGE_DECODE | ZZ9K_CAP_IMAGE_SCALE |
             ZZ9K_CAP_SERVICE_DISCOVERY;
  return zz9k_picture_query_cached_caps(ctx, &caps) &&
         zz9k_has_capabilities(caps.capability_bits, required);
}

static int zz9k_picture_require_datatype_caps(ZZ9KContext *ctx)
{
  ZZ9KCaps caps;
  uint32_t required;

  required = ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_IMAGE_DECODE |
             ZZ9K_CAP_SERVICE_DISCOVERY;
  return zz9k_picture_query_cached_caps(ctx, &caps) &&
         zz9k_has_capabilities(caps.capability_bits, required);
}

static int zz9k_picture_require_stream_service(
    ZZ9KContext *ctx,
    ZZ9KPictureCodec codec,
    uint32_t output_mode,
    ZZ9KServiceInfo *image_service)
{
  ZZ9KServiceInfo service;
  uint32_t required_flags;
  uint32_t image_codec;

  image_codec = zz9k_picture_image_codec(codec);
  if (!ctx || image_codec == 0U ||
      !zz9k_image_stream_required_service_flags(
          image_codec, output_mode, &required_flags)) {
    return 0;
  }

  if (!zz9k_picture_query_cached_image_service(ctx, &service)) {
    return 0;
  }
  if (!zz9k_has_service_flags(service.flags, required_flags)) {
    return 0;
  }
  if (image_service) {
    *image_service = service;
  }
  if (output_mode == ZZ9K_IMAGE_OUTPUT_TILE_BUFFER) {
    return 1;
  }
  return zz9k_image_service_supports_clipped_scale(
      service.opcode_count, service.flags, ZZ9K_SCALE_BILINEAR);
}

static int zz9k_picture_direct_pixels_prefer_rgb888_tiles(
    const ZZ9KServiceInfo *image_service,
    const struct pdtBlitPixelArray *direct_pixels)
{
  return image_service &&
         direct_pixels &&
         direct_pixels->pbpa_PixelFormat == PBPAFMT_RGB &&
         zz9k_has_service_flag(image_service->flags,
                               ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT);
}

static int zz9k_picture_choose_datatype_tile_format(
    const ZZ9KServiceInfo *image_service,
    const struct pdtBlitPixelArray *direct_pixels,
    uint32_t *output_format,
    uint32_t *output_bpp)
{
  if (!output_format || !output_bpp) {
    return 0;
  }

  if (zz9k_picture_direct_pixels_prefer_rgb888_tiles(
          image_service, direct_pixels)) {
    *output_format = ZZ9K_SURFACE_FORMAT_RGB888;
    *output_bpp = ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    zz9k_picture_trace("metadata: datatype direct rgb888 tiles");
    return 1;
  }

  if (!direct_pixels && image_service &&
      zz9k_has_service_flag(image_service->flags,
                            ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT)) {
    *output_format = ZZ9K_SURFACE_FORMAT_RGB888;
    *output_bpp = ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    zz9k_picture_trace("metadata: datatype rgb888 tiles");
    return 1;
  }

  *output_format = ZZ9K_SURFACE_FORMAT_BGRA8888;
  *output_bpp = ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
  zz9k_picture_trace("metadata: datatype bgra fallback tiles");
  return 1;
}

static int zz9k_picture_choose_png_datatype_tile_format(
    const ZZ9KServiceInfo *image_service,
    const struct pdtBlitPixelArray *direct_pixels,
    int has_alpha,
    uint32_t *output_format,
    uint32_t *output_bpp)
{
  if (!output_format || !output_bpp) {
    return 0;
  }
  if (has_alpha) {
    *output_format = ZZ9K_SURFACE_FORMAT_RGBA8888;
    *output_bpp = ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL;
    zz9k_picture_trace("metadata: datatype png rgba alpha tiles");
    return 1;
  }
  return zz9k_picture_choose_datatype_tile_format(
      image_service, direct_pixels, output_format, output_bpp);
}

static int zz9k_picture_require_framebuffer(
    ZZ9KContext *ctx,
    ZZ9KPictureInstance *instance)
{
  ZZ9KSurface framebuffer;

  if (!ctx || !instance) {
    return 0;
  }

  memset(&framebuffer, 0, sizeof(framebuffer));
  zz9k_picture_trace("decode: before framebuffer map");
  if (zz9k_map_framebuffer_surface(ctx, &framebuffer) !=
      ZZ9K_STATUS_OK) {
    return 0;
  }
  if ((framebuffer.flags & ZZ9K_SURFACE_FLAG_FRAMEBUFFER) == 0U ||
      !zz9k_surface_is_native_rtg_format(framebuffer.format) ||
      framebuffer.width == 0U || framebuffer.height == 0U) {
    return 0;
  }

  instance->framebuffer_width = framebuffer.width;
  instance->framebuffer_height = framebuffer.height;
  return 1;
}

static int zz9k_picture_accumulate_surface_bytes(uint32_t height,
                                                 uint32_t pitch,
                                                 uint32_t *length);
static int zz9k_picture_shared_copy_to_bytes(ZZ9KSharedBuffer *buffer,
                                             uint32_t offset,
                                             const uint8_t *src,
                                             uint32_t length);

static int zz9k_picture_surface_layout(uint32_t width,
                                       uint32_t height,
                                       uint32_t format,
                                       uint32_t *pitch,
                                       uint32_t *length)
{
  uint32_t bpp;

  if (!pitch || !length || width == 0U || height == 0U) {
    return 0;
  }

  switch (format) {
  case ZZ9K_SURFACE_FORMAT_ARGB8888:
  case ZZ9K_SURFACE_FORMAT_RGBA8888:
  case ZZ9K_SURFACE_FORMAT_BGRA8888:
    bpp = 4U;
    break;
  case ZZ9K_SURFACE_FORMAT_RGB888:
    bpp = 3U;
    break;
  case ZZ9K_SURFACE_FORMAT_RGB565:
  case ZZ9K_SURFACE_FORMAT_RGB555:
    bpp = 2U;
    break;
  case ZZ9K_SURFACE_FORMAT_INDEX8:
    bpp = 1U;
    break;
  default:
    return 0;
  }

  if (!zz9k_picture_min_row_bytes(width, bpp, pitch)) {
    zz9k_picture_trace("decode: surface pitch overflow");
    return 0;
  }
  zz9k_picture_trace("decode: surface pitch ready");

  if (!zz9k_picture_accumulate_surface_bytes(height, *pitch, length)) {
    return 0;
  }
  zz9k_picture_trace("decode: surface bytes ready");
  return 1;
}

static int zz9k_picture_accumulate_surface_bytes(uint32_t height,
                                                 uint32_t pitch,
                                                 uint32_t *length)
{
  uint32_t row;
  uint32_t total;

  if (!length || height == 0U || pitch == 0U) {
    return 0;
  }
  if (pitch > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    *length = ZZ9K_PICTURE_MAX_SURFACE_BYTES + 1U;
    return 1;
  }

  total = 0U;
  for (row = 0U; row < height; row++) {
    if (total > (0xffffffffUL - pitch)) {
      zz9k_picture_trace("decode: surface size overflow");
      return 0;
    }
    if (total > (ZZ9K_PICTURE_MAX_SURFACE_BYTES - pitch)) {
      *length = ZZ9K_PICTURE_MAX_SURFACE_BYTES + 1U;
      return 1;
    }
    total += pitch;
  }

  *length = total;
  return 1;
}

static int zz9k_picture_shared_copy_to_bytes(ZZ9KSharedBuffer *buffer,
                                             uint32_t offset,
                                             const uint8_t *src,
                                             uint32_t length)
{
  volatile uint8_t *dst;
  uint32_t i;

  if (!buffer || buffer->handle == ZZ9K_INVALID_HANDLE ||
      !buffer->data || offset > buffer->length ||
      length > (buffer->length - offset) ||
      (length != 0U && !src)) {
    return 0;
  }

  dst = (volatile uint8_t *)buffer->data + offset;
  for (i = 0U; i < length; i++) {
    dst[i] = src[i];
  }

  return 1;
}

static void zz9k_picture_init_stream_input(ZZ9KPictureStreamInput *input)
{
  if (!input) {
    return;
  }
  memset(input, 0, sizeof(*input));
  input->staging.handle = ZZ9K_INVALID_HANDLE;
}

static uint32_t zz9k_picture_stream_input_bytes(ZZ9KPictureSource *source)
{
  uint32_t source_size;

  if (zz9k_picture_file_size(source, &source_size) &&
      source_size < ZZ9K_PICTURE_STAGING_BYTES) {
    return source_size + 1U;
  }

  return ZZ9K_PICTURE_STAGING_BYTES;
}

static void zz9k_picture_free_stream_input(ZZ9KContext *ctx,
                                           ZZ9KPictureStreamInput *input)
{
  if (!input) {
    return;
  }
  if (ctx && input->staging.handle != ZZ9K_INVALID_HANDLE) {
    (void)zz9k_free_shared(ctx, input->staging.handle);
  }
  input->staging.handle = ZZ9K_INVALID_HANDLE;
  input->staging.data = 0;
  input->staging.length = 0U;
  if (input->read_scratch) {
    FreeMem(input->read_scratch, (ULONG)input->read_scratch_bytes);
  }
  input->read_scratch = 0;
  input->read_scratch_bytes = 0U;
}

static int zz9k_picture_alloc_stream_input(ZZ9KContext *ctx,
                                           ZZ9KPictureSource *source,
                                           ZZ9KPictureStreamInput *input,
                                           const char **failure_out)
{
  ZZ9KCaps caps;
  uint32_t input_bytes;
  uint32_t allocation_flags;
  int status;

  if (!ctx || !source || !input) {
    return 0;
  }

  zz9k_picture_init_stream_input(input);
  input_bytes = zz9k_picture_stream_input_bytes(source);
  allocation_flags = 0U;
  memset(&caps, 0, sizeof(caps));
  if (zz9k_picture_query_cached_caps(ctx, &caps) &&
      (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U) {
    if (input_bytes > ZZ9K_PICTURE_Z2_STAGING_BYTES) {
      input_bytes = ZZ9K_PICTURE_Z2_STAGING_BYTES;
    }
    allocation_flags = ZZ9K_ALLOC_HOST_WINDOW;
  }
  if (input_bytes == 0U ||
      input_bytes > ZZ9K_PICTURE_STAGING_BYTES) {
    input_bytes = ZZ9K_PICTURE_STAGING_BYTES;
  }

  zz9k_picture_trace_u32("decode: datatype staging bytes", input_bytes);
  zz9k_picture_trace("decode: datatype before staging alloc");
  status = zz9k_alloc_shared(ctx, input_bytes,
                             16U, allocation_flags, &input->staging);
  zz9k_picture_trace_u32(
      "decode: datatype staging alloc status", (uint32_t)status);
  if (status != ZZ9K_STATUS_OK) {
    if (failure_out) {
      *failure_out = "decode: shared staging alloc failed";
    }
    return 0;
  }
  zz9k_picture_trace("decode: datatype staging alloc ok");

  if (source->type != ZZ9K_PICTURE_SOURCE_MEMORY) {
    input->read_scratch = (uint8_t *)AllocMem(
        (ULONG)input_bytes, MEMF_PUBLIC);
    if (!input->read_scratch) {
      if (failure_out) {
        *failure_out = "decode: read scratch alloc failed";
      }
      zz9k_picture_free_stream_input(ctx, input);
      return 0;
    }
    input->read_scratch_bytes = input_bytes;
  }

  return 1;
}

static int zz9k_picture_read_chunk_to_shared(ZZ9KPictureSource *source,
                                             ZZ9KSharedBuffer *staging,
                                             uint8_t *read_scratch,
                                             uint32_t read_scratch_length,
                                             uint32_t offset,
                                             uint32_t capacity,
                                             uint32_t *copied,
                                             int *eof,
                                             uint32_t *trace_chunks)
{
  uint32_t total;

  if (!source || !staging ||
      (source->type != ZZ9K_PICTURE_SOURCE_MEMORY &&
       (!read_scratch || read_scratch_length == 0U)) ||
      !copied || !eof ||
      offset > staging->length || capacity > (staging->length - offset)) {
    return 0;
  }

  total = 0U;
  while (total < capacity && !*eof) {
    uint32_t want;
    int trace_this;
    LONG bytes_read;

    want = capacity - total;
    if (source->type != ZZ9K_PICTURE_SOURCE_MEMORY &&
        want > read_scratch_length) {
      want = read_scratch_length;
    }
    trace_this = ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE &&
                 trace_chunks && *trace_chunks != 0U;
    if (trace_this) {
      zz9k_picture_trace_u32(
          "stream: datatype chunk offset", offset + total);
      zz9k_picture_trace_u32("stream: datatype chunk capacity", want);
      zz9k_picture_trace("stream: datatype before file read");
    }
#if ZZ9K_PICTURE_TRACE_STREAM_CHUNKS
    zz9k_picture_trace("stream: before file read");
#endif
    if (source->type == ZZ9K_PICTURE_SOURCE_MEMORY) {
      uint32_t available;

      if (!source->memory || source->position > source->size) {
        return 0;
      }
      available = source->size - source->position;
      bytes_read = (LONG)(want < available ? want : available);
    } else {
      bytes_read = zz9k_picture_source_read(source, read_scratch, want);
    }
    if (trace_this) {
      zz9k_picture_trace_u32(
          "stream: datatype file read bytes", (uint32_t)bytes_read);
    }
    if (bytes_read < 0) {
      return 0;
    }
    if (bytes_read == 0) {
      if (trace_this) {
        (*trace_chunks)--;
      }
      *eof = 1;
      break;
    }
    if (!staging->data) {
      zz9k_picture_trace("stream: staging pointer unavailable");
      return 0;
    }
#if ZZ9K_PICTURE_TRACE_STREAM_CHUNKS
    zz9k_picture_trace("stream: before shared copy");
    zz9k_picture_trace("stream: before shared byte copy");
#endif
    if (trace_this) {
      zz9k_picture_trace("stream: datatype before shared byte copy");
    }
    if (!zz9k_picture_shared_copy_to_bytes(
            staging, offset + total,
            source->type == ZZ9K_PICTURE_SOURCE_MEMORY
                ? source->memory + source->position
                : read_scratch,
            (uint32_t)bytes_read)) {
      return 0;
    }
    if (source->type == ZZ9K_PICTURE_SOURCE_MEMORY) {
      source->position += (uint32_t)bytes_read;
    }
    if (trace_this) {
      zz9k_picture_trace("stream: datatype shared byte copy ok");
      (*trace_chunks)--;
    }
#if ZZ9K_PICTURE_TRACE_STREAM_CHUNKS
    zz9k_picture_trace("stream: shared byte copy ok");
#endif
    total += (uint32_t)bytes_read;
    if ((uint32_t)bytes_read < want) {
      *eof = 1;
    }
  }

  *copied = total;
  return 1;
}

static int zz9k_picture_fill_staging(ZZ9KPictureSource *source,
                                     ZZ9KSharedBuffer *staging,
                                     uint8_t *read_scratch,
                                     uint32_t read_scratch_length,
                                     uint32_t *buffered,
                                     int *eof,
                                     uint32_t *trace_chunks)
{
  uint32_t copied;

  if (!source || !staging ||
      (source->type != ZZ9K_PICTURE_SOURCE_MEMORY &&
       (!read_scratch || read_scratch_length == 0U)) ||
      !buffered || !eof ||
      *buffered > staging->length) {
    return 0;
  }
  if (*eof || *buffered == staging->length) {
    return 1;
  }
  copied = 0U;
  if (!zz9k_picture_read_chunk_to_shared(
          source, staging, read_scratch, read_scratch_length, *buffered,
          staging->length - *buffered, &copied, eof, trace_chunks)) {
    return 0;
  }
  *buffered += copied;
  return 1;
}

static int zz9k_picture_stream_result_made_progress(
    const ZZ9KImageSessionResult *result)
{
  return result && (result->bytes_consumed != 0U ||
                    result->bytes_written != 0U ||
                    result->state ==
                        ZZ9K_IMAGE_SESSION_STATE_COMPLETE);
}

static int zz9k_picture_no_progress_is_fatal(
    const ZZ9KImageSessionResult *result,
    uint32_t buffered,
    uint32_t capacity,
    int eof)
{
  if (zz9k_picture_stream_result_made_progress(result)) {
    return 0;
  }
  return eof || buffered >= capacity;
}

static int zz9k_picture_feed_stream(ZZ9KContext *ctx,
                                    ZZ9KPictureSource *source,
                                    ZZ9KSharedBuffer *staging,
                                    uint8_t *read_scratch,
                                    uint32_t read_scratch_length,
                                    uint32_t session,
                                    uint32_t drain_limit,
                                    ZZ9KImageSessionResult *final_result)
{
  uint32_t buffered;
  uint32_t empty_eof_feeds;
  int eof;

  if (!ctx || !source || !staging ||
      (source->type != ZZ9K_PICTURE_SOURCE_MEMORY &&
       (!read_scratch || read_scratch_length == 0U)) ||
      session == 0U || !final_result) {
    return 0;
  }

  buffered = 0U;
  empty_eof_feeds = 0U;
  eof = 0;
  memset(final_result, 0, sizeof(*final_result));
  while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
    uint32_t consumed;

    zz9k_picture_trace("stream: before fill staging");
    if (!zz9k_picture_fill_staging(source, staging, read_scratch,
                                   read_scratch_length, &buffered, &eof,
                                   0)) {
      return 0;
    }
    zz9k_picture_trace("stream: fill staging ok");
    if (buffered == 0U && eof) {
      empty_eof_feeds++;
      if (empty_eof_feeds > drain_limit) {
        return 0;
      }
    }

    do {
      ZZ9KImageSessionFeedDesc feed;
      ZZ9KImageSessionResult result;
      int status;

      zz9k_picture_trace("stream: before feed desc");
      if (!zz9k_image_build_session_feed_desc(
              &feed, session, staging->handle, 0U, buffered,
              eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
        return 0;
      }
      zz9k_picture_trace("stream: feed desc ok");
      memset(&result, 0, sizeof(result));
      zz9k_picture_trace("stream: before session feed");
      status = zz9k_image_session_feed(ctx, &feed, &result);
      zz9k_picture_trace("stream: session feed ok");
      if (status != ZZ9K_STATUS_OK || result.bytes_consumed > buffered) {
        return 0;
      }

      consumed = result.bytes_consumed;
      if (consumed != 0U) {
        buffered -= consumed;
        if (buffered != 0U &&
            !zz9k_shared_move(staging, 0U, consumed, buffered)) {
          return 0;
        }
      }

      if (result.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
        *final_result = result;
        break;
      }
      if (result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT &&
          result.state != ZZ9K_IMAGE_SESSION_STATE_HEADER_READY &&
          result.state != ZZ9K_IMAGE_SESSION_STATE_TILE_READY) {
        return 0;
      }
      if (zz9k_picture_no_progress_is_fatal(
              &result, buffered, staging->length, eof)) {
        return 0;
      }
      if (buffered == 0U ||
          (!zz9k_picture_stream_result_made_progress(&result) && !eof)) {
        break;
      }
    } while (1);
  }

  return 1;
}

static int zz9k_picture_choose_tile_layout(uint32_t width,
                                           uint32_t bytes_per_pixel,
                                           uint32_t max_rows,
                                           uint32_t target_bytes,
                                           uint32_t *rows,
                                           uint32_t *pitch,
                                           uint32_t *bytes)
{
  uint32_t candidate_rows;
  uint32_t candidate_pitch;
  uint32_t candidate_bytes;

  if (!rows || !pitch || !bytes || max_rows == 0U ||
      target_bytes == 0U) {
    return 0;
  }

  if (!zz9k_picture_min_row_bytes(
          width, bytes_per_pixel, &candidate_pitch)) {
    return 0;
  }
  candidate_rows = max_rows;
  while (candidate_rows != 0U) {
    candidate_bytes = 0U;
    if (zz9k_picture_accumulate_surface_bytes(
            candidate_rows, candidate_pitch, &candidate_bytes) &&
        candidate_bytes <= target_bytes) {
      *rows = candidate_rows;
      *pitch = candidate_pitch;
      *bytes = candidate_bytes;
      return 1;
    }
    candidate_rows--;
  }
  return 0;
}

static void zz9k_picture_init_datatype_target(
    ZZ9KPictureDatatypeTarget *target,
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    uint32_t output_format,
    uint32_t output_bpp)
{
  if (!target) {
    return;
  }
  memset(target, 0, sizeof(*target));
  target->object = object;
  target->cl = cl;
  target->instance = instance;
  if (instance) {
    target->width = instance->width;
    target->height = instance->height;
  }
  target->output_format = output_format;
  target->output_bpp = output_bpp;
}

static int zz9k_picture_choose_datatype_tile_layout(
    uint32_t width,
    uint32_t output_format,
    uint32_t output_bpp,
    uint32_t *tile_max_rows,
    uint32_t *tile_target_bytes,
    uint32_t *tile_rows,
    uint32_t *tile_pitch,
    uint32_t *tile_bytes)
{
  uint32_t max_rows;
  uint32_t target_bytes;

  if (!tile_max_rows || !tile_target_bytes ||
      !tile_rows || !tile_pitch || !tile_bytes) {
    return 0;
  }

  if (output_format == ZZ9K_SURFACE_FORMAT_RGB888) {
    max_rows = ZZ9K_PICTURE_RGB888_TILE_MAX_ROWS;
    target_bytes = ZZ9K_PICTURE_RGB888_TILE_TARGET_BYTES;
  } else {
    max_rows = ZZ9K_PICTURE_TILE_MAX_ROWS;
    target_bytes = ZZ9K_PICTURE_TILE_TARGET_BYTES;
  }

  *tile_max_rows = max_rows;
  *tile_target_bytes = target_bytes;
  return zz9k_picture_choose_tile_layout(
      width, output_bpp, max_rows, target_bytes,
      tile_rows, tile_pitch, tile_bytes);
}

static int zz9k_picture_choose_png_full_datatype_tile_layout(
    uint32_t width,
    uint32_t height,
    uint32_t output_bpp,
    uint32_t *tile_max_rows,
    uint32_t *tile_target_bytes,
    uint32_t *tile_rows,
    uint32_t *tile_pitch,
    uint32_t *tile_bytes)
{
  uint32_t pitch;
  uint32_t bytes;

  if (!tile_max_rows || !tile_target_bytes ||
      !tile_rows || !tile_pitch || !tile_bytes ||
      width == 0U || height == 0U || output_bpp == 0U) {
    return 0;
  }
  if (!zz9k_picture_min_row_bytes(width, output_bpp, &pitch) ||
      !zz9k_picture_accumulate_surface_bytes(height, pitch, &bytes)) {
    return 0;
  }
  if (bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    zz9k_picture_trace("decode: datatype png full layout too large");
    return 0;
  }

  *tile_max_rows = height;
  *tile_target_bytes = bytes;
  *tile_rows = height;
  *tile_pitch = pitch;
  *tile_bytes = bytes;
  return bytes != 0U;
}

static int zz9k_picture_write_rgb_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  uint32_t min_row_bytes;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl || target->output_format != ZZ9K_SURFACE_FORMAT_RGB888 ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      result->tile_height > target->tile_rows ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL,
          &min_row_bytes) ||
      tile_stride < min_row_bytes) {
    return 0;
  }

  if (!zz9k_picture_write_v43_rgb_buffer(
      target->cl, target->object, (uint8_t *)tile->data,
      result->tile_x, result->tile_y,
      result->tile_width, result->tile_height, tile_stride)) {
    zz9k_picture_trace("decode: datatype writepixelarray failed");
    return 0;
  }

  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype tile written");
  }
  return 1;
}

static void zz9k_picture_bgra_to_rgb(const volatile uint8_t *src,
                                     uint8_t *dst)
{
  if (!src || !dst) {
    return;
  }

  dst[0] = src[2];
  dst[1] = src[1];
  dst[2] = src[0];
}

static void zz9k_picture_bgra_to_rgba(const volatile uint8_t *src,
                                      uint8_t *dst)
{
  if (!src || !dst) {
    return;
  }

  dst[0] = src[2];
  dst[1] = src[1];
  dst[2] = src[0];
  dst[3] = src[3];
}

static uint32_t zz9k_picture_lut8_map_slot(uint32_t key)
{
  return ((key * ZZ9K_PICTURE_LUT8_MAP_HASH_MULTIPLIER) >> 23U) &
         (ZZ9K_PICTURE_LUT8_MAP_SLOTS - 1U);
}

static uint8_t zz9k_picture_png_nearest_palette_index(
    const ZZ9KPicturePngPalette *palette,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
  uint32_t best_index;
  uint32_t best_distance;
  uint32_t index;

  best_index = 0U;
  best_distance = 0xFFFFFFFFUL;
  for (index = 0U; index < (uint32_t)palette->count; index++) {
    uint32_t red_delta;
    uint32_t green_delta;
    uint32_t blue_delta;
    uint32_t distance;

    red_delta = (uint32_t)red - palette->rgb[(index * 3U) + 0U];
    green_delta = (uint32_t)green - palette->rgb[(index * 3U) + 1U];
    blue_delta = (uint32_t)blue - palette->rgb[(index * 3U) + 2U];
    distance = (red_delta * red_delta) +
               (green_delta * green_delta) +
               (blue_delta * blue_delta);
    if (distance < best_distance) {
      best_distance = distance;
      best_index = index;
    }
  }
  return (uint8_t)best_index;
}

static void zz9k_picture_build_png_lut8_map(
    const ZZ9KPicturePngPalette *palette,
    uint32_t *keys,
    uint8_t *vals)
{
  uint32_t pass;
  uint32_t index;

  memset(keys, 0, sizeof(uint32_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS);
  memset(vals, 0, sizeof(uint8_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS);

  /* Two passes so opaque entries claim their colour first; remaining
   * duplicate colours only occur between entries of equal transparency,
   * which map to the shared transparent index anyway (mixed-opacity
   * duplicates are rejected by zz9k_picture_png_palette_lut8_eligible
   * before the decode path is chosen). All fully transparent entries are
   * inserted in the second pass pointing at the published transparent
   * index, so several zero-alpha entries share one
   * mskHasTransparentColor index. */
  for (pass = 0U; pass < 2U; pass++) {
    for (index = 0U; index < (uint32_t)palette->count; index++) {
      uint32_t transparent;
      uint32_t mapped_index;
      uint32_t key;
      uint32_t slot;

      transparent =
          (palette->transparent_mask[index >> 3] >> (index & 7U)) & 1U;
      if (pass == 0U && transparent != 0U) {
        continue;
      }
      if (pass == 1U && transparent == 0U) {
        continue;
      }
      mapped_index = transparent != 0U ?
          (uint32_t)palette->transparent_index : index;
      key = ((uint32_t)palette->rgb[(index * 3U) + 0U] << 16) |
            ((uint32_t)palette->rgb[(index * 3U) + 1U] << 8) |
            (uint32_t)palette->rgb[(index * 3U) + 2U];
      key += 1U; /* 0 marks an empty slot */
      slot = zz9k_picture_lut8_map_slot(key);
      while (keys[slot] != 0U && keys[slot] != key) {
        slot = (slot + 1U) & (ZZ9K_PICTURE_LUT8_MAP_SLOTS - 1U);
      }
      if (keys[slot] == 0U) {
        keys[slot] = key;
        vals[slot] = (uint8_t)(mapped_index + 1U);
      }
    }
  }
}

static uint8_t zz9k_picture_png_lut8_lookup(
    const ZZ9KPicturePngPalette *palette,
    const uint32_t *keys,
    const uint8_t *vals,
    uint8_t red,
    uint8_t green,
    uint8_t blue)
{
  uint32_t key;
  uint32_t slot;

  key = ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
  key += 1U;
  slot = zz9k_picture_lut8_map_slot(key);
  while (keys[slot] != 0U) {
    if (keys[slot] == key) {
      return (uint8_t)(vals[slot] - 1U);
    }
    slot = (slot + 1U) & (ZZ9K_PICTURE_LUT8_MAP_SLOTS - 1U);
  }
  /* Exact-decode pixels always hit the map; the nearest match only covers
   * firmware paths that hand back colour-transformed pixels. */
  return zz9k_picture_png_nearest_palette_index(palette, red, green, blue);
}

static uint8_t zz9k_picture_rgb_to_legacy_lut8_index(uint8_t red,
                                                     uint8_t green,
                                                     uint8_t blue)
{
  uint8_t red_level;
  uint8_t green_level;
  uint8_t blue_level;

  red_level = zz9k_picture_legacy_lut8_component(red);
  green_level = zz9k_picture_legacy_lut8_component(green);
  blue_level = zz9k_picture_legacy_lut8_component(blue);
  return (uint8_t)(1U + ((uint32_t)red_level * 36U) +
                   ((uint32_t)green_level * 6U) +
                   (uint32_t)blue_level);
}

static uint8_t zz9k_picture_surface_pixel_to_legacy_lut8_index(
    volatile uint8_t *src,
    uint32_t output_format,
    int has_alpha)
{
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t alpha;

  if (!src) {
    return 0U;
  }

  alpha = 255U;
  if (output_format == ZZ9K_SURFACE_FORMAT_RGB888) {
    red = src[0];
    green = src[1];
    blue = src[2];
  } else if (output_format == ZZ9K_SURFACE_FORMAT_RGBA8888) {
    red = src[0];
    green = src[1];
    blue = src[2];
    alpha = src[3];
  } else if (output_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
    alpha = src[0];
    red = src[1];
    green = src[2];
    blue = src[3];
  } else if (output_format == ZZ9K_SURFACE_FORMAT_BGRA8888) {
    blue = src[0];
    green = src[1];
    red = src[2];
    alpha = src[3];
  } else {
    return 0U;
  }

  if (has_alpha && alpha < 128U) {
    return 0U;
  }
  return zz9k_picture_rgb_to_legacy_lut8_index(red, green, blue);
}

static int zz9k_picture_accumulate_byte_offset(uint32_t count,
                                               uint32_t quantum,
                                               uint32_t *offset)
{
  uint32_t i;
  uint32_t total;

  if (!offset || quantum == 0U) {
    return 0;
  }

  total = 0U;
  for (i = 0U; i < count; i++) {
    if (total > (0xffffffffUL - quantum)) {
      return 0;
    }
    total += quantum;
  }

  *offset = total;
  return 1;
}

static int zz9k_picture_write_legacy_bitmap_tile(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  struct RastPort rast_port;
  struct RastPort line_rast_port;
  uint8_t *row_pixels;
  uint32_t row_alloc_bytes;
  uint32_t bytes_per_pixel;
  uint32_t src_min_row_bytes;
  uint32_t row;

  if (!tile || !tile->data || !result || !target ||
      !target->legacy_bitmap || !target->legacy_line_bitmap ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      result->tile_height > target->tile_rows) {
    return 0;
  }

  bytes_per_pixel = zz9k_surface_bytes_per_pixel(target->output_format);
  if (bytes_per_pixel == 0U ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, bytes_per_pixel, &src_min_row_bytes) ||
      tile_stride < src_min_row_bytes) {
    return 0;
  }

  row_alloc_bytes = ((uint32_t)result->tile_width + 15U) & ~(uint32_t)15U;
  row_pixels = (uint8_t *)AllocMem((ULONG)row_alloc_bytes, MEMF_PUBLIC);
  if (!row_pixels) {
    zz9k_picture_trace("decode: datatype legacy bitmap row alloc failed");
    return 0;
  }

  InitRastPort(&rast_port);
  InitRastPort(&line_rast_port);
  rast_port.BitMap = target->legacy_bitmap;
  line_rast_port.BitMap = target->legacy_line_bitmap;
  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint32_t col;

    src = (volatile uint8_t *)tile->data + (row * tile_stride);
    for (col = 0U; col < result->tile_width; col++) {
      row_pixels[col] =
          zz9k_picture_surface_pixel_to_legacy_lut8_index(
              src, target->output_format, target->legacy_has_alpha);
      src += bytes_per_pixel;
    }

    WritePixelLine8(
        &rast_port,
        result->tile_x,
        result->tile_y + row,
        result->tile_width,
        row_pixels,
        &line_rast_port);
  }

  FreeMem(row_pixels, (ULONG)row_alloc_bytes);
  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype legacy bitmap tile written");
  }
  return 1;
}

static int zz9k_picture_copy_raw_tile_to_direct_pixels(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    const struct pdtBlitPixelArray *pixels,
    uint32_t tile_stride)
{
  uint32_t row;
  uint32_t byte;
  uint32_t bytes_per_pixel;
  uint32_t row_bytes;
  uint32_t dst_row_offset;
  uint32_t dst_x_offset;
  volatile uint8_t *src_row;
  uint8_t *dst_row;

  if (!tile || !tile->data || !result || !pixels ||
      !pixels->pbpa_PixelData ||
      pixels->pbpa_PixelArrayMod == 0UL ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > pixels->pbpa_Width ||
      result->tile_y > pixels->pbpa_Height ||
      result->tile_width > (pixels->pbpa_Width - result->tile_x) ||
      result->tile_height > (pixels->pbpa_Height - result->tile_y)) {
    return 0;
  }

  bytes_per_pixel =
      zz9k_picture_pbpa_bytes_per_pixel(pixels->pbpa_PixelFormat);
  if (bytes_per_pixel == 0U ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, bytes_per_pixel, &row_bytes) ||
      tile_stride < row_bytes ||
      pixels->pbpa_PixelArrayMod < row_bytes ||
      !zz9k_picture_accumulate_byte_offset(
          result->tile_y, (uint32_t)pixels->pbpa_PixelArrayMod,
          &dst_row_offset) ||
      !zz9k_picture_accumulate_byte_offset(
          result->tile_x, bytes_per_pixel, &dst_x_offset)) {
    return 0;
  }

  src_row = (volatile uint8_t *)tile->data;
  dst_row = (uint8_t *)pixels->pbpa_PixelData +
            dst_row_offset + dst_x_offset;
  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint8_t *dst;

    src = src_row;
    dst = dst_row;
    for (byte = 0U; byte < row_bytes; byte++) {
      dst[byte] = src[byte];
    }
    src_row += tile_stride;
    dst_row += (uint32_t)pixels->pbpa_PixelArrayMod;
  }

  zz9k_picture_trace("decode: datatype direct alpha tile copied");
  return 1;
}

static int zz9k_picture_write_bgra_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  struct pdtBlitPixelArray pixels;
  uint32_t row;
  uint32_t col;
  uint32_t src_min_row_bytes;
  uint32_t dst_min_row_bytes;
  ULONG method_result;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl || !target->scratch_pixels || target->scratch_pitch == 0U ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      result->tile_height > target->tile_rows ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL,
          &src_min_row_bytes) ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL,
          &dst_min_row_bytes) ||
      tile_stride < src_min_row_bytes ||
      target->scratch_pitch < dst_min_row_bytes) {
    return 0;
  }

  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint8_t *dst;

    src = (volatile uint8_t *)tile->data + (row * tile_stride);
    dst = target->scratch_pixels + (row * target->scratch_pitch);
    for (col = 0U; col < result->tile_width; col++) {
      zz9k_picture_bgra_to_rgb(src, dst);
      src += ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
      dst += ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    }
  }

  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = target->scratch_pixels;
  pixels.pbpa_PixelFormat = PBPAFMT_RGB;
  pixels.pbpa_PixelArrayMod = target->scratch_pitch;
  pixels.pbpa_Left = result->tile_x;
  pixels.pbpa_Top = result->tile_y;
  pixels.pbpa_Width = result->tile_width;
  pixels.pbpa_Height = result->tile_height;
  method_result = DoSuperMethodA(target->cl, target->object, (Msg)&pixels);
  if (method_result == 0UL) {
    zz9k_picture_trace("decode: datatype writepixelarray failed");
    return 0;
  }

  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype tile written");
  }
  return 1;
}

static int zz9k_picture_write_palette_lut8_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  struct pdtBlitPixelArray pixels;
  uint32_t src_bytes_per_pixel;
  uint32_t row;
  uint32_t col;
  uint32_t src_min_row_bytes;
  uint32_t dst_min_row_bytes;
  ULONG method_result;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl || !target->lut8_palette || !target->lut8_map_keys ||
      !target->lut8_map_vals || !target->scratch_pixels ||
      target->scratch_pitch == 0U ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      result->tile_height > target->tile_rows) {
    return 0;
  }
  if (target->output_format == ZZ9K_SURFACE_FORMAT_RGB888) {
    src_bytes_per_pixel = ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
  } else if (target->output_format == ZZ9K_SURFACE_FORMAT_BGRA8888) {
    src_bytes_per_pixel = ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
  } else {
    zz9k_picture_trace("decode: datatype lut8 tile format rejected");
    return 0;
  }
  if (!zz9k_picture_min_row_bytes(
          result->tile_width, src_bytes_per_pixel, &src_min_row_bytes) ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, 1U, &dst_min_row_bytes) ||
      tile_stride < src_min_row_bytes ||
      target->scratch_pitch < dst_min_row_bytes) {
    return 0;
  }

  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint8_t *dst;

    src = (volatile uint8_t *)tile->data + (row * tile_stride);
    dst = target->scratch_pixels + (row * target->scratch_pitch);
    for (col = 0U; col < result->tile_width; col++) {
      uint8_t red;
      uint8_t green;
      uint8_t blue;

      if (src_bytes_per_pixel == ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL) {
        red = src[0];
        green = src[1];
        blue = src[2];
      } else {
        blue = src[0];
        green = src[1];
        red = src[2];
      }
      dst[col] = zz9k_picture_png_lut8_lookup(
          target->lut8_palette, target->lut8_map_keys,
          target->lut8_map_vals, red, green, blue);
      src += src_bytes_per_pixel;
    }
  }

  memset(&pixels, 0, sizeof(pixels));
  pixels.MethodID = PDTM_WRITEPIXELARRAY;
  pixels.pbpa_PixelData = target->scratch_pixels;
  pixels.pbpa_PixelFormat = PBPAFMT_LUT8;
  pixels.pbpa_PixelArrayMod = target->scratch_pitch;
  pixels.pbpa_Left = result->tile_x;
  pixels.pbpa_Top = result->tile_y;
  pixels.pbpa_Width = result->tile_width;
  pixels.pbpa_Height = result->tile_height;
  method_result = DoSuperMethodA(target->cl, target->object, (Msg)&pixels);
  if (method_result == 0UL) {
    zz9k_picture_trace("decode: datatype writepixelarray failed");
    return 0;
  }

  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype lut8 tile written");
  }
  return 1;
}

static int zz9k_picture_write_png_alpha_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  uint32_t min_row_bytes;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl ||
      target->output_format != ZZ9K_SURFACE_FORMAT_RGBA8888 ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      result->tile_height > target->tile_rows ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_RGBA_BYTES_PER_PIXEL,
          &min_row_bytes) ||
      tile_stride < min_row_bytes) {
    return 0;
  }

  if (!zz9k_picture_write_v43_rgba_buffer(
      target->cl, target->object, (uint8_t *)tile->data,
      result->tile_x, result->tile_y,
      result->tile_width, result->tile_height, tile_stride)) {
    zz9k_picture_trace("decode: datatype writepixelarray failed");
    return 0;
  }

  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype alpha tile written");
    zz9k_picture_trace_source("decode: datatype alpha tile written");
  }
  return 1;
}

#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
static int zz9k_picture_write_alpha_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride);

static int zz9k_picture_clear_transparent_alpha_rgb(
    uint8_t *row_pixels,
    uint32_t pixel_count,
    ULONG pixel_format)
{
  uint32_t pixel;
  int cleared;

  if (!row_pixels || pixel_count == 0U) {
    return 0;
  }

  cleared = 0;
  if (pixel_format == PBPAFMT_RGBA) {
    for (pixel = 0U; pixel < pixel_count; pixel++) {
      uint8_t *rgba;

      rgba = row_pixels + (pixel * ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL);
      if (rgba[3] == 0U) {
        if (rgba[0] != 0U || rgba[1] != 0U || rgba[2] != 0U) {
          cleared = 1;
        }
        rgba[0] = 0U;
        rgba[1] = 0U;
        rgba[2] = 0U;
      }
    }
    return cleared;
  }

  if (pixel_format == PBPAFMT_ARGB) {
    for (pixel = 0U; pixel < pixel_count; pixel++) {
      uint8_t *argb;

      argb = row_pixels + (pixel * ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL);
      if (argb[0] == 0U) {
        if (argb[1] != 0U || argb[2] != 0U || argb[3] != 0U) {
          cleared = 1;
        }
        argb[1] = 0U;
        argb[2] = 0U;
        argb[3] = 0U;
      }
    }
    return cleared;
  }

  return 0;
}

static int zz9k_picture_write_alpha_tile_rows_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride,
    ULONG pixel_format,
    uint32_t row_bytes)
{
  struct pdtBlitPixelArray pixels;
  uint8_t *row_pixels;
  uint32_t row;
  ULONG method_result;
  int clear_trace_sent;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl || row_bytes == 0U || tile_stride < row_bytes) {
    return 0;
  }

  row_pixels = (uint8_t *)AllocMem((ULONG)row_bytes, MEMF_PUBLIC);
  if (!row_pixels) {
    zz9k_picture_trace("decode: datatype alpha row alloc failed");
    return 0;
  }

  clear_trace_sent = 0;
  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint32_t byte;

    src = (volatile uint8_t *)tile->data + (row * tile_stride);
    for (byte = 0U; byte < row_bytes; byte++) {
      row_pixels[byte] = src[byte];
    }
    if (zz9k_picture_clear_transparent_alpha_rgb(
            row_pixels, result->tile_width, pixel_format) &&
        !clear_trace_sent) {
      zz9k_picture_trace("decode: datatype alpha transparent rgb cleared");
      clear_trace_sent = 1;
    }

    memset(&pixels, 0, sizeof(pixels));
    pixels.MethodID = PDTM_WRITEPIXELARRAY;
    pixels.pbpa_PixelData = row_pixels;
    pixels.pbpa_PixelFormat = pixel_format;
    pixels.pbpa_PixelArrayMod = 0;
    pixels.pbpa_Left = result->tile_x;
    pixels.pbpa_Top = result->tile_y + row;
    pixels.pbpa_Width = result->tile_width;
    pixels.pbpa_Height = 1;
    method_result = DoSuperMethodA(target->cl, target->object, (Msg)&pixels);
    if (method_result == 0UL) {
      zz9k_picture_trace("decode: datatype writepixelarray failed");
      FreeMem(row_pixels, (ULONG)row_bytes);
      return 0;
    }
  }

  FreeMem(row_pixels, (ULONG)row_bytes);
  return 1;
}

static int zz9k_picture_write_alpha_bitmap_tile(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    struct BitMap *bitmap,
    uint32_t output_format,
    uint32_t tile_stride)
{
  struct RastPort rast_port;
  uint8_t *row_pixels;
  uint32_t src_min_row_bytes;
  uint32_t row;
  ULONG pixel_format;

  if (!tile || !tile->data || !result || !bitmap ||
      result->tile_width == 0U || result->tile_height == 0U ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL,
          &src_min_row_bytes) ||
      tile_stride < src_min_row_bytes ||
      !zz9k_picture_alpha_surface_format_to_pbpa(output_format,
                                                 &pixel_format)) {
    return 0;
  }

  row_pixels = (uint8_t *)AllocMem((ULONG)result->tile_width, MEMF_PUBLIC);
  if (!row_pixels) {
    zz9k_picture_trace("decode: datatype alpha bitmap row alloc failed");
    return 0;
  }

  InitRastPort(&rast_port);
  rast_port.BitMap = bitmap;
  for (row = 0U; row < result->tile_height; row++) {
    volatile uint8_t *src;
    uint32_t col;

    src = (volatile uint8_t *)tile->data + (row * tile_stride);
    for (col = 0U; col < result->tile_width; col++) {
      row_pixels[col] =
          zz9k_picture_rgba_to_transparent_lut8_index(src, pixel_format);
      src += ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
    }

    WriteChunkyPixels(
        &rast_port,
        result->tile_x,
        result->tile_y + row,
        result->tile_x + result->tile_width - 1U,
        result->tile_y + row,
        row_pixels,
        result->tile_width);
  }

  FreeMem(row_pixels, (ULONG)result->tile_width);
  zz9k_picture_trace("decode: datatype alpha bitmap tile written");
  return 1;
}

static int zz9k_picture_write_alpha_surface_to_bitmap(
    Object *object,
    ZZ9KPictureInstance *instance,
    const ZZ9KSurface *surface,
    uint32_t expected_format)
{
  ZZ9KSharedBuffer tile;
  ZZ9KImageSessionResult result;
  struct BitMap *bitmap;
  int ok;

  if (!object || !instance || !surface ||
      instance->width == 0U || instance->height == 0U ||
      !surface->data ||
      (surface->flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0U ||
      surface->width != instance->width ||
      surface->height != instance->height ||
      surface->format != expected_format) {
    return 0;
  }

  bitmap = 0;
  ok = 0;
  if (!zz9k_picture_prepare_png_transparent_bitmap(
          object, instance, &bitmap)) {
    return 0;
  }

  memset(&tile, 0, sizeof(tile));
  tile.handle = ZZ9K_INVALID_HANDLE;
  tile.data = surface->data;
  tile.length = surface->length;

  memset(&result, 0, sizeof(result));
  result.tile_x = 0U;
  result.tile_y = 0U;
  result.tile_width = instance->width;
  result.tile_height = instance->height;

  if (zz9k_picture_write_alpha_bitmap_tile(
          &tile, &result, bitmap, expected_format, surface->pitch) &&
      zz9k_picture_publish_png_transparent_bitmap(
          object, instance, bitmap)) {
    bitmap = 0;
    ok = 1;
  }

  if (bitmap) {
    FreeBitMap(bitmap);
  }
  if (ok) {
    zz9k_picture_trace("decode: datatype alpha bitmap written");
  }
  return ok;
}

static int zz9k_picture_write_argb_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  if (target) {
    target->output_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  }
  return zz9k_picture_write_alpha_tile_to_object(
      tile, result, target, tile_stride);
}

static int zz9k_picture_write_alpha_tile_to_object(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  ULONG pixel_format;
  uint32_t min_row_bytes;

  if (!tile || !tile->data || !result || !target || !target->object ||
      !target->cl ||
      result->tile_width == 0U || result->tile_height == 0U ||
      result->tile_x > target->width ||
      result->tile_y > target->height ||
      result->tile_width > (target->width - result->tile_x) ||
      result->tile_height > (target->height - result->tile_y) ||
      !zz9k_picture_min_row_bytes(
          result->tile_width, ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL,
          &min_row_bytes) ||
      tile_stride < min_row_bytes) {
    return 0;
  }
  if (!zz9k_picture_alpha_surface_format_to_pbpa(
          target->output_format, &pixel_format)) {
    return 0;
  }
  if (!zz9k_picture_write_alpha_tile_rows_to_object(
          tile, result, target, tile_stride, pixel_format, min_row_bytes)) {
    return 0;
  }

  target->tiles_written++;
  if (target->tiles_written == 1U) {
    zz9k_picture_trace("decode: datatype alpha tile written");
  }
  return 1;
}

static int zz9k_picture_copy_tile_to_datatype(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride);

static int zz9k_picture_alpha_surface_all_opaque(
    const ZZ9KSurface *surface,
    uint32_t width,
    uint32_t height,
    uint32_t format)
{
  uint32_t min_row_bytes;
  uint32_t surface_bytes;
  uint32_t alpha_offset;
  uint32_t row;
  uint32_t col;
  volatile uint8_t *row_ptr;

  if (!surface || !surface->data || width == 0U || height == 0U ||
      surface->width < width || surface->height < height ||
      (surface->flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0U) {
    return 0;
  }
  if (format == ZZ9K_SURFACE_FORMAT_RGBA8888) {
    alpha_offset = 3U;
  } else if (format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
    alpha_offset = 0U;
  } else {
    return 0;
  }
  if (surface->format != format ||
      !zz9k_picture_min_row_bytes(
          width, ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL, &min_row_bytes) ||
      surface->pitch < min_row_bytes ||
      !zz9k_picture_accumulate_surface_bytes(
          height, surface->pitch, &surface_bytes) ||
      surface->length < surface_bytes) {
    return 0;
  }

  row_ptr = (volatile uint8_t *)surface->data;
  for (row = 0U; row < height; row++) {
    volatile uint8_t *alpha;

    alpha = row_ptr + alpha_offset;
    for (col = 0U; col < width; col++) {
      if (*alpha != 255U) {
        return 0;
      }
      alpha += ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
    }
    row_ptr += surface->pitch;
  }
  return 1;
}

static int zz9k_picture_write_alpha_surface_as_rgb(
    ZZ9KPictureDatatypeTarget *target,
    const ZZ9KSurface *surface,
    uint32_t format)
{
  struct pdtBlitPixelArray pixels;
  uint8_t *row_pixels;
  uint32_t src_min_row_bytes;
  uint32_t dst_row_bytes;
  uint32_t surface_bytes;
  uint32_t row;
  ULONG method_result;

  if (!target || !target->object || !target->cl || !surface ||
      !surface->data || target->width == 0U || target->height == 0U ||
      surface->width < target->width || surface->height < target->height ||
      surface->format != format ||
      (format != ZZ9K_SURFACE_FORMAT_RGBA8888 &&
       format != ZZ9K_SURFACE_FORMAT_ARGB8888) ||
      !zz9k_picture_min_row_bytes(
          target->width, ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL,
          &src_min_row_bytes) ||
      !zz9k_picture_min_row_bytes(
          target->width, ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL,
          &dst_row_bytes) ||
      surface->pitch < src_min_row_bytes ||
      !zz9k_picture_accumulate_surface_bytes(
          target->height, surface->pitch, &surface_bytes) ||
      surface->length < surface_bytes) {
    return 0;
  }

  row_pixels = (uint8_t *)AllocMem((ULONG)dst_row_bytes, MEMF_PUBLIC);
  if (!row_pixels) {
    zz9k_picture_trace("decode: png opaque rgba row alloc failed");
    return 0;
  }

  for (row = 0U; row < target->height; row++) {
    volatile uint8_t *src;
    uint8_t *dst;
    uint32_t col;

    src = (volatile uint8_t *)surface->data + (row * surface->pitch);
    dst = row_pixels;
    for (col = 0U; col < target->width; col++) {
      if (format == ZZ9K_SURFACE_FORMAT_RGBA8888) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
      } else {
        dst[0] = src[1];
        dst[1] = src[2];
        dst[2] = src[3];
      }
      src += ZZ9K_PICTURE_BGRA_BYTES_PER_PIXEL;
      dst += ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    }

    memset(&pixels, 0, sizeof(pixels));
    pixels.MethodID = PDTM_WRITEPIXELARRAY;
    pixels.pbpa_PixelData = row_pixels;
    pixels.pbpa_PixelFormat = PBPAFMT_RGB;
    pixels.pbpa_PixelArrayMod = 0;
    pixels.pbpa_Left = 0U;
    pixels.pbpa_Top = row;
    pixels.pbpa_Width = target->width;
    pixels.pbpa_Height = 1U;
    method_result = DoSuperMethodA(target->cl, target->object, (Msg)&pixels);
    if (method_result == 0UL) {
      zz9k_picture_trace("decode: datatype writepixelarray failed");
      FreeMem(row_pixels, (ULONG)dst_row_bytes);
      return 0;
    }
  }

  FreeMem(row_pixels, (ULONG)dst_row_bytes);
  target->tiles_written++;
  zz9k_picture_trace("decode: png opaque rgba surface write");
  zz9k_picture_trace_source("decode: png opaque rgba surface write");
  return 1;
}

static int zz9k_picture_write_png_surface_to_datatype(
    Class *cl,
    Object *object,
    ZZ9KPictureInstance *instance,
    const struct pdtBlitPixelArray *direct_pixels,
    const ZZ9KSurface *surface,
    int *opaque_alpha_out)
{
  ZZ9KPictureDatatypeTarget target;
  ZZ9KSharedBuffer tile;
  ZZ9KImageSessionResult result;
  uint32_t expected_format;
  uint32_t output_bpp;
  uint32_t min_row_bytes;
  uint32_t surface_bytes;

  if (!cl || !object || !instance || !surface ||
      instance->width == 0U || instance->height == 0U ||
      surface->handle == ZZ9K_INVALID_HANDLE ||
      surface->width != instance->width ||
      surface->height != instance->height ||
      (surface->flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0U ||
      !surface->data) {
    return 0;
  }
  expected_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  if (direct_pixels &&
      !zz9k_picture_alpha_pixel_format_to_surface_format(
          direct_pixels->pbpa_PixelFormat, &expected_format)) {
    return 0;
  } else if (!direct_pixels) {
    expected_format = surface->format;
  }
  if (surface->format != expected_format) {
    return 0;
  }
  output_bpp = zz9k_surface_bytes_per_pixel(expected_format);
  if (output_bpp == 0U) {
    return 0;
  }

  if (!zz9k_picture_min_row_bytes(
          instance->width, output_bpp,
          &min_row_bytes) ||
      surface->pitch < min_row_bytes ||
      !zz9k_picture_accumulate_surface_bytes(
          instance->height, surface->pitch, &surface_bytes) ||
      surface->length < surface_bytes) {
    return 0;
  }

  zz9k_picture_init_datatype_target(
      &target, cl, object, instance, expected_format, output_bpp);
  target.tile_rows = instance->height;
  memset(&tile, 0, sizeof(tile));
  tile.handle = ZZ9K_INVALID_HANDLE;
  tile.data = surface->data;
  tile.length = surface->length;

  memset(&result, 0, sizeof(result));
  result.tile_x = 0U;
  result.tile_y = 0U;
  result.tile_width = instance->width;
  result.tile_height = instance->height;

  zz9k_picture_trace("decode: png direct surface write");
  if (direct_pixels) {
    return zz9k_picture_copy_raw_tile_to_direct_pixels(
        &tile, &result, direct_pixels, surface->pitch);
  }
  if ((expected_format == ZZ9K_SURFACE_FORMAT_RGBA8888 ||
       expected_format == ZZ9K_SURFACE_FORMAT_ARGB8888) &&
      zz9k_picture_alpha_surface_all_opaque(
          surface, instance->width, instance->height, expected_format)) {
    zz9k_picture_trace("decode: png alpha surface is opaque");
    zz9k_picture_trace_source("decode: png alpha surface is opaque");
    if (opaque_alpha_out) {
      *opaque_alpha_out = 1;
    }
    return 0;
  }
  if (expected_format == ZZ9K_SURFACE_FORMAT_RGB888) {
    zz9k_picture_trace_source(
        "metadata: png datatype deferred rgb prepare");
    if (!zz9k_picture_prepare_png_datatype_v43(
            object, instance, 0)) {
      return 0;
    }
    if (!zz9k_picture_write_rgb_tile_to_object(
            &tile, &result, &target, surface->pitch)) {
      return 0;
    }
    return target.tiles_written != 0U;
  }
  if (expected_format == ZZ9K_SURFACE_FORMAT_RGBA8888 ||
      expected_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
    zz9k_picture_trace_source(
        "metadata: png datatype classic bitmap prepare");
    if (!zz9k_picture_write_alpha_surface_to_bitmap(
            object, instance, surface, expected_format)) {
      return 0;
    }
    target.tiles_written = 1U;
    return 1;
  }
  if (!zz9k_picture_write_bgra_tile_to_object(
          &tile, &result, &target, surface->pitch)) {
    return 0;
  }

  return target.tiles_written != 0U;
}

static int zz9k_picture_copy_png_local_surface_to_datatype(
    Class *cl,
    Object *object,
    ZZ9KContext *ctx,
    ZZ9KPictureInstance *instance,
    const struct pdtBlitPixelArray *direct_pixels,
    const ZZ9KSurface *local_surface)
{
  ZZ9KPictureDatatypeTarget target;
  ZZ9KSurface tile_surface;
  ZZ9KSharedBuffer tile;
  ZZ9KImageSessionResult result;
  ZZ9KSurfaceCopyDesc copy;
  ZZ9KRect source_rect;
  uint32_t expected_format;
  uint32_t output_bpp;
  uint32_t output_pitch;
  uint32_t output_bytes;
  uint32_t tile_rows;
  uint32_t tile_pitch;
  uint32_t tile_bytes;
  uint32_t tile_max_rows;
  uint32_t tile_target_bytes;
  uint32_t y;
  struct BitMap *alpha_bitmap;
  int tile_surface_allocated;
  int ok;

  if (!cl || !object || !ctx || !instance || !local_surface ||
      instance->width == 0U || instance->height == 0U ||
      local_surface->handle == ZZ9K_INVALID_HANDLE ||
      local_surface->width != instance->width ||
      local_surface->height != instance->height ||
      (local_surface->flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL) == 0U) {
    return 0;
  }
  expected_format = ZZ9K_SURFACE_FORMAT_ARGB8888;
  if (direct_pixels &&
      !zz9k_picture_alpha_pixel_format_to_surface_format(
          direct_pixels->pbpa_PixelFormat, &expected_format)) {
    return 0;
  } else if (!direct_pixels) {
    expected_format = local_surface->format;
  }
  if (local_surface->format != expected_format) {
    return 0;
  }
  output_bpp = zz9k_surface_bytes_per_pixel(expected_format);
  if (output_bpp == 0U) {
    return 0;
  }

  if (!zz9k_picture_surface_layout(
          instance->width, instance->height, expected_format,
          &output_pitch, &output_bytes) ||
      local_surface->pitch < output_pitch ||
      local_surface->length < output_bytes ||
      !zz9k_picture_choose_datatype_tile_layout(
          instance->width, expected_format,
          output_bpp,
          &tile_max_rows, &tile_target_bytes,
          &tile_rows, &tile_pitch, &tile_bytes) ||
      tile_bytes == 0U) {
    return 0;
  }
  zz9k_picture_trace_u32("decode: datatype tile max rows", tile_max_rows);
  zz9k_picture_trace_u32(
      "decode: datatype tile target bytes", tile_target_bytes);

  zz9k_picture_init_datatype_target(
      &target, cl, object, instance, expected_format, output_bpp);
  target.tile_rows = tile_rows;
  memset(&tile_surface, 0, sizeof(tile_surface));
  tile_surface.handle = ZZ9K_INVALID_HANDLE;
  alpha_bitmap = 0;
  tile_surface_allocated = 0;
  ok = 0;

  zz9k_picture_trace("decode: png before tile surface alloc");
  if (zz9k_alloc_surface_ex(ctx, instance->width, tile_rows,
                            expected_format, 0U,
                            tile_pitch, &tile_surface) != ZZ9K_STATUS_OK) {
    zz9k_picture_trace("decode: png tile surface alloc failed");
    goto cleanup;
  }
  tile_surface_allocated = 1;
  if (!tile_surface.data ||
      (tile_surface.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0U ||
      tile_surface.pitch < tile_pitch ||
      tile_surface.length < tile_bytes ||
      tile_surface.format != expected_format) {
    zz9k_picture_trace("decode: png tile surface data unavailable");
    goto cleanup;
  }
  if (!direct_pixels) {
    if (expected_format == ZZ9K_SURFACE_FORMAT_RGB888) {
      zz9k_picture_trace_source(
          "metadata: png datatype deferred rgb prepare");
      if (!zz9k_picture_prepare_png_datatype_v43(
              object, instance, 0)) {
        goto cleanup;
      }
    } else if (expected_format == ZZ9K_SURFACE_FORMAT_RGBA8888 ||
               expected_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
      zz9k_picture_trace_source(
          "metadata: png datatype classic bitmap prepare");
      if (!zz9k_picture_prepare_png_transparent_bitmap(
              object, instance, &alpha_bitmap)) {
        goto cleanup;
      }
    }
  }

  for (y = 0U; y < instance->height;) {
    uint32_t rows;

    rows = tile_rows;
    if (rows > (instance->height - y)) {
      rows = instance->height - y;
    }

    source_rect.x = 0U;
    source_rect.y = y;
    source_rect.w = instance->width;
    source_rect.h = rows;
    if (!zz9k_surface_build_copy_desc(
            &copy, local_surface->handle, tile_surface.handle,
            &source_rect, 0U, 0U, 0U) ||
        zz9k_copy_surface(ctx, &copy) != ZZ9K_STATUS_OK) {
      zz9k_picture_trace("decode: png tile surface copy failed");
      goto cleanup;
    }
    if (y == 0U) {
      zz9k_picture_trace("decode: png tile surface copied");
    }

    memset(&tile, 0, sizeof(tile));
    tile.handle = ZZ9K_INVALID_HANDLE;
    tile.data = tile_surface.data;
    tile.length = tile_surface.length;

    memset(&result, 0, sizeof(result));
    result.tile_x = 0U;
    result.tile_y = y;
    result.tile_width = instance->width;
    result.tile_height = rows;

    if (direct_pixels) {
      if (!zz9k_picture_copy_raw_tile_to_direct_pixels(
              &tile, &result, direct_pixels, tile_surface.pitch)) {
        goto cleanup;
      }
      target.tiles_written++;
    } else if (expected_format == ZZ9K_SURFACE_FORMAT_RGB888) {
      if (!zz9k_picture_write_rgb_tile_to_object(
              &tile, &result, &target, tile_surface.pitch)) {
        goto cleanup;
      }
    } else if (expected_format == ZZ9K_SURFACE_FORMAT_RGBA8888 ||
               expected_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
      if (!zz9k_picture_write_alpha_bitmap_tile(
              &tile, &result, alpha_bitmap, expected_format,
              tile_surface.pitch)) {
        goto cleanup;
      }
      target.tiles_written++;
    } else {
      if (!zz9k_picture_write_bgra_tile_to_object(
              &tile, &result, &target, tile_surface.pitch)) {
        goto cleanup;
      }
    }
    y += rows;
  }

  ok = target.tiles_written != 0U;
  if (ok && alpha_bitmap) {
    if (zz9k_picture_publish_png_transparent_bitmap(
            object, instance, alpha_bitmap)) {
      alpha_bitmap = 0;
    } else {
      ok = 0;
    }
  }

cleanup:
  if (alpha_bitmap) {
    FreeBitMap(alpha_bitmap);
  }
  if (tile_surface_allocated) {
    (void)zz9k_free_surface(ctx, tile_surface.handle);
  }
  return ok;
}
#endif

static int zz9k_picture_copy_tile_to_datatype(
    const ZZ9KSharedBuffer *tile,
    const ZZ9KImageSessionResult *result,
    ZZ9KPictureDatatypeTarget *target,
    uint32_t tile_stride)
{
  if (!target) {
    return 0;
  }
  if (target->legacy_bitmap) {
    return zz9k_picture_write_legacy_bitmap_tile(
        tile, result, target, tile_stride);
  }
  if (target->png_lut8) {
    return zz9k_picture_write_palette_lut8_tile_to_object(
        tile, result, target, tile_stride);
  }
  if (target->direct) {
    if (target->output_format == ZZ9K_SURFACE_FORMAT_RGB888 &&
      target->direct_pixels->pbpa_PixelFormat == PBPAFMT_RGB) {
      if (!zz9k_picture_copy_raw_tile_to_direct_pixels(
            tile, result, target->direct_pixels, tile_stride)) {
        return 0;
      }
    } else {
      zz9k_picture_trace("decode: datatype direct rgb mismatch");
      return 0;
    }
    target->tiles_written++;
    return 1;
  }
  if (target->output_format == ZZ9K_SURFACE_FORMAT_RGB888) {
    return zz9k_picture_write_rgb_tile_to_object(
        tile, result, target, tile_stride);
  }
  if (target->output_format == ZZ9K_SURFACE_FORMAT_RGBA8888 ||
      target->output_format == ZZ9K_SURFACE_FORMAT_ARGB8888) {
    return zz9k_picture_write_png_alpha_tile_to_object(
        tile, result, target, tile_stride);
  }
  return zz9k_picture_write_bgra_tile_to_object(
      tile, result, target, tile_stride);
}

static int zz9k_picture_feed_stream_to_datatype(
    ZZ9KContext *ctx,
    ZZ9KPictureSource *source,
    ZZ9KSharedBuffer *staging,
    uint8_t *read_scratch,
    uint32_t read_scratch_length,
    ZZ9KSharedBuffer *tile,
    uint32_t tile_stride,
    ZZ9KPictureDatatypeTarget *target,
    ZZ9KPictureCodec codec,
    uint32_t session,
    uint32_t drain_limit,
    ZZ9KImageSessionResult *final_result)
{
  uint32_t buffered;
  uint32_t empty_eof_feeds;
  uint32_t trace_chunks;
  int eof;

  if (!ctx || !source || !staging ||
      (source->type != ZZ9K_PICTURE_SOURCE_MEMORY &&
       (!read_scratch || read_scratch_length == 0U)) ||
      !tile || !target ||
      session == 0U || !final_result) {
    return 0;
  }

  buffered = 0U;
  empty_eof_feeds = 0U;
  trace_chunks = ZZ9K_PICTURE_DATATYPE_TRACE_CHUNKS;
  eof = 0;
  memset(final_result, 0, sizeof(*final_result));
  while (final_result->state != ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
    uint32_t consumed;

#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
    zz9k_picture_trace("decode: datatype feed loop");
    zz9k_picture_trace("decode: datatype before fill staging");
#endif
    if (!zz9k_picture_fill_staging(
            source, staging, read_scratch, read_scratch_length, &buffered,
            &eof, &trace_chunks)) {
      return 0;
    }
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
    zz9k_picture_trace("decode: datatype fill staging ok");
    zz9k_picture_trace_u32("decode: datatype buffered", buffered);
    zz9k_picture_trace_u32("decode: datatype eof", (uint32_t)eof);
#endif
    if (buffered == 0U && eof) {
      empty_eof_feeds++;
      if (empty_eof_feeds > drain_limit) {
        return 0;
      }
    }

    do {
      ZZ9KImageSessionFeedDesc feed;
      ZZ9KImageSessionResult result;
      int status;

#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
      zz9k_picture_trace("decode: datatype before feed desc");
#endif
      if (!zz9k_image_build_session_feed_desc(
              &feed, session, staging->handle, 0U, buffered,
              eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U)) {
        return 0;
      }
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
      zz9k_picture_trace("decode: datatype feed desc ready");
      zz9k_picture_trace_u32("decode: datatype feed length", buffered);
      zz9k_picture_trace_u32(
          "decode: datatype feed flags",
          eof ? ZZ9K_IMAGE_SESSION_FEED_EOF : 0U);
#endif

      memset(&result, 0, sizeof(result));
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
      zz9k_picture_trace("decode: datatype before session feed");
#endif
      status = zz9k_image_session_feed(ctx, &feed, &result);
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
      zz9k_picture_trace_u32(
          "decode: datatype session feed status", (uint32_t)status);
      zz9k_picture_trace_u32("decode: datatype result state", result.state);
      zz9k_picture_trace_u32(
          "decode: datatype result consumed", result.bytes_consumed);
      zz9k_picture_trace_u32(
          "decode: datatype result written", result.bytes_written);
      zz9k_picture_trace_u32(
          "decode: datatype result tile x", result.tile_x);
      zz9k_picture_trace_u32(
          "decode: datatype result tile y", result.tile_y);
      zz9k_picture_trace_u32(
          "decode: datatype result tile width", result.tile_width);
      zz9k_picture_trace_u32(
          "decode: datatype result tile height", result.tile_height);
#endif
      if (status != ZZ9K_STATUS_OK || result.bytes_consumed > buffered) {
        return 0;
      }

      consumed = result.bytes_consumed;
      if (consumed != 0U) {
        buffered -= consumed;
        if (buffered != 0U &&
            !zz9k_shared_move(staging, 0U, consumed, buffered)) {
          return 0;
        }
      }

      if (result.state == ZZ9K_IMAGE_SESSION_STATE_TILE_READY) {
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
        zz9k_picture_trace("decode: datatype before tile copy");
#endif
        if (!zz9k_picture_copy_tile_to_datatype(
                tile, &result, target, tile_stride)) {
          return 0;
        }
#if ZZ9K_PICTURE_DATATYPE_TRACE_VERBOSE
        zz9k_picture_trace("decode: datatype tile copy ok");
#endif
        if ((codec == ZZ9K_PICTURE_CODEC_PNG ||
             codec == ZZ9K_PICTURE_CODEC_WEBP) &&
            eof && buffered == 0U &&
            (result.flags & ZZ9K_IMAGE_SESSION_RESULT_PARTIAL) == 0U) {
          *final_result = result;
          final_result->state = ZZ9K_IMAGE_SESSION_STATE_COMPLETE;
          zz9k_picture_trace(codec == ZZ9K_PICTURE_CODEC_PNG ?
              "decode: datatype final png tile" :
              "decode: datatype final webp tile");
          break;
        }
      } else if (result.state == ZZ9K_IMAGE_SESSION_STATE_COMPLETE) {
        *final_result = result;
        zz9k_picture_trace("decode: datatype feed complete");
        break;
      } else if (result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT &&
                 result.state != ZZ9K_IMAGE_SESSION_STATE_HEADER_READY) {
        return 0;
      }
      if (zz9k_picture_no_progress_is_fatal(
              &result, buffered, staging->length, eof)) {
        return 0;
      }
      if (buffered == 0U ||
          (!zz9k_picture_stream_result_made_progress(&result) && !eof)) {
        break;
      }
    } while (1);
  }

  return 1;
}

#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
static int zz9k_picture_try_png_direct_surface(
    Class *cl,
    Object *object,
    ZZ9KContext *ctx,
    ZZ9KPictureSource *source,
    ZZ9KPictureInstance *instance,
    const struct pdtBlitPixelArray *direct_pixels,
    uint32_t output_format,
    const char **failure_out,
    int *opaque_alpha_out)
{
  ZZ9KPictureStreamInput stream_input;
  ZZ9KSurface surface;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult result;
  ZZ9KRect output_rect;
  uint32_t image_codec;
  uint32_t output_pitch;
  uint32_t output_bytes;
  uint32_t session;
  LONG original_pos;
  int surface_allocated;
  int session_open;
  int ok;
  int status;
  const char *failure;

  if (!cl || !object || !ctx || !source || !instance ||
      instance->codec != ZZ9K_PICTURE_CODEC_PNG ||
      instance->width == 0U || instance->height == 0U ||
      (output_format != ZZ9K_SURFACE_FORMAT_ARGB8888 &&
       output_format != ZZ9K_SURFACE_FORMAT_RGBA8888 &&
       output_format != ZZ9K_SURFACE_FORMAT_RGB888)) {
    return 0;
  }

  zz9k_picture_init_stream_input(&stream_input);
  memset(&surface, 0, sizeof(surface));
  memset(&begin, 0, sizeof(begin));
  memset(&result, 0, sizeof(result));
  surface.handle = ZZ9K_INVALID_HANDLE;
  session = 0U;
  original_pos = -1;
  surface_allocated = 0;
  session_open = 0;
  ok = 0;
  status = 0;
  failure = 0;

  zz9k_picture_trace("decode: before direct surface layout");
  if (!zz9k_picture_surface_layout(instance->width, instance->height,
                                   output_format, &output_pitch,
                                   &output_bytes)) {
    failure = "decode: surface layout failed";
    goto cleanup;
  }
  if (output_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    failure = "decode: surface too large";
    goto cleanup;
  }
  if (output_bytes > ZZ9K_PICTURE_DIRECT_DATATYPE_SURFACE_MAX_BYTES) {
    failure = "decode: png direct surface too large";
    goto cleanup;
  }

  if (!zz9k_picture_alloc_stream_input(
          ctx, source, &stream_input, &failure)) {
    goto cleanup;
  }

  zz9k_picture_trace("decode: png before direct surface alloc");
  status = zz9k_alloc_surface_ex(ctx, instance->width,
                                 instance->height, output_format,
                                 0U, output_pitch, &surface);
  if (status != ZZ9K_STATUS_OK) {
    failure = "decode: output surface alloc failed";
    goto cleanup;
  }
  surface_allocated = 1;
  if ((surface.flags & ZZ9K_SURFACE_FLAG_CPU_VISIBLE) == 0U ||
      !surface.data ||
      surface.pitch < output_pitch ||
      surface.length < output_bytes ||
      surface.format != output_format) {
    failure = "decode: png direct surface unavailable";
    goto cleanup;
  }
  zz9k_picture_trace("decode: png direct surface ready");

  image_codec = zz9k_picture_image_codec(instance->codec);
  output_rect.x = 0U;
  output_rect.y = 0U;
  output_rect.w = instance->width;
  output_rect.h = instance->height;
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, image_codec, surface.handle, &output_rect,
          output_format, 0U)) {
    failure = "decode: session desc failed";
    goto cleanup;
  }

  status = zz9k_image_session_begin(ctx, &begin, &result);
  if (status != ZZ9K_STATUS_OK ||
      result.session == 0U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    failure = "decode: session begin failed";
    goto cleanup;
  }
  session = result.session;
  session_open = 1;

  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    failure = "decode: seek beginning failed";
    goto cleanup;
  }
  if (!zz9k_picture_feed_stream(
          ctx, source, &stream_input.staging, stream_input.read_scratch,
          stream_input.read_scratch_bytes, session,
          instance->height + 8U, &result) ||
      result.image_width != instance->width ||
      result.image_height != instance->height ||
      result.output_format != output_format ||
      result.bytes_written != output_bytes) {
    failure = "decode: feed failed";
    goto cleanup;
  }

  if (session_open) {
    (void)zz9k_image_session_close(ctx, session, 0U);
    session_open = 0;
  }
  zz9k_picture_free_stream_input(ctx, &stream_input);

  if (!zz9k_picture_write_png_surface_to_datatype(
          cl, object, instance, direct_pixels, &surface,
          opaque_alpha_out)) {
    if (opaque_alpha_out && *opaque_alpha_out) {
      goto cleanup;
    }
    failure = "decode: png surface write failed";
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (original_pos >= 0) {
    zz9k_picture_restore_pos(source, original_pos);
  }
  if (session_open) {
    (void)zz9k_image_session_close(ctx, session, 0U);
  }
  if (surface_allocated) {
    (void)zz9k_free_surface(ctx, surface.handle);
  }
  zz9k_picture_free_stream_input(ctx, &stream_input);
  if (ok) {
    return 1;
  }
  if (failure_out && failure) {
    *failure_out = failure;
  }
  return 0;
}

static int zz9k_picture_try_png_local_surface(
    Class *cl,
    Object *object,
    ZZ9KContext *ctx,
    ZZ9KPictureSource *source,
    ZZ9KPictureInstance *instance,
    const struct pdtBlitPixelArray *direct_pixels,
    uint32_t output_format,
    const char **failure_out)
{
  ZZ9KPictureStreamInput stream_input;
  ZZ9KSurface local_surface;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult result;
  ZZ9KRect output_rect;
  uint32_t image_codec;
  uint32_t output_pitch;
  uint32_t output_bytes;
  uint32_t session;
  LONG original_pos;
  int surface_allocated;
  int session_open;
  int ok;
  int status;
  const char *failure;

  if (!cl || !object || !ctx || !source || !instance ||
      instance->codec != ZZ9K_PICTURE_CODEC_PNG ||
      instance->width == 0U || instance->height == 0U ||
      (output_format != ZZ9K_SURFACE_FORMAT_ARGB8888 &&
       output_format != ZZ9K_SURFACE_FORMAT_RGBA8888 &&
       output_format != ZZ9K_SURFACE_FORMAT_RGB888)) {
    return 0;
  }

  zz9k_picture_init_stream_input(&stream_input);
  memset(&local_surface, 0, sizeof(local_surface));
  memset(&begin, 0, sizeof(begin));
  memset(&result, 0, sizeof(result));
  local_surface.handle = ZZ9K_INVALID_HANDLE;
  session = 0U;
  original_pos = -1;
  surface_allocated = 0;
  session_open = 0;
  ok = 0;
  status = 0;
  failure = 0;

  zz9k_picture_trace("decode: before local surface layout");
  if (!zz9k_picture_surface_layout(instance->width, instance->height,
                                   output_format, &output_pitch,
                                   &output_bytes)) {
    failure = "decode: surface layout failed";
    goto cleanup;
  }
  if (output_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    failure = "decode: surface too large";
    goto cleanup;
  }

  if (!zz9k_picture_alloc_stream_input(
          ctx, source, &stream_input, &failure)) {
    goto cleanup;
  }

  zz9k_picture_trace("decode: png before local surface alloc");
  status = zz9k_alloc_surface_ex(ctx, instance->width,
                                 instance->height, output_format,
                                 ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                                 output_pitch, &local_surface);
  if (status != ZZ9K_STATUS_OK) {
    failure = "decode: output surface alloc failed";
    goto cleanup;
  }
  surface_allocated = 1;
  if ((local_surface.flags & ZZ9K_SURFACE_FLAG_ARM_LOCAL) == 0U ||
      local_surface.pitch < output_pitch ||
      local_surface.length < output_bytes ||
      local_surface.format != output_format) {
    failure = "decode: png local surface unavailable";
    goto cleanup;
  }
  zz9k_picture_trace("decode: png local surface ready");

  image_codec = zz9k_picture_image_codec(instance->codec);
  output_rect.x = 0U;
  output_rect.y = 0U;
  output_rect.w = instance->width;
  output_rect.h = instance->height;
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, image_codec, local_surface.handle, &output_rect,
          output_format, 0U)) {
    failure = "decode: session desc failed";
    goto cleanup;
  }

  status = zz9k_image_session_begin(ctx, &begin, &result);
  if (status != ZZ9K_STATUS_OK ||
      result.session == 0U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    failure = "decode: session begin failed";
    goto cleanup;
  }
  session = result.session;
  session_open = 1;

  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    failure = "decode: seek beginning failed";
    goto cleanup;
  }
  if (!zz9k_picture_feed_stream(
          ctx, source, &stream_input.staging, stream_input.read_scratch,
          stream_input.read_scratch_bytes, session,
          instance->height + 8U, &result) ||
      result.image_width != instance->width ||
      result.image_height != instance->height ||
      result.output_format != output_format ||
      result.bytes_written != output_bytes) {
    failure = "decode: feed failed";
    goto cleanup;
  }

  if (session_open) {
    (void)zz9k_image_session_close(ctx, session, 0U);
    session_open = 0;
  }
  zz9k_picture_free_stream_input(ctx, &stream_input);

  if (!zz9k_picture_copy_png_local_surface_to_datatype(
          cl, object, ctx, instance, direct_pixels, &local_surface)) {
    failure = "decode: png surface write failed";
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (original_pos >= 0) {
    zz9k_picture_restore_pos(source, original_pos);
  }
  if (session_open) {
    (void)zz9k_image_session_close(ctx, session, 0U);
  }
  if (surface_allocated) {
    (void)zz9k_free_surface(ctx, local_surface.handle);
  }
  zz9k_picture_free_stream_input(ctx, &stream_input);
  if (ok) {
    return 1;
  }
  if (failure_out && failure) {
    *failure_out = failure;
  }
  return 0;
}

static int zz9k_picture_decode_png_to_datatype_pixels(
    Class *cl,
    Object *object,
    ZZ9KPictureSource *source,
    ZZ9KPictureInstance *instance,
    int *opaque_alpha_out)
{
  ZZ9KContext *ctx;
  ZZ9KServiceInfo image_service;
  uint32_t output_format;
  const char *failure;
  int has_alpha;
  int decode_lock_held;
  int png_alpha_opaque;
  int ok;

  if (!cl || !object || !source || !instance ||
      instance->codec != ZZ9K_PICTURE_CODEC_PNG ||
      instance->width == 0U || instance->height == 0U) {
    return 0;
  }

  ctx = 0;
  memset(&image_service, 0, sizeof(image_service));
  failure = "decode: png surface decode failed";
  output_format = ZZ9K_SURFACE_FORMAT_RGB888;
  has_alpha = 0;
  decode_lock_held = 0;
  png_alpha_opaque = 0;
  ok = 0;
  if (opaque_alpha_out) {
    *opaque_alpha_out = 0;
  }

  if (instance->png_alpha_known) {
    has_alpha = instance->png_has_alpha ? 1 : 0;
  } else {
    if (!zz9k_picture_png_has_alpha(source, &has_alpha)) {
      zz9k_picture_trace("metadata: png alpha scan failed");
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
    instance->png_alpha_known = 1U;
    instance->png_has_alpha = has_alpha ? 1U : 0U;
  }
  zz9k_picture_trace_u32("metadata: png alpha detected", (uint32_t)has_alpha);
  if (has_alpha) {
    output_format = ZZ9K_SURFACE_FORMAT_RGBA8888;
  }

  if (!zz9k_picture_decode_lock_obtain()) {
    failure = "decode: serialize unavailable";
    goto cleanup;
  }
  decode_lock_held = 1;

  if (zz9k_picture_open_cached_context(&ctx) != ZZ9K_STATUS_OK) {
    failure = "decode: zz9k_open failed";
    goto cleanup;
  }
  if (!zz9k_picture_require_caps(ctx)) {
    failure = "decode: required capabilities missing";
    goto cleanup;
  }
  if (!zz9k_picture_require_stream_service(
          ctx, instance->codec, ZZ9K_IMAGE_OUTPUT_SURFACE,
          &image_service)) {
    failure = "decode: stream service missing";
    goto cleanup;
  }

  zz9k_picture_trace("decode: png trying cpu-visible direct surface");
  if (zz9k_picture_try_png_direct_surface(
          cl, object, ctx, source, instance, 0, output_format,
          &failure, &png_alpha_opaque)) {
    ok = 1;
  }
  if (png_alpha_opaque) {
    if (opaque_alpha_out) {
      *opaque_alpha_out = 1;
    }
    failure = 0;
    goto cleanup;
  }
  if (!ok) {
    zz9k_picture_trace("decode: png trying local direct surface");
    if (zz9k_picture_try_png_local_surface(
            cl, object, ctx, source, instance, 0, output_format,
            &failure)) {
      ok = 1;
    }
  }

cleanup:
  if (decode_lock_held) {
    zz9k_picture_decode_lock_release();
  }
  if (ok) {
    if (!zz9k_picture_finalize_datatype_v43_attrs(object, instance)) {
      return 0;
    }
    zz9k_picture_trace("metadata: datatype pixels ready");
    return 1;
  }
  if (failure) {
    zz9k_picture_trace(failure);
  }
  return 0;
}
#endif

static int zz9k_picture_try_datatype_v47_direct(
    Object *object,
    ZZ9KPictureInstance *instance,
    int png_has_alpha,
    struct pdtBlitPixelArray *pixels,
    uint32_t *pixel_bytes,
    ZZ9KPictureDatatypeTarget *target)
{
  if (!object || !instance || !pixels || !pixel_bytes || !target) {
    return 0;
  }
  if (png_has_alpha) {
    zz9k_picture_trace(
        "metadata: datatype dynamic v47 alpha skipped; v43 fallback");
    return 0;
  }
  if (!zz9k_picture_prepare_datatype_v47_direct(
          object, instance, pixels, pixel_bytes)) {
    return 0;
  }

  target->direct = 1U;
  target->direct_pixels = pixels;
  zz9k_picture_trace("metadata: datatype dynamic v47 direct path");
  return 1;
}

static int zz9k_picture_try_jpeg_datatype_v47_rgb_direct(
    Object *object,
    ZZ9KPictureInstance *instance,
    const ZZ9KServiceInfo *image_service,
    struct pdtBlitPixelArray *pixels,
    ZZ9KPictureDatatypeTarget *target)
{
  if (!object || !instance || !image_service || !pixels || !target ||
      instance->codec != ZZ9K_PICTURE_CODEC_JPEG) {
    return 0;
  }
  if (!zz9k_has_service_flag(image_service->flags,
                             ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT)) {
    zz9k_picture_trace("metadata: jpeg v47 rgb service unavailable");
    return 0;
  }
  if (!zz9k_picture_prepare_jpeg_datatype_v47_rgb_direct(
          object, instance, pixels)) {
    return 0;
  }

  target->direct = 1U;
  target->direct_pixels = pixels;
  zz9k_picture_trace("metadata: datatype jpeg v47 rgb direct path");
  return 1;
}

static int zz9k_picture_try_datatype_v43_writepixelarray(
    Object *object,
    ZZ9KPictureInstance *instance,
    int png_has_alpha)
{
  if (!object || !instance) {
    return 0;
  }

  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG ||
      instance->codec == ZZ9K_PICTURE_CODEC_WEBP) {
    if (!zz9k_picture_prepare_png_datatype_v43(
            object, instance, png_has_alpha)) {
      return 0;
    }
  } else if (!zz9k_picture_prepare_datatype_v43(object, instance)) {
    return 0;
  }

  zz9k_picture_trace("metadata: datatype dynamic v43 writepixelarray path");
  zz9k_picture_trace("metadata: datatype writepixelarray path");
  return 1;
}

static int zz9k_picture_decode_to_legacy_bitmap(
    Object *object,
    ZZ9KPictureInstance *instance,
    int png_has_alpha,
    ZZ9KPictureDatatypeTarget *target)
{
  struct BitMap *bitmap;
  struct BitMap *line_bitmap;

  if (!object || !instance || !target) {
    return 0;
  }

  bitmap = 0;
  line_bitmap = 0;
  zz9k_picture_trace("metadata: datatype dynamic legacy bitmap path");
  if (!zz9k_picture_prepare_legacy_bitmap(
          object, instance, png_has_alpha, &bitmap, &line_bitmap)) {
    return 0;
  }

  target->legacy_bitmap = bitmap;
  target->legacy_line_bitmap = line_bitmap;
  target->legacy_has_alpha = png_has_alpha ? 1U : 0U;
  return 1;
}

static int zz9k_picture_decode_to_datatype_pixels(
    Class *cl,
    Object *object,
    ZZ9KPictureSource *source,
    ZZ9KPictureInstance *instance)
{
  struct pdtBlitPixelArray pixels;
  ZZ9KPictureDatatypeTarget target;
  ZZ9KContext *ctx;
  ZZ9KPictureStreamInput stream_input;
  ZZ9KSharedBuffer tile;
  ZZ9KCaps caps;
  ZZ9KServiceInfo image_service;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult result;
  uint8_t *scratch_pixels;
  uint32_t pixel_bytes;
  uint32_t scratch_pitch;
  uint32_t scratch_bytes;
  uint32_t image_codec;
  uint32_t tile_rows;
  uint32_t tile_pitch;
  uint32_t tile_bytes;
  uint32_t tile_format;
  uint32_t tile_bpp;
  uint32_t tile_max_rows;
  uint32_t tile_target_bytes;
  uint32_t session;
  LONG original_pos;
  UWORD version;
  int png_has_alpha;
  int png_lut8;
#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
  int png_alpha_opaque;
#endif
  int tile_allocated;
  int scratch_allocated;
  int session_open;
  int decode_lock_held;
  int compact_z2;
  int ok;
  int status;
  const char *failure;

  if (!cl || !object || !source || !instance || instance->width == 0U ||
      instance->height == 0U) {
    return 0;
  }
  version = zz9k_picture_superclass_version();
  zz9k_picture_trace_u32("metadata: superclass version", (uint32_t)version);
  zz9k_picture_trace_u32("metadata: image width", instance->width);
  zz9k_picture_trace_u32("metadata: image height", instance->height);
  png_has_alpha = 0;
  png_lut8 = 0;
#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
  png_alpha_opaque = 0;
#endif
  /* The header scan records WebP alpha (VP8X flag / VP8L alpha hint) in the
   * same field, so both codecs take the RGBA route on 32-bit screens. */
  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG ||
      instance->codec == ZZ9K_PICTURE_CODEC_WEBP) {
    if (instance->png_alpha_known) {
      png_has_alpha = instance->png_has_alpha ? 1 : 0;
    } else {
      if (!zz9k_picture_png_has_alpha(source, &png_has_alpha)) {
        zz9k_picture_trace("metadata: datatype png alpha scan failed");
        SetIoErr(DTERROR_INVALID_DATA);
        return 0;
      }
      instance->png_alpha_known = 1U;
      instance->png_has_alpha = png_has_alpha ? 1U : 0U;
    }
    zz9k_picture_trace_u32(
        "metadata: datatype png alpha detected",
        (uint32_t)png_has_alpha);
  }
  if (version >= 43U && png_has_alpha &&
      instance->flatten_png_alpha) {
    zz9k_picture_trace_source(
        "metadata: datatype png alpha rgb compatibility path");
    png_has_alpha = 0;
    instance->png_has_alpha = 0U;
  }
  if (version >= 43U && instance->codec == ZZ9K_PICTURE_CODEC_PNG &&
      zz9k_picture_png_palette_lut8_eligible(&instance->png_palette)) {
    /* Indexed PNG: decode stays truecolor on the firmware side but is
     * published as LUT8 with the file's own palette, matching PNGdt44.
     * tRNS transparency becomes the transparent colour instead of a
     * per-pixel alpha channel that MUI composites in software. */
    png_lut8 = 1;
    png_has_alpha = 0;
    instance->png_has_alpha = 0U;
    zz9k_picture_trace_source(
        "metadata: datatype png palette lut8 path");
  }
  if (png_has_alpha) {
    zz9k_picture_trace_source(
        "metadata: datatype png alpha v43 rgba path");
#if ZZ9K_PICTURE_ENABLE_PNG_ALPHA_EXPERIMENTS
    zz9k_picture_trace_source(
        "metadata: datatype png alpha surface path");
    if (!zz9k_picture_decode_png_to_datatype_pixels(
            cl, object, source, instance, &png_alpha_opaque)) {
      if (png_alpha_opaque) {
        zz9k_picture_trace_source(
            "metadata: datatype png opaque alpha rgb tile path");
        png_has_alpha = 0;
        instance->png_has_alpha = 0U;
      } else {
        return 0;
      }
    } else {
      return 1;
    }
#endif
  }

  zz9k_picture_trace("metadata: datatype decode begin");
  ctx = 0;
  zz9k_picture_init_stream_input(&stream_input);
  memset(&tile, 0, sizeof(tile));
  memset(&caps, 0, sizeof(caps));
  memset(&image_service, 0, sizeof(image_service));
  memset(&begin, 0, sizeof(begin));
  memset(&result, 0, sizeof(result));
  memset(&pixels, 0, sizeof(pixels));
  zz9k_picture_init_datatype_target(&target, cl, object, instance, 0U, 0U);
  if (png_lut8) {
    target.png_lut8 = 1U;
    target.lut8_palette = &instance->png_palette;
  }
  tile.handle = ZZ9K_INVALID_HANDLE;
  scratch_pixels = 0;
  session = 0U;
  original_pos = -1;
  tile_allocated = 0;
  scratch_allocated = 0;
  session_open = 0;
  decode_lock_held = 0;
  compact_z2 = 0;
  ok = 0;
  status = 0;
  failure = 0;

  if (!zz9k_picture_decode_lock_obtain()) {
    failure = "decode: serialize unavailable";
    goto cleanup;
  }
  decode_lock_held = 1;

  if (zz9k_picture_open_cached_context(&ctx) != ZZ9K_STATUS_OK) {
    failure = "decode: zz9k_open failed";
    goto cleanup;
  }
  if (!zz9k_picture_require_datatype_caps(ctx)) {
    failure = "decode: required capabilities missing";
    goto cleanup;
  }
  if (zz9k_picture_query_cached_caps(ctx, &caps) &&
      (caps.capability_bits & ZZ9K_CAP_APERTURE_LAYOUT) != 0U) {
    compact_z2 = 1;
  }
  if (!zz9k_picture_require_stream_service(
          ctx, instance->codec, ZZ9K_IMAGE_OUTPUT_TILE_BUFFER,
          &image_service)) {
    failure = "decode: stream service missing";
    goto cleanup;
  }

#if ZZ9K_PICTURE_DYNAMIC_DATATYPE_FEATURES
#if ZZ9K_PICTURE_ENABLE_JPEG_DATATYPE_V47_RGB_DIRECT
  if (version >= 47U &&
      instance->codec == ZZ9K_PICTURE_CODEC_JPEG) {
    if (!zz9k_picture_try_jpeg_datatype_v47_rgb_direct(
            object, instance, &image_service, &pixels, &target)) {
      zz9k_picture_trace(
          "metadata: datatype jpeg v47 rgb unavailable; v43 fallback");
    }
  } else if (version >= 47U &&
             (instance->codec == ZZ9K_PICTURE_CODEC_PNG ||
              instance->codec == ZZ9K_PICTURE_CODEC_WEBP)) {
    zz9k_picture_trace(
        "metadata: datatype v47 alpha uses v43 writepixelarray");
  }
#elif ZZ9K_PICTURE_ENABLE_DATATYPE_V47_DIRECT
  if (version >= 47U) {
    if (!zz9k_picture_try_datatype_v47_direct(
            object, instance, png_has_alpha, &pixels, &pixel_bytes,
            &target)) {
      zz9k_picture_trace(
          "metadata: datatype dynamic v47 unavailable; v43 fallback");
    }
  }
#else
  if (version >= 47U) {
    zz9k_picture_trace(
        "metadata: datatype v47 direct disabled; v43 fallback");
  }
#endif
  if (!target.direct && !target.legacy_bitmap) {
    if (version >= 43U) {
      if (!zz9k_picture_try_datatype_v43_writepixelarray(
              object, instance, png_has_alpha)) {
        failure = "metadata: datatype v43 prepare failed";
        goto cleanup;
      }
    } else if (!zz9k_picture_decode_to_legacy_bitmap(
                   object, instance, png_has_alpha, &target)) {
      failure = "metadata: datatype legacy bitmap prepare failed";
      goto cleanup;
    }
  }
#elif ZZ9K_PICTURE_FORCE_DATATYPE_V47_TRUECOLOR
  zz9k_picture_trace("metadata: forced datatype v47 truecolor");
  if (version >= 47U &&
      zz9k_picture_prepare_datatype_v47_truecolor(
          object, instance, &pixels, &pixel_bytes)) {
    target.direct = 1U;
    target.direct_pixels = &pixels;
    zz9k_picture_trace("metadata: datatype v47 truecolor path");
  } else {
    zz9k_picture_trace(
        "metadata: datatype v47 truecolor unavailable; v43 fallback");
    if (!zz9k_picture_prepare_datatype_v43(object, instance)) {
      failure = "metadata: datatype v43 prepare failed";
      goto cleanup;
    }
    zz9k_picture_trace("metadata: datatype writepixelarray path");
  }
#elif ZZ9K_PICTURE_FORCE_DATATYPE_V47_DIRECT
  zz9k_picture_trace("metadata: forced datatype v47 direct");
  if (version >= 47U &&
      zz9k_picture_prepare_datatype_v47_direct(
          object, instance, &pixels, &pixel_bytes)) {
    target.direct = 1U;
    target.direct_pixels = &pixels;
    zz9k_picture_trace("metadata: datatype v47 direct path");
  } else {
    zz9k_picture_trace(
        "metadata: datatype v47 direct unavailable; v43 fallback");
    if (!zz9k_picture_prepare_datatype_v43(object, instance)) {
      failure = "metadata: datatype v43 prepare failed";
      goto cleanup;
    }
    zz9k_picture_trace("metadata: datatype writepixelarray path");
  }
#elif ZZ9K_PICTURE_FORCE_DATATYPE_V43_WRITEPIXELS
  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG) {
    if (!zz9k_picture_prepare_png_datatype_v43(
            object, instance, png_has_alpha)) {
      failure = "metadata: datatype png v43 prepare failed";
      goto cleanup;
    }
  } else {
    if (!zz9k_picture_prepare_datatype_v43(object, instance)) {
      failure = "metadata: datatype v43 prepare failed";
      goto cleanup;
    }
  }
  zz9k_picture_trace("metadata: datatype writepixelarray path");
#else
  if (version >= 47U) {
    if (!zz9k_picture_obtain_direct_pixels(
            object, &pixels, instance, instance->codec,
            instance->width, instance->height,
            &pixel_bytes)) {
      failure = "metadata: v47 direct required; placeholder";
      goto cleanup;
    }
    target.direct = 1U;
    target.direct_pixels = &pixels;
  } else {
    if (!zz9k_picture_set_v43_pixel_attrs(
            object, instance->codec, instance->width, instance->height,
            FALSE, TRUE)) {
      failure = "metadata: datatype v43 attrs failed";
      goto cleanup;
    }
    zz9k_picture_trace("metadata: v43 fallback allowed");
    zz9k_picture_trace("metadata: datatype writepixelarray path");
  }
#endif

  if (target.direct &&
      !zz9k_picture_direct_pixels_prefer_rgb888_tiles(
          &image_service, target.direct_pixels)) {
    zz9k_picture_trace(
        "metadata: datatype direct rgb888 unavailable; v43 fallback");
    target.direct = 0U;
    target.direct_pixels = 0;
    if (!zz9k_picture_try_datatype_v43_writepixelarray(
            object, instance, png_has_alpha)) {
      failure = (instance->codec == ZZ9K_PICTURE_CODEC_PNG ||
                 instance->codec == ZZ9K_PICTURE_CODEC_WEBP) ?
          "metadata: datatype v43 prepare failed" :
          "metadata: datatype v43 prepare failed";
      goto cleanup;
    }
  }
  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG ||
      instance->codec == ZZ9K_PICTURE_CODEC_WEBP) {
    if (!zz9k_picture_choose_png_datatype_tile_format(
            &image_service, target.direct ? target.direct_pixels : 0,
            png_has_alpha,
            &tile_format, &tile_bpp)) {
      failure = "decode: datatype tile format failed";
      goto cleanup;
    }
  } else if (!zz9k_picture_choose_datatype_tile_format(
                 &image_service, target.direct ? target.direct_pixels : 0,
                 &tile_format, &tile_bpp)) {
    failure = "decode: datatype tile format failed";
    goto cleanup;
  }
  target.output_format = tile_format;
  target.output_bpp = tile_bpp;
  zz9k_picture_trace("decode: datatype before tile layout");
  if (instance->codec == ZZ9K_PICTURE_CODEC_PNG) {
    if (!instance->png_interlaced) {
      /* Non-interlaced rows stream strictly in order, so a JPEG-style
       * partial-height tile works and the decode stays bounded by the
       * tile size instead of the whole image (the shared heap cannot
       * hold a full-height tile for large sources). Interlaced Adam7
       * passes revisit rows out of order and still need a full-height
       * tile on the firmware side. */
      if (!zz9k_picture_choose_datatype_tile_layout(
              instance->width, tile_format, tile_bpp,
              &tile_max_rows, &tile_target_bytes,
              &tile_rows, &tile_pitch, &tile_bytes)) {
        failure = "decode: datatype png tile layout failed";
        goto cleanup;
      }
    } else if (!zz9k_picture_choose_png_full_datatype_tile_layout(
            instance->width, instance->height, tile_bpp,
            &tile_max_rows, &tile_target_bytes,
            &tile_rows, &tile_pitch, &tile_bytes)) {
      failure = "decode: datatype png full tile layout failed";
      goto cleanup;
    }
  } else {
    if (!zz9k_picture_choose_datatype_tile_layout(
            instance->width, tile_format, tile_bpp,
            &tile_max_rows, &tile_target_bytes,
            &tile_rows, &tile_pitch, &tile_bytes)) {
      failure = "decode: datatype tile layout failed";
      goto cleanup;
    }
  }
  if (compact_z2 && tile_bytes > ZZ9K_PICTURE_Z2_TILE_TARGET_BYTES) {
    tile_target_bytes = ZZ9K_PICTURE_Z2_TILE_TARGET_BYTES;
    if (!zz9k_picture_choose_tile_layout(
            instance->width, tile_bpp, tile_max_rows, tile_target_bytes,
            &tile_rows, &tile_pitch, &tile_bytes) ||
        tile_bytes > ZZ9K_PICTURE_Z2_TILE_TARGET_BYTES) {
      failure = "decode: datatype tile row exceeds Z2 host window";
      goto cleanup;
    }
  }
  target.tile_rows = tile_rows;
  zz9k_picture_trace_u32("decode: datatype tile max rows", tile_max_rows);
  zz9k_picture_trace_u32(
      "decode: datatype tile target bytes", tile_target_bytes);
  zz9k_picture_trace("decode: datatype tile layout ready");
  zz9k_picture_trace_u32("decode: datatype tile rows", tile_rows);
  zz9k_picture_trace_u32("decode: datatype tile pitch", tile_pitch);
  zz9k_picture_trace_u32("decode: datatype tile bytes", tile_bytes);
  if (!zz9k_picture_alloc_stream_input(
          ctx, source, &stream_input, &failure)) {
    goto cleanup;
  }

  zz9k_picture_trace("decode: datatype before tile alloc");
  status = zz9k_alloc_shared(
      ctx, tile_bytes, 16U,
      compact_z2 ? ZZ9K_ALLOC_HOST_WINDOW : 0U, &tile);
  zz9k_picture_trace_u32(
      "decode: datatype tile alloc status", (uint32_t)status);
  if (status != ZZ9K_STATUS_OK) {
    failure = "decode: datatype tile alloc failed";
    goto cleanup;
  }
  tile_allocated = 1;
  zz9k_picture_trace("decode: datatype tile alloc ok");

  if (!target.direct && !target.legacy_bitmap &&
      (target.png_lut8 ||
       target.output_format == ZZ9K_SURFACE_FORMAT_BGRA8888)) {
    uint32_t scratch_bytes_per_pixel;

    scratch_bytes_per_pixel = target.png_lut8 ?
        1U : ZZ9K_PICTURE_RGB_BYTES_PER_PIXEL;
    if (!zz9k_picture_min_row_bytes(
            instance->width, scratch_bytes_per_pixel, &scratch_pitch) ||
        !zz9k_picture_accumulate_surface_bytes(
            tile_rows, scratch_pitch, &scratch_bytes)) {
      failure = "decode: datatype writepixelarray layout failed";
      goto cleanup;
    }
    scratch_pixels = (uint8_t *)AllocMem(
        (ULONG)scratch_bytes, MEMF_PUBLIC);
    if (!scratch_pixels) {
      failure = "decode: datatype writepixelarray alloc failed";
      goto cleanup;
    }
    scratch_allocated = 1;
    target.scratch_pixels = scratch_pixels;
    target.scratch_pitch = scratch_pitch;
    zz9k_picture_trace("metadata: datatype writepixelarray ready");
  }

  if (target.png_lut8) {
    target.lut8_map_keys = (uint32_t *)AllocMem(
        sizeof(uint32_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS, MEMF_PUBLIC);
    target.lut8_map_vals = (uint8_t *)AllocMem(
        sizeof(uint8_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS, MEMF_PUBLIC);
    if (!target.lut8_map_keys || !target.lut8_map_vals) {
      failure = "decode: datatype lut8 map alloc failed";
      goto cleanup;
    }
    zz9k_picture_build_png_lut8_map(
        target.lut8_palette, target.lut8_map_keys, target.lut8_map_vals);
    zz9k_picture_trace("metadata: datatype lut8 map ready");
  }


  image_codec = zz9k_picture_image_codec(instance->codec);
  zz9k_picture_trace_u32("decode: datatype image codec", image_codec);
  zz9k_picture_trace_u32("decode: datatype tile handle", tile.handle);
  zz9k_picture_trace("decode: datatype before session desc");
  if (!zz9k_image_build_tile_session_begin_desc(
          &begin, image_codec, tile.handle, tile_pitch, tile_rows,
          tile_format, 0U)) {
    failure = "decode: session desc failed";
    goto cleanup;
  }
  zz9k_picture_trace("decode: datatype session desc ready");

  zz9k_picture_trace("decode: datatype before session begin");
  status = zz9k_image_session_begin(ctx, &begin, &result);
  zz9k_picture_trace_u32(
      "decode: datatype session begin status", (uint32_t)status);
  zz9k_picture_trace_u32("decode: datatype session id", result.session);
  zz9k_picture_trace_u32("decode: datatype session state", result.state);
  if (status != ZZ9K_STATUS_OK ||
      result.session == 0U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    failure = "decode: session begin failed";
    goto cleanup;
  }
  session = result.session;
  session_open = 1;
  zz9k_picture_trace("decode: datatype session begin ok");

  zz9k_picture_trace("decode: datatype before seek");
  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    failure = "decode: seek beginning failed";
    goto cleanup;
  }
  zz9k_picture_trace("decode: datatype seek ready");
  zz9k_picture_trace("decode: datatype before feed");
  if (!zz9k_picture_feed_stream_to_datatype(
          ctx, source, &stream_input.staging, stream_input.read_scratch,
          stream_input.read_scratch_bytes, &tile, tile_pitch, &target,
          instance->codec, session, instance->height + 8U, &result) ||
      result.image_width != instance->width ||
      result.image_height != instance->height ||
      result.output_format != tile_format) {
    failure = "decode: datatype feed failed";
    goto cleanup;
  }
  zz9k_picture_trace_u32(
      "decode: datatype tiles written", target.tiles_written);
  ok = 1;

cleanup:
  if (original_pos >= 0) {
    zz9k_picture_restore_pos(source, original_pos);
  }
  if (session_open) {
    (void)zz9k_image_session_close(ctx, session, 0U);
  }
  zz9k_picture_free_stream_input(ctx, &stream_input);
  if (tile_allocated) {
    (void)zz9k_free_shared(ctx, tile.handle);
  }
  if (decode_lock_held) {
    zz9k_picture_decode_lock_release();
  }
  if (scratch_allocated) {
    FreeMem(scratch_pixels, (ULONG)scratch_bytes);
  }
  if (ok) {
    if (target.legacy_bitmap) {
      if (!zz9k_picture_publish_legacy_bitmap(
              object, instance, target.legacy_bitmap)) {
        failure = "metadata: datatype legacy bitmap publish failed";
        ok = 0;
      } else {
        target.legacy_bitmap = 0;
      }
    } else if (!zz9k_picture_finalize_datatype_v43_attrs(
                   object, instance)) {
      failure = "metadata: datatype final attrs failed";
      ok = 0;
    }
  }
  if (target.legacy_line_bitmap) {
    FreeBitMap(target.legacy_line_bitmap);
    target.legacy_line_bitmap = 0;
  }
  if (target.legacy_bitmap) {
    FreeBitMap(target.legacy_bitmap);
    target.legacy_bitmap = 0;
  }
  if (target.lut8_map_keys) {
    FreeMem(target.lut8_map_keys,
            sizeof(uint32_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS);
    target.lut8_map_keys = 0;
  }
  if (target.lut8_map_vals) {
    FreeMem(target.lut8_map_vals,
            sizeof(uint8_t) * ZZ9K_PICTURE_LUT8_MAP_SLOTS);
    target.lut8_map_vals = 0;
  }
  if (ok) {
    zz9k_picture_trace("metadata: datatype pixels ready");
    return 1;
  }
  if (failure) {
    zz9k_picture_trace(failure);
  }
  return 0;
}

static int zz9k_picture_decode_to_surface(ZZ9KPictureSource *source,
                                          ZZ9KPictureInstance *instance)
{
  ZZ9KPictureStreamInput stream_input;
  ZZ9KSurface surface;
  ZZ9KImageSessionBeginDesc begin;
  ZZ9KImageSessionResult result;
  ZZ9KRect output_rect;
  uint32_t image_codec;
  uint32_t output_format;
  uint32_t output_pitch;
  uint32_t output_bytes;
  uint32_t session;
  int surface_allocated;
  int session_open;
  int ok;
  const char *failure;

  if (!source || !instance || instance->width == 0U ||
      instance->height == 0U) {
    zz9k_picture_trace("decode: invalid input");
    return 0;
  }

  zz9k_picture_init_stream_input(&stream_input);
  memset(&surface, 0, sizeof(surface));
  memset(&begin, 0, sizeof(begin));
  memset(&result, 0, sizeof(result));
  surface.handle = ZZ9K_INVALID_HANDLE;
  session = 0U;
  surface_allocated = 0;
  session_open = 0;
  ok = 0;
  failure = 0;

  zz9k_picture_trace("decode: before zz9k_open");
  if (zz9k_open(&instance->ctx) != ZZ9K_STATUS_OK) {
    failure = "decode: zz9k_open failed";
    goto cleanup;
  }
  zz9k_picture_trace("decode: zz9k_open ok");
  zz9k_picture_trace("decode: before capability checks");
  if (!zz9k_picture_require_caps(instance->ctx)) {
    failure = "decode: required capabilities missing";
    goto cleanup;
  }
  zz9k_picture_trace("decode: before stream service check");
  if (!zz9k_picture_require_stream_service(instance->ctx,
                                           instance->codec,
                                           ZZ9K_IMAGE_OUTPUT_SURFACE,
                                           0)) {
    failure = "decode: stream service missing";
    goto cleanup;
  }
  if (!zz9k_picture_require_framebuffer(instance->ctx, instance)) {
    failure = "decode: framebuffer unavailable";
    goto cleanup;
  }

  if (!zz9k_picture_alloc_stream_input(
          instance->ctx, source, &stream_input, &failure)) {
    goto cleanup;
  }

  output_format = zz9k_surface_native_rtg_format();
  zz9k_picture_trace("decode: before local surface layout");
  if (!zz9k_picture_surface_layout(instance->width, instance->height,
                                   output_format, &output_pitch,
                                   &output_bytes)) {
    failure = "decode: surface layout failed";
    goto cleanup;
  }
  if (output_bytes > ZZ9K_PICTURE_MAX_SURFACE_BYTES) {
    failure = "decode: surface too large";
    goto cleanup;
  }

  zz9k_picture_trace("decode: before output surface alloc");
  if (zz9k_alloc_surface_ex(instance->ctx, instance->width,
                            instance->height, output_format,
                            ZZ9K_SURFACE_FLAG_ARM_LOCAL,
                            output_pitch, &surface) != ZZ9K_STATUS_OK) {
    failure = "decode: output surface alloc failed";
    goto cleanup;
  }
  surface_allocated = 1;
  zz9k_picture_trace("decode: output surface alloc ok");

  image_codec = zz9k_picture_image_codec(instance->codec);
  output_rect.x = 0U;
  output_rect.y = 0U;
  output_rect.w = instance->width;
  output_rect.h = instance->height;
  zz9k_picture_trace("decode: before session begin");
  if (!zz9k_image_build_surface_session_begin_desc(
          &begin, image_codec, surface.handle, &output_rect,
          output_format, 0U) ||
      zz9k_image_session_begin(instance->ctx, &begin, &result) !=
          ZZ9K_STATUS_OK ||
      result.session == 0U ||
      result.state != ZZ9K_IMAGE_SESSION_STATE_NEED_INPUT) {
    failure = "decode: session begin failed";
    goto cleanup;
  }
  session = result.session;
  session_open = 1;

  zz9k_picture_trace("decode: before session feed");
  if (!zz9k_picture_feed_stream(instance->ctx, source,
                                &stream_input.staging,
                                stream_input.read_scratch,
                                stream_input.read_scratch_bytes, session,
                                instance->height + 8U, &result) ||
      result.image_width != instance->width ||
      result.image_height != instance->height ||
      result.output_format != output_format ||
      result.bytes_written != output_bytes) {
    failure = "decode: feed failed";
    goto cleanup;
  }

  ok = 1;

cleanup:
  if (session_open) {
    (void)zz9k_image_session_close(instance->ctx, session, 0U);
  }
  zz9k_picture_free_stream_input(instance->ctx, &stream_input);
  if (ok) {
    instance->source_handle = surface.handle;
    instance->source_ready = 1;
    return 1;
  }
  if (surface_allocated) {
    (void)zz9k_free_surface(instance->ctx, surface.handle);
  }
  if (instance->ctx) {
    zz9k_close(instance->ctx);
    instance->ctx = 0;
  }
  if (failure) {
    zz9k_picture_trace(failure);
  }
  instance->source_handle = ZZ9K_INVALID_HANDLE;
  instance->source_ready = 0;
  return 0;
}

static int zz9k_picture_decode_source(ZZ9KPictureSource *source,
                                      ZZ9KPictureInstance *instance)
{
  LONG original_pos;
  int ok;

  if (!source || !instance) {
    return 0;
  }

  original_pos = zz9k_picture_source_seek(source, 0, OFFSET_CURRENT);
  if (!zz9k_picture_seek_begin(source)) {
    zz9k_picture_trace("decode: seek beginning failed");
    return 0;
  }
  ok = zz9k_picture_decode_to_surface(source, instance);
  zz9k_picture_restore_pos(source, original_pos);
  return ok;
}

static int zz9k_picture_prepare_hardware(Object *object,
                                         ZZ9KPictureInstance *instance)
{
  ZZ9KPictureSource source;
  ZZ9KPictureRenderMode render_mode;

#if !ZZ9K_PICTURE_RENDER_HARDWARE
  (void)object;
  (void)instance;
  return 0;
#else
  if (!object || !instance) {
    return 0;
  }
  render_mode = zz9k_picture_render_mode();
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DATATYPE) {
    zz9k_picture_trace("layout: datatype pixels; superclass");
    return 0;
  }
  if (zz9k_picture_reference_mode(render_mode)) {
    zz9k_picture_trace("layout: reference pattern; superclass");
    return 0;
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_OFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_SMALLOFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL) {
    zz9k_picture_trace("layout: hardware render off; using placeholder");
    return 0;
  }
  if (instance->source_ready) {
    if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DECODE) {
      zz9k_picture_trace("layout: hardware decode only; using placeholder");
      return 0;
    }
    if (!instance->render_attrs_ready &&
        zz9k_picture_enable_hardware_render(
            object, instance->codec, instance->width, instance->height)) {
      instance->render_attrs_ready = 1;
      zz9k_picture_trace("layout: subclass render enabled");
    }
    return instance->render_attrs_ready;
  }
  if (instance->decode_attempted) {
    return 0;
  }
  instance->decode_attempted = 1;

  if (!zz9k_picture_get_source(object, &source)) {
    return 0;
  }

  zz9k_picture_trace("layout: hardware decode begin");
  if (!zz9k_picture_decode_source(&source, instance)) {
    zz9k_picture_trace("layout: hardware unavailable; using placeholder");
    return 0;
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DECODE) {
    zz9k_picture_trace("layout: hardware decode only; using placeholder");
    return 0;
  }
  if (!zz9k_picture_enable_hardware_render(
          object, instance->codec, instance->width, instance->height)) {
    zz9k_picture_trace("layout: hardware unavailable; using placeholder");
    return 0;
  }

  instance->render_attrs_ready = 1;
  zz9k_picture_trace("layout: subclass render enabled");
  zz9k_picture_trace("layout: hardware decode ready");
  return 1;
#endif
}

static void zz9k_picture_notify_datatype_sync(
    Object *object,
    ZZ9KPictureInstance *instance,
    const struct gpLayout *layout)
{
  struct Window *window;
  struct Requester *requester;

  if (!object || !instance || instance->datatype_sync_sent) {
    return;
  }

  window = 0;
  requester = 0;
  if (layout && layout->gpl_GInfo) {
    window = layout->gpl_GInfo->gi_Window;
    requester = layout->gpl_GInfo->gi_Requester;
  }

  (void)SetDTAttrs(
      object, window, requester,
      DTA_Busy, FALSE,
      DTA_Sync, TRUE,
      TAG_END);
  instance->datatype_sync_sent = 1U;
  zz9k_picture_trace("layout: datatype sync notified");
}

static int zz9k_picture_load_metadata(Class *cl,
                                      Object *object,
                                      ZZ9KPictureInstance *instance)
{
  ZZ9KPictureSource source;
  ZZ9KPictureCodec codec;
  ZZ9KPicturePngPalette palette;
  uint32_t width;
  uint32_t height;
  ZZ9KPictureRenderMode render_mode;
  int png_has_alpha;
  uint8_t png_interlace;
  zz9k_picture_source_reset(&source);
  codec = ZZ9K_PICTURE_CODEC_UNKNOWN;
  memset(&palette, 0, sizeof(palette));
  palette.transparent_index = ZZ9K_PICTURE_PNG_TRANSPARENT_NONE;
  width = 0U;
  height = 0U;
  render_mode = ZZ9K_PICTURE_RENDER_MODE_DATATYPE;
  png_interlace = 0U;

  zz9k_picture_trace_reset();
  if (!object || !instance || !zz9k_picture_get_source(object, &source)) {
    SetIoErr(ERROR_REQUIRED_ARG_MISSING);
    return 0;
  }
  zz9k_picture_capture_object_name(object, instance);

  if (!zz9k_picture_read_dimensions(
          &source, &codec, &width, &height, &png_has_alpha,
          &png_interlace, &palette)) {
    zz9k_picture_trace("metadata: dimension read failed");
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  if (codec == ZZ9K_PICTURE_CODEC_JPEG) {
    zz9k_picture_trace_source("metadata: codec jpeg");
  } else if (codec == ZZ9K_PICTURE_CODEC_PNG) {
    zz9k_picture_trace_source("metadata: codec png");
  } else if (codec == ZZ9K_PICTURE_CODEC_WEBP) {
    zz9k_picture_trace_source("metadata: codec webp");
  }
  zz9k_picture_trace_source_hex("metadata: image width", width);
  zz9k_picture_trace_source_hex("metadata: image height", height);
  if (codec == ZZ9K_PICTURE_CODEC_PNG) {
    zz9k_picture_trace_source_hex(
        "metadata: png alpha", png_has_alpha ? 1U : 0U);
  } else if (codec == ZZ9K_PICTURE_CODEC_WEBP) {
    zz9k_picture_trace_source_hex(
        "metadata: webp alpha", png_has_alpha ? 1U : 0U);
  }

  instance->codec = codec;
  instance->width = width;
  instance->height = height;
  instance->framebuffer_width = 0U;
  instance->framebuffer_height = 0U;
  instance->source_handle = ZZ9K_INVALID_HANDLE;
  instance->source_ready = 0;
  instance->decode_attempted = 0;
  instance->render_attrs_ready = 0;
  instance->ctx = 0;
  instance->png_alpha_known =
      (codec == ZZ9K_PICTURE_CODEC_PNG || codec == ZZ9K_PICTURE_CODEC_WEBP) ? 1U : 0U;
  instance->png_has_alpha = png_has_alpha ? 1U : 0U;
  instance->png_interlaced =
      codec == ZZ9K_PICTURE_CODEC_PNG ? png_interlace : 0U;
  memset(&instance->png_palette, 0, sizeof(instance->png_palette));
  if (codec == ZZ9K_PICTURE_CODEC_PNG && palette.present) {
    instance->png_palette = palette;
    zz9k_picture_trace_source_hex(
        "metadata: png palette entries", (uint32_t)palette.count);
  }

  render_mode = zz9k_picture_render_mode();
#if ZZ9K_PICTURE_FORCE_DATATYPE_V43_WRITEPIXELS
  zz9k_picture_trace("metadata: forced datatype v43 writepixelarray");
#endif
  if (zz9k_picture_alpha_reference_mode(render_mode)) {
    instance->decode_attempted = 1;
    if (!zz9k_picture_set_v43_alpha_reference_pattern(
            cl, object, instance, codec, width, height)) {
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
    instance->source_ready = 0;
    return 1;
  }
  if (zz9k_picture_reference_mode(render_mode)) {
    instance->decode_attempted = 1;
#if ZZ9K_PICTURE_FORCE_REFERENCE_V43_WRITEPIXELS
    if (!zz9k_picture_set_v43_attrs_reference_pattern(
            cl, object, instance, codec, width, height)) {
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
#else
    if (!zz9k_picture_set_reference_pattern(
            object, instance, codec, width, height)) {
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
#endif
    instance->source_ready = 0;
    return 1;
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DATATYPE) {
    instance->decode_attempted = 1;
    zz9k_picture_trace_source("metadata: datatype decode begin");
    if (zz9k_picture_decode_to_datatype_pixels(
            cl, object, &source, instance)) {
      zz9k_picture_trace_source("metadata: datatype decode ok");
      instance->source_ready = 0;
      return 1;
    }
    zz9k_picture_trace_source("metadata: datatype decode failed");
    instance->source_ready = 0;
    if (codec == ZZ9K_PICTURE_CODEC_PNG || codec == ZZ9K_PICTURE_CODEC_WEBP) {
      zz9k_picture_trace(
          codec == ZZ9K_PICTURE_CODEC_WEBP ?
          "metadata: webp datatype decode failed; aborting" :
          "metadata: png datatype decode failed; aborting");
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
    zz9k_picture_trace("metadata: datatype decode failed; placeholder");
    if (!zz9k_picture_set_capped_placeholder_best(object, codec, width, height)) {
      SetIoErr(DTERROR_INVALID_DATA);
      return 0;
    }
    zz9k_picture_trace("metadata: decode deferred");
    return 1;
  }

  if (!zz9k_picture_set_object_dimensions(object, codec, width, height)) {
    SetIoErr(DTERROR_INVALID_DATA);
    return 0;
  }
  zz9k_picture_trace("metadata: decode deferred");
  return 1;
}

static int zz9k_picture_render_ready(const ZZ9KPictureInstance *instance)
{
  return instance && instance->source_ready && instance->ctx &&
         instance->source_handle != ZZ9K_INVALID_HANDLE &&
         instance->width != 0U && instance->height != 0U;
}

static void zz9k_picture_divmod_u32(uint32_t value,
                                    uint32_t divisor,
                                    uint32_t *quotient_out,
                                    uint32_t *remainder_out);

static int zz9k_picture_rect_from_signed_edges(int32_t left,
                                               int32_t top,
                                               int32_t right,
                                               int32_t bottom,
                                               ZZ9KFbRect *rect)
{
  if (!rect || right <= left || bottom <= top ||
      right <= 0 || bottom <= 0) {
    return 0;
  }
  if (left < 0) {
    left = 0;
  }
  if (top < 0) {
    top = 0;
  }
  if (right <= left || bottom <= top) {
    return 0;
  }

  rect->x = (uint32_t)left;
  rect->y = (uint32_t)top;
  rect->w = (uint32_t)(right - left);
  rect->h = (uint32_t)(bottom - top);
  return rect->w != 0U && rect->h != 0U;
}

static int zz9k_picture_rect_extents(const ZZ9KFbRect *rect,
                                     uint32_t *right,
                                     uint32_t *bottom)
{
  if (!rect || !right || !bottom || rect->w == 0U || rect->h == 0U ||
      rect->x > (0xffffffffUL - rect->w) ||
      rect->y > (0xffffffffUL - rect->h)) {
    return 0;
  }
  *right = rect->x + rect->w;
  *bottom = rect->y + rect->h;
  return 1;
}

static int zz9k_picture_intersect_rects(const ZZ9KFbRect *a,
                                        const ZZ9KFbRect *b,
                                        ZZ9KFbRect *out)
{
  uint32_t a_right;
  uint32_t a_bottom;
  uint32_t b_right;
  uint32_t b_bottom;
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;

  if (!out ||
      !zz9k_picture_rect_extents(a, &a_right, &a_bottom) ||
      !zz9k_picture_rect_extents(b, &b_right, &b_bottom)) {
    return 0;
  }

  left = a->x > b->x ? a->x : b->x;
  top = a->y > b->y ? a->y : b->y;
  right = a_right < b_right ? a_right : b_right;
  bottom = a_bottom < b_bottom ? a_bottom : b_bottom;
  if (right <= left || bottom <= top) {
    return 0;
  }

  out->x = left;
  out->y = top;
  out->w = right - left;
  out->h = bottom - top;
  return 1;
}

static int zz9k_picture_clip_rect_to_framebuffer(
    const ZZ9KPictureInstance *instance,
    ZZ9KFbRect *rect)
{
  if (!rect || rect->w == 0U || rect->h == 0U) {
    return 0;
  }
  if (!instance || instance->framebuffer_width == 0U ||
      instance->framebuffer_height == 0U) {
    return 1;
  }
  if (rect->x >= instance->framebuffer_width ||
      rect->y >= instance->framebuffer_height) {
    return 0;
  }
  if (rect->w > (instance->framebuffer_width - rect->x)) {
    rect->w = instance->framebuffer_width - rect->x;
  }
  if (rect->h > (instance->framebuffer_height - rect->y)) {
    rect->h = instance->framebuffer_height - rect->y;
  }
  return rect->w != 0U && rect->h != 0U;
}

static int zz9k_picture_window_content_rect(const struct Window *window,
                                            ZZ9KFbRect *rect)
{
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;

  if (!window || !rect) {
    return 0;
  }

  left = (int32_t)window->LeftEdge + (int32_t)window->BorderLeft;
  top = (int32_t)window->TopEdge + (int32_t)window->BorderTop;
  right = (int32_t)window->LeftEdge + (int32_t)window->Width -
          (int32_t)window->BorderRight;
  bottom = (int32_t)window->TopEdge + (int32_t)window->Height -
           (int32_t)window->BorderBottom;
  return zz9k_picture_rect_from_signed_edges(left, top, right, bottom, rect);
}

static int zz9k_picture_render_area(const ZZ9KPictureInstance *instance,
                                    const struct gpRender *render,
                                    ZZ9KFbRect *area)
{
  const struct Window *window;
  ZZ9KFbRect content;
  ZZ9KFbRect domain;
  int32_t x;
  int32_t y;
  int32_t right;
  int32_t bottom;

  if (!render || !render->gpr_GInfo || !area ||
      render->gpr_GInfo->gi_Domain.Width <= 0 ||
      render->gpr_GInfo->gi_Domain.Height <= 0) {
    return 0;
  }

  window = render->gpr_GInfo->gi_Window;
  x = (int32_t)render->gpr_GInfo->gi_Domain.Left;
  y = (int32_t)render->gpr_GInfo->gi_Domain.Top;
  if (window) {
    x += (int32_t)window->LeftEdge;
    y += (int32_t)window->TopEdge;
  }
  right = x + (int32_t)render->gpr_GInfo->gi_Domain.Width;
  bottom = y + (int32_t)render->gpr_GInfo->gi_Domain.Height;
  if (!zz9k_picture_rect_from_signed_edges(x, y, right, bottom, &domain) ||
      !zz9k_picture_clip_rect_to_framebuffer(instance, &domain)) {
    return 0;
  }

  *area = domain;
  if (window &&
      zz9k_picture_window_content_rect(window, &content) &&
      zz9k_picture_clip_rect_to_framebuffer(instance, &content)) {
    if (!zz9k_picture_intersect_rects(&domain, &content, area)) {
      return 0;
    }
  }

  return area->w != 0U && area->h != 0U;
}

static uint32_t zz9k_picture_muldiv_floor_u32(uint32_t value,
                                              uint32_t multiplier,
                                              uint32_t divisor)
{
  uint32_t add_quotient;
  uint32_t add_remainder;
  uint32_t quotient;
  uint32_t remainder;
  int bit;

  if (divisor == 0U || value == 0U || multiplier == 0U) {
    return 0U;
  }

  quotient = 0U;
  remainder = 0U;
  add_quotient = 0U;
  add_remainder = 0U;
  zz9k_picture_divmod_u32(value, divisor, &add_quotient, &add_remainder);
  for (bit = 31; bit >= 0; bit--) {
    if (quotient > 0x7fffffffUL) {
      return 0xffffffffUL;
    }
    quotient <<= 1;
    if (remainder >= (divisor - remainder)) {
      remainder = remainder - (divisor - remainder);
      if (quotient != 0xffffffffUL) {
        quotient++;
      }
    } else {
      remainder += remainder;
    }

    if (((multiplier >> bit) & 1U) == 0U) {
      continue;
    }

    if (quotient > (0xffffffffUL - add_quotient)) {
      return 0xffffffffUL;
    }
    quotient += add_quotient;
    if (add_remainder >= (divisor - remainder)) {
      remainder = add_remainder - (divisor - remainder);
      if (quotient != 0xffffffffUL) {
        quotient++;
      }
    } else {
      remainder += add_remainder;
    }
  }

  return quotient;
}

static void zz9k_picture_divmod_u32(uint32_t value,
                                    uint32_t divisor,
                                    uint32_t *quotient_out,
                                    uint32_t *remainder_out)
{
  uint32_t quotient;
  uint32_t remainder;
  int bit;

  if (!quotient_out || !remainder_out) {
    return;
  }

  quotient = 0U;
  remainder = 0U;
  if (divisor == 0U) {
    *quotient_out = 0U;
    *remainder_out = 0U;
    return;
  }

  for (bit = 31; bit >= 0; bit--) {
    if (quotient > 0x7fffffffUL) {
      quotient = 0xffffffffUL;
    } else {
      quotient <<= 1;
    }

    if (remainder >= (divisor - remainder)) {
      remainder = remainder - (divisor - remainder);
      if (quotient != 0xffffffffUL) {
        quotient++;
      }
    } else {
      remainder += remainder;
    }

    if (((value >> bit) & 1U) != 0U) {
      if (remainder == (divisor - 1U)) {
        remainder = 0U;
        if (quotient != 0xffffffffUL) {
          quotient++;
        }
      } else {
        remainder++;
      }
    }
  }

  *quotient_out = quotient;
  *remainder_out = remainder;
}

static int zz9k_picture_fit_size_to_area(uint32_t src_width,
                                         uint32_t src_height,
                                         uint32_t area_width,
                                         uint32_t area_height,
                                         uint32_t *width,
                                         uint32_t *height)
{
  uint32_t fitted_height;
  uint32_t fitted_width;

  if (src_width == 0U || src_height == 0U ||
      area_width == 0U || area_height == 0U ||
      !width || !height) {
    return 0;
  }

  fitted_height = zz9k_picture_muldiv_floor_u32(
      src_height, area_width, src_width);
  if (fitted_height == 0U) {
    fitted_height = 1U;
  }
  if (fitted_height <= area_height) {
    *width = area_width;
    *height = fitted_height;
    return 1;
  }

  fitted_width = zz9k_picture_muldiv_floor_u32(
      src_width, area_height, src_height);
  if (fitted_width == 0U) {
    fitted_width = 1U;
  }
  if (fitted_width > area_width) {
    fitted_width = area_width;
  }
  *width = fitted_width;
  *height = area_height;
  return 1;
}

static int zz9k_picture_choose_draw_rect_in_area(const ZZ9KFbRect *area,
                                                 uint32_t src_width,
                                                 uint32_t src_height,
                                                 ZZ9KFbRect *rect)
{
  uint32_t width;
  uint32_t height;

  if (!area || !rect || area->w == 0U || area->h == 0U ||
      src_width == 0U || src_height == 0U ||
      area->x > (0xffffffffUL - area->w) ||
      area->y > (0xffffffffUL - area->h)) {
    return 0;
  }
  if (!zz9k_picture_fit_size_to_area(src_width, src_height,
                                     area->w, area->h,
                                     &width, &height) ||
      width == 0U || height == 0U ||
      width > area->w || height > area->h) {
    return 0;
  }

  rect->x = area->x + ((area->w - width) >> 1U);
  rect->y = area->y + ((area->h - height) >> 1U);
  rect->w = width;
  rect->h = height;
  return 1;
}

static int zz9k_picture_hardware_screen_ok(
    ZZ9KPictureInstance *instance,
    const struct gpRender *render)
{
  const struct Screen *screen;

  if (!instance || !render || !render->gpr_GInfo ||
      !render->gpr_GInfo->gi_Screen) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_SCREEN_INFO_FAILED,
        "render: screen info unavailable; superclass");
    return 0;
  }

  screen = render->gpr_GInfo->gi_Screen;
  if (instance->framebuffer_width != 0U &&
      (uint32_t)screen->Width != instance->framebuffer_width) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_SCREEN_BOUNDS_FAILED,
        "render: screen bounds mismatch; superclass");
    return 0;
  }
  if (instance->framebuffer_height != 0U &&
      (uint32_t)screen->Height != instance->framebuffer_height) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_SCREEN_BOUNDS_FAILED,
        "render: screen bounds mismatch; superclass");
    return 0;
  }

  return 1;
}

static ULONG zz9k_picture_render(Class *cl, Object *object,
                                 struct gpRender *render)
{
#if !ZZ9K_PICTURE_RENDER_HARDWARE
  return DoSuperMethodA(cl, object, (Msg)render);
#else
  ZZ9KPictureInstance *instance;
  ZZ9KFbRect area;
  ZZ9KFbRect draw_rect;
  ZZ9KFbRect visible[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
  ZZ9KFbRect clips[ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS];
  uint32_t visible_count;
  uint32_t clip_count;
  uint32_t fit_width;
  uint32_t fit_height;
  ZZ9KPictureRenderMode render_mode;

  instance = (ZZ9KPictureInstance *)INST_DATA(cl, object);
  render_mode = zz9k_picture_render_mode();
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DATATYPE) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_OFF,
                                   "render: mode datatype; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (zz9k_picture_reference_mode(render_mode)) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_OFF,
                                   "render: mode reference; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_OFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_SMALLOFF ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_V43SMALL) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_OFF,
                                   "render: mode off; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DECODE) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_DECODE,
                                   "render: mode decode; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_SUBCLASS) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_SUBCLASS,
                                   "render: mode subclass complete");
    return 1;
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_SUPER) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_SUPER,
                                   "render: mode super; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (!zz9k_picture_render_ready(instance)) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_SOURCE_NOT_READY,
        "render: source not ready; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (!zz9k_picture_hardware_screen_ok(instance, render)) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_SCREEN_REJECTED,
        "render: screen rejected; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_SCREEN) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_MODE_SCREEN_COMPLETE,
        "render: mode screen complete");
    return 1;
  }
  zz9k_picture_trace_render_once(instance,
                                 ZZ9K_PICTURE_RENDER_TRACE_BEGIN,
                                 "render: hardware render begin");
  if (!zz9k_picture_render_area(instance, render, &area)) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_AREA_FAILED,
        "render: render area unavailable; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  zz9k_picture_trace_render_once(instance,
                                 ZZ9K_PICTURE_RENDER_TRACE_AREA_READY,
                                 "render: area ready");
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_AREA) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_MODE_AREA_COMPLETE,
        "render: mode area complete");
    return 1;
  }

  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY ||
      render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAW) {
    zz9k_picture_trace_render(
        render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWCOPY ?
        "render: mode drawcopy complete" :
        "render: mode draw complete");
    return 1;
  }

  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWFIT) {
    if (!zz9k_picture_fit_size_to_area(instance->width, instance->height,
                                       area.w, area.h,
                                       &fit_width, &fit_height)) {
      zz9k_picture_trace_render_once(
          instance, ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_FAILED,
          "render: draw rect unavailable; superclass");
      return DoSuperMethodA(cl, object, (Msg)render);
    }
    (void)fit_width;
    (void)fit_height;
    zz9k_picture_trace_render("render: mode drawfit complete");
    return 1;
  }

  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWCENTER) {
    if (!zz9k_picture_choose_draw_rect_in_area(
            &area, instance->width, instance->height, &draw_rect)) {
      zz9k_picture_trace_render_once(
          instance, ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_FAILED,
          "render: draw rect unavailable; superclass");
      return DoSuperMethodA(cl, object, (Msg)render);
    }
    zz9k_picture_trace_render("render: mode drawcenter complete");
    return 1;
  }

  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DRAWTRACE) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_READY,
                                   "render: draw rect ready");
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_MODE_DRAW_COMPLETE,
        "render: mode drawtrace complete");
    return 1;
  }

#if 0
  zz9k_picture_trace_render_once(instance,
                                 ZZ9K_PICTURE_RENDER_TRACE_DRAW_RECT_READY,
                                 "render: draw rect ready");
#endif
#if ZZ9K_PICTURE_USE_LAYER_CLIPS
  if (!zz9k_image_window_visible_clips_for_window(
          render->gpr_GInfo->gi_Window, &area, visible,
          ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS, &visible_count)) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_VISIBLE_FAILED,
        "render: visible clips unavailable; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
#else
  visible[0] = area;
  visible_count = 1U;
#endif
  zz9k_picture_trace_render_once(instance,
                                 ZZ9K_PICTURE_RENDER_TRACE_VISIBLE_READY,
                                 "render: visible clips ready");
  if (!zz9k_image_window_build_damage_clips(
          visible, visible_count, &area, clips,
          ZZ9K_IMAGE_WINDOW_MAX_VISIBLE_CLIPS, &clip_count)) {
    zz9k_picture_trace_render_once(
        instance, ZZ9K_PICTURE_RENDER_TRACE_DAMAGE_FAILED,
        "render: damage clips unavailable; superclass");
    return DoSuperMethodA(cl, object, (Msg)render);
  }
  zz9k_picture_trace_render_once(instance,
                                 ZZ9K_PICTURE_RENDER_TRACE_DAMAGE_READY,
                                 "render: damage clips ready");
  if (render_mode == ZZ9K_PICTURE_RENDER_MODE_PROBE) {
    zz9k_picture_trace_render_once(instance,
                                   ZZ9K_PICTURE_RENDER_TRACE_MODE_PROBE,
                                   "render: mode probe complete");
    return 1;
  }
  /* GM_RENDER runs while Intuition holds the window layer. Keep this final
   * fallback deliberately mailbox-free even if another diagnostic enum is
   * added without its own lock-safe handler. */
  return DoSuperMethodA(cl, object, (Msg)render);
#endif
}

static void zz9k_picture_instance_dispose(ZZ9KPictureInstance *instance)
{
  if (!instance) {
    return;
  }
  if (instance->ctx && instance->source_handle != ZZ9K_INVALID_HANDLE) {
    (void)zz9k_free_surface(instance->ctx, instance->source_handle);
  }
  if (instance->ctx) {
    zz9k_close(instance->ctx);
  }
  memset(instance, 0, sizeof(*instance));
  instance->source_handle = ZZ9K_INVALID_HANDLE;
}

static ULONG zz9k_picture_datatype_dispatch(REG(a0, struct Hook *hook),
                                            REG(a2, Object *object),
                                            REG(a1, Msg msg))
{
  Class *cl;
  ULONG result;

  if (!hook || !msg) {
    return 0;
  }

  cl = (Class *)hook;
  switch (msg->MethodID) {
  case OM_NEW:
    result = DoSuperMethodA(cl, object, msg);
    if (result) {
      Object *new_object = (Object *)result;
      ZZ9KPictureInstance *instance =
          (ZZ9KPictureInstance *)INST_DATA(cl, new_object);

      memset(instance, 0, sizeof(*instance));
      instance->source_handle = ZZ9K_INVALID_HANDLE;
      zz9k_picture_capture_screen_alpha_policy(
          instance, ((struct opSet *)msg)->ops_AttrList);
      if (!zz9k_picture_load_metadata(cl, new_object, instance)) {
        CoerceMethod(cl, new_object, OM_DISPOSE);
        result = 0;
      } else {
        zz9k_picture_apply_object_name(new_object, instance);
      }
    }
    return result;

  case OM_DISPOSE:
  {
    ZZ9KPictureInstance *instance;
    uint8_t *reference_pixels;
    uint32_t reference_pixel_bytes;

    instance = (ZZ9KPictureInstance *)INST_DATA(cl, object);
    reference_pixels = instance ? instance->reference_pixels : 0;
    reference_pixel_bytes = instance ? instance->reference_pixel_bytes : 0U;
    if (instance) {
      instance->reference_pixels = 0;
      instance->reference_pixel_bytes = 0U;
      zz9k_picture_instance_dispose(instance);
    }
    result = DoSuperMethodA(cl, object, msg);
    if (reference_pixels && reference_pixel_bytes != 0U) {
      FreeMem(reference_pixels, (ULONG)reference_pixel_bytes);
    }
    return result;
  }

  case DTM_PROCLAYOUT:
  case DTM_ASYNCLAYOUT:
  {
    ZZ9KPictureInstance *instance;
    ZZ9KPictureRenderMode render_mode;

    instance = (ZZ9KPictureInstance *)INST_DATA(cl, object);
    (void)zz9k_picture_prepare_hardware(object, instance);
    render_mode = zz9k_picture_render_mode();
    if (zz9k_picture_reference_nolayout_mode(render_mode)) {
      zz9k_picture_trace("layout: reference nolayout skip");
      zz9k_picture_notify_datatype_sync(
          object, instance, (const struct gpLayout *)msg);
      zz9k_picture_trace("layout: reference nolayout notified");
      return 1;
    }
    result = DoSuperMethodA(cl, object, msg);
    if (render_mode == ZZ9K_PICTURE_RENDER_MODE_DATATYPE) {
      zz9k_picture_trace("layout: datatype superclass returned");
      zz9k_picture_notify_datatype_sync(
          object, instance, (const struct gpLayout *)msg);
    } else if (zz9k_picture_reference_mode(render_mode)) {
#if ZZ9K_PICTURE_FORCE_REFERENCE_V43_STYLE
      zz9k_picture_trace("layout: v43 reference superclass returned");
#else
      zz9k_picture_trace("layout: reference superclass returned");
      zz9k_picture_notify_datatype_sync(
          object, instance, (const struct gpLayout *)msg);
#endif
    }
    return result;
  }

  case GM_RENDER:
    return zz9k_picture_render(cl, object, (struct gpRender *)msg);

  default:
    return DoSuperMethodA(cl, object, msg);
  }
}

static ZZ9KPictureDatatypeBase *zz9k_picture_datatype_open(
    REG(a6, ZZ9KPictureDatatypeBase *base))
{
  if (!base) {
    return 0;
  }

  base->class_library.cl_Lib.lib_OpenCnt++;
  base->class_library.cl_Lib.lib_Flags &= (uint8_t)~LIBF_DELEXP;
  return base;
}

static BPTR zz9k_picture_datatype_close(
    REG(a6, ZZ9KPictureDatatypeBase *base))
{
  if (!base || base->class_library.cl_Lib.lib_OpenCnt == 0) {
    return 0;
  }

  base->class_library.cl_Lib.lib_OpenCnt--;
  if (base->class_library.cl_Lib.lib_OpenCnt == 0 &&
      (base->class_library.cl_Lib.lib_Flags & LIBF_DELEXP)) {
    return zz9k_picture_datatype_expunge(base);
  }
  return 0;
}

static void zz9k_picture_datatype_close_system_bases(void)
{
  if (UtilityBase) {
    CloseLibrary(UtilityBase);
    UtilityBase = 0;
  }
  if (DataTypesBase) {
    CloseLibrary(DataTypesBase);
    DataTypesBase = 0;
  }
  if (PictureBase) {
    CloseLibrary(PictureBase);
    PictureBase = 0;
  }
  if (GfxBase) {
    CloseLibrary((struct Library *)GfxBase);
    GfxBase = 0;
  }
  if (IntuitionBase) {
    CloseLibrary((struct Library *)IntuitionBase);
    IntuitionBase = 0;
  }
  if (DOSBase) {
    CloseLibrary((struct Library *)DOSBase);
    DOSBase = 0;
  }
}

static BPTR zz9k_picture_datatype_expunge(
    REG(a6, ZZ9KPictureDatatypeBase *base))
{
  BPTR segment;

  if (!base) {
    return 0;
  }
  if (base->class_library.cl_Lib.lib_OpenCnt != 0) {
    base->class_library.cl_Lib.lib_Flags |= LIBF_DELEXP;
    return 0;
  }

  Remove((struct Node *)base);
  if (base->class_added && base->class_library.cl_Class) {
    RemoveClass(base->class_library.cl_Class);
    base->class_added = 0;
  }
  if (base->class_library.cl_Class) {
    FreeClass(base->class_library.cl_Class);
    base->class_library.cl_Class = 0;
  }
  zz9k_picture_close_cached_context();
  zz9k_picture_decode_semaphore_ready = 0U;
  zz9k_picture_datatype_close_system_bases();

  segment = base->segment;
  FreeMem((uint8_t *)base - base->class_library.cl_Lib.lib_NegSize,
          base->class_library.cl_Lib.lib_NegSize +
          base->class_library.cl_Lib.lib_PosSize);
  return segment;
}

static ULONG zz9k_picture_datatype_null(void)
{
  return 0;
}

static Class *zz9k_picture_datatype_get_class(
    REG(a6, ZZ9KPictureDatatypeBase *base))
{
  return base ? base->class_library.cl_Class : 0;
}

static void zz9k_picture_datatype_free_unpublished(
    ZZ9KPictureDatatypeBase *base)
{
  if (!base) {
    return;
  }
  if (base->class_added && base->class_library.cl_Class) {
    RemoveClass(base->class_library.cl_Class);
  }
  if (base->class_library.cl_Class) {
    FreeClass(base->class_library.cl_Class);
  }
  FreeMem((uint8_t *)base - base->class_library.cl_Lib.lib_NegSize,
          base->class_library.cl_Lib.lib_NegSize +
          base->class_library.cl_Lib.lib_PosSize);
}

static ZZ9KPictureDatatypeBase *zz9k_picture_datatype_init(
    REG(a0, BPTR segment))
{
  ZZ9KPictureDatatypeBase *base;

  SysBase = *(struct ExecBase **)4;
  DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
  if (!DOSBase) {
    return 0;
  }
  UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 39);
  if (!UtilityBase) {
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }
  IntuitionBase = (struct IntuitionBase *)OpenLibrary(
      (CONST_STRPTR)"intuition.library", 39);
  if (!IntuitionBase) {
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }
  DataTypesBase = OpenLibrary((CONST_STRPTR)"datatypes.library", 39);
  if (!DataTypesBase) {
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }
  PictureBase = OpenLibrary((CONST_STRPTR)"datatypes/picture.datatype", 39);
  if (!PictureBase) {
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }
  GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
  if (!GfxBase) {
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }
  InitSemaphore(&zz9k_picture_decode_semaphore);
  zz9k_picture_decode_semaphore_ready = 1U;

  base = (ZZ9KPictureDatatypeBase *)MakeLibrary(
      (CONST_APTR)zz9k_picture_datatype_vectors, 0, 0,
      sizeof(*base), 0);
  if (!base) {
    zz9k_picture_decode_semaphore_ready = 0U;
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }

  base->class_library.cl_Lib.lib_Node.ln_Type = NT_LIBRARY;
  base->class_library.cl_Lib.lib_Node.ln_Name =
      (char *)ZZ9K_PICTURE_DATATYPE_NAME;
  base->class_library.cl_Lib.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
  base->class_library.cl_Lib.lib_Version = ZZ9K_PICTURE_DATATYPE_VERSION;
  base->class_library.cl_Lib.lib_Revision = ZZ9K_PICTURE_DATATYPE_REVISION;
  base->class_library.cl_Lib.lib_IdString =
      (APTR)ZZ9K_PICTURE_DATATYPE_ID_STRING;
  base->segment = segment;

  base->class_library.cl_Class = MakeClass(
      (CONST_STRPTR)ZZ9K_PICTURE_DATATYPE_NAME,
      (CONST_STRPTR)PICTUREDTCLASS, 0,
      sizeof(ZZ9KPictureInstance), 0);
  if (!base->class_library.cl_Class) {
    zz9k_picture_decode_semaphore_ready = 0U;
    zz9k_picture_datatype_free_unpublished(base);
    zz9k_picture_datatype_close_system_bases();
    return 0;
  }

  base->class_library.cl_Class->cl_Dispatcher.h_Entry =
      (ULONG (*)())zz9k_picture_datatype_dispatch;
  base->class_library.cl_Class->cl_UserData = (ULONG)base;
  AddClass(base->class_library.cl_Class);
  base->class_added = 1;

  AddLibrary((struct Library *)base);
  return base;
}
