/*
 * List SDK v2 services advertised by the firmware service registry.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zz9k/caps.h"
#include "zz9k/host.h"
#include <stdio.h>
#include <string.h>

/* Set by the logic test, which compiles this file directly to reach the
 * release requirement helpers without a board. */
#ifndef ZZ9K_SERVICES_NO_MAIN
#define ZZ9K_SERVICES_NO_MAIN 0
#endif

typedef enum ZZ9KServicesMode {
  ZZ9K_SERVICES_MODE_LIST,
  ZZ9K_SERVICES_MODE_LIST_ALL,
  ZZ9K_SERVICES_MODE_CHECK_RELEASE
} ZZ9KServicesMode;

typedef struct ReleaseServiceRequirement {
  uint32_t service_id;
  uint32_t required_caps;
  uint32_t required_flags;
  uint32_t opcode_base;
  uint32_t min_opcode_count;
} ReleaseServiceRequirement;

#define ZZ9K_RELEASE_SERVICE_VERSION_MAJOR 2U

static const ReleaseServiceRequirement release_services[] = {
  {
    ZZ9K_SERVICE_CORE,
    ZZ9K_CAP_MAILBOX | ZZ9K_CAP_POLLING_COMPLETION |
      ZZ9K_CAP_SERVICE_DISCOVERY,
    ZZ9K_SERVICE_FLAG_FIRMWARE,
    ZZ9K_SERVICE_CORE,
    6U
  },
  {
    ZZ9K_SERVICE_MEMORY,
    ZZ9K_CAP_SHARED_ALLOC | ZZ9K_CAP_MEMORY_OPS,
    ZZ9K_SERVICE_FLAG_FIRMWARE,
    ZZ9K_SERVICE_MEMORY,
    4U
  },
  {
    ZZ9K_SERVICE_SURFACE,
    ZZ9K_CAP_SURFACES | ZZ9K_CAP_FRAMEBUFFER_SURFACE |
      ZZ9K_CAP_SURFACE_OPS,
    ZZ9K_SERVICE_FLAG_FIRMWARE | ZZ9K_SERVICE_FLAG_ZERO_COPY,
    ZZ9K_SERVICE_SURFACE,
    5U
  },
  {
    ZZ9K_SERVICE_IMAGE,
    ZZ9K_CAP_IMAGE_SCALE | ZZ9K_CAP_IMAGE_DECODE,
    ZZ9K_SERVICE_FLAG_FIRMWARE |
      ZZ9K_SERVICE_FLAG_IMAGE_JPEG_BASELINE |
      ZZ9K_SERVICE_FLAG_IMAGE_JPEG_PROGRESSIVE |
      ZZ9K_SERVICE_FLAG_IMAGE_JPEG_DIRECT_BGRA |
      ZZ9K_SERVICE_FLAG_IMAGE_JPEG_SCALING |
      ZZ9K_SERVICE_FLAG_IMAGE_STREAMING_INPUT |
      ZZ9K_SERVICE_FLAG_IMAGE_TILE_OUTPUT |
      ZZ9K_SERVICE_FLAG_IMAGE_FRAMEBUFFER_OUTPUT |
      ZZ9K_SERVICE_FLAG_IMAGE_SCALE_BILINEAR |
      ZZ9K_SERVICE_FLAG_IMAGE_SCALE_CLIPPED |
      ZZ9K_SERVICE_FLAG_IMAGE_PNG_DIRECT_BGRA |
      ZZ9K_SERVICE_FLAG_IMAGE_RGB888_OUTPUT,
    ZZ9K_SERVICE_IMAGE,
    8U
  },
  {
    ZZ9K_SERVICE_CODEC,
    ZZ9K_CAP_COMPRESSION,
    ZZ9K_SERVICE_FLAG_FIRMWARE |
      ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_RAW |
      ZZ9K_SERVICE_FLAG_CODEC_ZLIB |
      ZZ9K_SERVICE_FLAG_CODEC_GZIP |
      ZZ9K_SERVICE_FLAG_CODEC_LZMA_ALONE |
      ZZ9K_SERVICE_FLAG_CODEC_LZMA2 |
      ZZ9K_SERVICE_FLAG_CODEC_CHECKSUM |
      ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_TEST |
      ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_STREAM |
      ZZ9K_SERVICE_FLAG_CODEC_DECOMPRESS_FEED |
      ZZ9K_SERVICE_FLAG_CODEC_DEFLATE_FEED |
      ZZ9K_SERVICE_FLAG_CODEC_ZLIB_FEED |
      ZZ9K_SERVICE_FLAG_CODEC_GZIP_FEED,
    ZZ9K_SERVICE_CODEC,
    6U
  },
  {
    ZZ9K_SERVICE_AUDIO,
    ZZ9K_CAP_AUDIO_DECODE | ZZ9K_CAP_AUDIO_PLAYBACK |
      ZZ9K_CAP_AUDIO_CONTROL | ZZ9K_CAP_AUDIO_METERING |
      ZZ9K_CAP_AUDIO_FABRIC,
    ZZ9K_SERVICE_FLAG_FIRMWARE |
      ZZ9K_SERVICE_FLAG_AUDIO_MP3_DECODE |
      ZZ9K_SERVICE_FLAG_AUDIO_MP3_STREAM |
      ZZ9K_SERVICE_FLAG_AUDIO_CONTROL |
      ZZ9K_SERVICE_FLAG_AUDIO_FABRIC |
      ZZ9K_SERVICE_FLAG_AUDIO_FABRIC_RATE,
    ZZ9K_SERVICE_AUDIO,
    21U /* 0x0500..0x0514 incl. control plane + fabric leases
         * (0x0512-0x0514); qualified on hardware 2026-08-28
         * (docs/audio-fabric.md) */
  },
  {
    ZZ9K_SERVICE_CRYPTO,
    ZZ9K_CAP_CRYPTO,
    ZZ9K_SERVICE_FLAG_FIRMWARE |
      ZZ9K_SERVICE_FLAG_CRYPTO_X25519,
    ZZ9K_SERVICE_CRYPTO,
    4U
  },
  {
    ZZ9K_SERVICE_DIAG,
    ZZ9K_CAP_DIAGNOSTICS,
    ZZ9K_SERVICE_FLAG_FIRMWARE,
    ZZ9K_SERVICE_DIAG,
    4U
  }
};

static void print_usage(void)
{
  printf("usage: zz9k-services [--all|--check-release]\n");
}

static void print_flag_name(int *first, const char *name)
{
  printf("%s%s", *first ? "" : ",", name);
  *first = 0;
}

static void print_service_flags(uint32_t service_id, uint32_t flags)
{
  uint32_t remaining;
  uint32_t i;
  uint32_t count;
  int first = 1;

  printf("           flag-names=");
  remaining = flags;
  count = zz9k_known_service_flag_count(service_id);
  for (i = 0; i < count; i++) {
    const char *name;
    uint32_t flag;

    flag = zz9k_known_service_flag(service_id, i);
    if (flag == 0U || (flags & flag) == 0U) {
      continue;
    }

    name = zz9k_service_flag_name(service_id, flag);
    if (name) {
      print_flag_name(&first, name);
      remaining &= ~flag;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? "" : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first) {
    printf("none");
  }
  printf("\n");
}

static void print_capability_names(const char *prefix, uint32_t caps)
{
  uint32_t remaining;
  uint32_t i;
  uint32_t count;
  int first = 1;

  printf("%s", prefix);
  remaining = caps;
  count = zz9k_known_capability_count();
  for (i = 0; i < count; i++) {
    const char *name;
    uint32_t bit;

    bit = zz9k_known_capability_bit(i);
    if (bit == 0U || (caps & bit) == 0U) {
      continue;
    }

    name = zz9k_capability_name(bit);
    if (name) {
      print_flag_name(&first, name);
      remaining &= ~bit;
    }
  }
  if (remaining != 0U) {
    printf("%s0x%08lx", first ? "" : ",", (unsigned long)remaining);
    first = 0;
  }
  if (first) {
    printf("none");
  }
  printf("\n");
}

static void print_service(const ZZ9KServiceInfo *service,
                          const char *fallback_name)
{
  const char *name;

  name = service->name[0] ? service->name : fallback_name;
  printf("%-10s id=0x%04lx version=%lu.%lu caps=0x%08lx flags=0x%08lx "
         "ops=0x%04lx+%lu inline=%lu\n",
         name,
         (unsigned long)service->service_id,
         (unsigned long)((service->version >> 16) & 0xffffUL),
         (unsigned long)(service->version & 0xffffUL),
         (unsigned long)service->capability_bits,
         (unsigned long)service->flags,
         (unsigned long)service->opcode_base,
         (unsigned long)service->opcode_count,
         (unsigned long)service->max_inline_payload);
  print_service_flags(service->service_id, service->flags);
}

/* Capabilities the release requires from the firmware as a whole rather
 * than from any single service registry entry. The firmware advertises
 * these only in the global capability word -- its audio service reports
 * AUDIO_DECODE|AUDIO_PLAYBACK and its memory service reports
 * SHARED_ALLOC|MEMORY_OPS -- so they must not be added to
 * release_services[], where they would read as missing on correct
 * firmware. mhizz9000.library refuses to allocate a decoder without
 * AUDIO_STREAM_DRAIN, and Zorro 2 MHI/mpega staging allocates from the
 * HOST_WINDOW_HEAP. Safe Zorro 2 mapping additionally requires
 * APERTURE_LAYOUT, but matched Zorro 3 firmware deliberately omits that
 * Z2-only capability. */
#define ZZ9K_RELEASE_GLOBAL_CAPS \
  (ZZ9K_CAP_AUDIO_STREAM_DRAIN | ZZ9K_CAP_HOST_WINDOW_HEAP)

static uint32_t release_required_capabilities(uint16_t zorro_version)
{
  uint32_t required;
  uint32_t i;

  required = ZZ9K_RELEASE_GLOBAL_CAPS;
  if (zorro_version == 2U) {
    required |= ZZ9K_CAP_APERTURE_LAYOUT;
  }
  for (i = 0U;
       i < (uint32_t)(sizeof(release_services) / sizeof(release_services[0]));
       i++) {
    required |= release_services[i].required_caps;
  }
  return required;
}

static int check_release_service(ZZ9KContext *ctx,
                                 const ReleaseServiceRequirement *required)
{
  ZZ9KServiceInfo service;
  const char *fallback_name;
  const char *name;
  uint32_t missing;
  uint32_t version_major;
  int status;
  int ok = 1;

  fallback_name = zz9k_service_name(required->service_id);
  memset(&service, 0, sizeof(service));
  status = zz9k_query_service(ctx, required->service_id, &service);
  if (status != ZZ9K_STATUS_OK) {
    printf("%-10s id=0x%04lx release query failed: %s (%d)\n",
           fallback_name,
           (unsigned long)required->service_id,
           zz9k_status_name(status),
           status);
    return 0;
  }

  name = service.name[0] ? service.name : fallback_name;
  version_major = (service.version >> 16) & 0xffffU;
  if (version_major != ZZ9K_RELEASE_SERVICE_VERSION_MAJOR) {
    printf("%-10s release version mismatch: got %lu.%lu expected %lu.x\n",
           name,
           (unsigned long)version_major,
           (unsigned long)(service.version & 0xffffU),
           (unsigned long)ZZ9K_RELEASE_SERVICE_VERSION_MAJOR);
    ok = 0;
  }

  missing = zz9k_missing_capabilities(service.capability_bits,
                                      required->required_caps);
  if (missing != 0U) {
    printf("%-10s release missing service capabilities: ", name);
    print_capability_names("", missing);
    ok = 0;
  }

  missing = zz9k_missing_service_flags(service.flags,
                                       required->required_flags);
  if (missing != 0U) {
    printf("%-10s release missing service flags:\n", name);
    print_service_flags(service.service_id, missing);
    ok = 0;
  }

  if (service.opcode_base != required->opcode_base) {
    printf("%-10s release opcode base mismatch: got 0x%04lx expected 0x%04lx\n",
           name,
           (unsigned long)service.opcode_base,
           (unsigned long)required->opcode_base);
    ok = 0;
  }

  if (service.opcode_count < required->min_opcode_count) {
    printf("%-10s release opcode count too small: got %lu expected >= %lu\n",
           name,
           (unsigned long)service.opcode_count,
           (unsigned long)required->min_opcode_count);
    ok = 0;
  }

  if (ok) {
    printf("%-10s release ok\n", name);
  }
  return ok;
}

static int check_release_services(ZZ9KContext *ctx, const ZZ9KCaps *caps,
                                  uint16_t zorro_version)
{
  uint32_t missing_caps;
  uint32_t i;
  int ok = 1;

  missing_caps = zz9k_missing_capabilities(
      caps->capability_bits, release_required_capabilities(zorro_version));
  if (missing_caps != 0U) {
    print_capability_names("release missing firmware capabilities: ",
                           missing_caps);
    ok = 0;
  }

  for (i = 0U;
       i < (uint32_t)(sizeof(release_services) / sizeof(release_services[0]));
       i++) {
    if (!check_release_service(ctx, &release_services[i])) {
      ok = 0;
    }
  }

  if (ok) {
    printf("release check ok\n");
  } else {
    printf("release check failed\n");
  }
  return ok;
}

#if !ZZ9K_SERVICES_NO_MAIN
int main(int argc, char **argv)
{
  ZZ9KContext *ctx = 0;
  ZZ9KCaps caps;
  ZZ9KBoard board;
  uint32_t count;
  uint32_t i;
  ZZ9KServicesMode mode = ZZ9K_SERVICES_MODE_LIST;
  int status;

  if (argc > 2) {
    print_usage();
    return 1;
  }
  if (argc == 2) {
    if (strcmp(argv[1], "--all") != 0) {
      if (strcmp(argv[1], "--check-release") != 0) {
        print_usage();
        return 1;
      }
      mode = ZZ9K_SERVICES_MODE_CHECK_RELEASE;
    } else {
      mode = ZZ9K_SERVICES_MODE_LIST_ALL;
    }
  }

  status = zz9k_open(&ctx);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-services: open failed: %s (%d)\n",
           zz9k_status_name(status), status);
    return 1;
  }

  memset(&caps, 0, sizeof(caps));
  status = zz9k_query_caps(ctx, &caps);
  if (status != ZZ9K_STATUS_OK) {
    printf("zz9k-services: query caps failed: %s (%d)\n",
           zz9k_status_name(status), status);
    zz9k_close(ctx);
    return 1;
  }

  if ((caps.capability_bits & ZZ9K_CAP_SERVICE_DISCOVERY) == 0) {
    printf("service discovery unavailable\n");
    zz9k_close(ctx);
    return 1;
  }

  if (mode == ZZ9K_SERVICES_MODE_CHECK_RELEASE) {
    int ok;

    status = zz9k_find_board(&board);
    if (status != ZZ9K_STATUS_OK) {
      printf("zz9k-services: board query failed: %s (%d)\n",
             zz9k_status_name(status), status);
      zz9k_close(ctx);
      return 1;
    }
    ok = check_release_services(ctx, &caps, board.zorro_version);
    zz9k_close(ctx);
    return ok ? 0 : 1;
  }

  count = zz9k_known_service_count();
  for (i = 0; i < count; i++) {
    uint32_t service_id;
    const char *fallback_name;
    ZZ9KServiceInfo service;

    service_id = zz9k_known_service_id(i);
    fallback_name = zz9k_service_name(service_id);
    memset(&service, 0, sizeof(service));

    if (!zz9k_service_advertised_by_caps(service_id,
                                         caps.capability_bits)) {
      if (mode == ZZ9K_SERVICES_MODE_LIST_ALL) {
        printf("%-10s id=0x%04lx skipped (capability not advertised)\n",
               fallback_name, (unsigned long)service_id);
      }
      continue;
    }

    status = zz9k_query_service(ctx, service_id, &service);
    if (status == ZZ9K_STATUS_NOT_FOUND ||
        status == ZZ9K_STATUS_UNSUPPORTED) {
      if (mode == ZZ9K_SERVICES_MODE_LIST_ALL) {
        printf("%-10s id=0x%04lx unavailable: %s (%d)\n",
               fallback_name, (unsigned long)service_id,
               zz9k_status_name(status), status);
      }
      continue;
    }
    if (status != ZZ9K_STATUS_OK) {
      printf("%-10s id=0x%04lx query failed: %s (%d)\n",
             fallback_name, (unsigned long)service_id,
             zz9k_status_name(status), status);
      continue;
    }

    print_service(&service, fallback_name);
  }

  zz9k_close(ctx);
  return 0;
}
#endif /* !ZZ9K_SERVICES_NO_MAIN */
