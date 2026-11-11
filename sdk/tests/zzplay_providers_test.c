/*
 * Checking service-module descriptors and keeping the loaded set.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-providers.h"

static void *fake_open(const ZZPlayHost *host, char *err, uint32_t size)
{
  (void)host;
  (void)err;
  (void)size;
  return (void *)1;
}

static void fake_close(void *ctx)
{
  (void)ctx;
}

static int fake_list(void *ctx, const char *node, uint32_t start,
                     uint32_t count, const ZZPlayItemSink *sink, char *err,
                     uint32_t size)
{
  (void)ctx; (void)node; (void)start; (void)count; (void)sink; (void)err;
  (void)size;
  return 1;
}

static int fake_search(void *ctx, const char *query, const ZZPlayItemSink *sink,
                       char *err, uint32_t size)
{
  (void)ctx; (void)query; (void)sink; (void)err; (void)size;
  return 1;
}

static int fake_resolve(void *ctx, const char *item, ZZPlayResolved *out,
                        char *err, uint32_t size)
{
  (void)ctx; (void)item; (void)out; (void)err; (void)size;
  return 1;
}

static ZZPlayProvider make(const char *scheme)
{
  ZZPlayProvider p;

  memset(&p, 0, sizeof(p));
  p.id = ZZPLAY_PROVIDER_ID;
  p.abi = ZZPLAY_PROVIDER_ABI;
  p.size = sizeof(ZZPlayProvider);
  p.scheme = scheme;
  p.name = "Test";
  p.open = fake_open;
  p.close = fake_close;
  p.list = fake_list;
  p.search = fake_search;
  p.resolve = fake_resolve;
  return p;
}

static int test_check(void)
{
  ZZPlayProvider p = make("radio");

  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_VALID) return 1;
  p.abi = ZZPLAY_PROVIDER_ABI + 1UL;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_OTHER_ABI) return 2;
  p.abi = ZZPLAY_PROVIDER_ABI - 1UL;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_OTHER_ABI) return 3;
  p = make("radio");
  p.size = sizeof(ZZPlayProvider) - 4U;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_TOO_SMALL) return 4;
  p = make("radio");
  p.resolve = NULL;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_INCOMPLETE) return 5;
  p = make("radio");
  p.field_count = 2U;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_INCOMPLETE) return 6;
  p = make("radio");
  p.id = 0x12345678UL;
  if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_NOT_A_MODULE) return 7;
  if (zzplay_provider_check(NULL) != ZZPLAY_PROVIDER_NOT_A_MODULE) return 8;
  {
    static const char *const bad[] = { "", "Plex", "http", "https", "a-b",
                                       "averyveryverylongname" };
    size_t i;

    for (i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
      p = make(bad[i]);
      if (zzplay_provider_check(&p) != ZZPLAY_PROVIDER_BAD_SCHEME) return 9;
    }
  }
  if (strcmp(zzplay_provider_check_text(ZZPLAY_PROVIDER_OTHER_ABI),
             "built for another version of ZZPlay") != 0)
    return 10;
  return 0;
}

static int test_registry(void)
{
  static ZZPlayProviders set;
  ZZPlayProvider radio = make("radio");
  ZZPlayProvider plex = make("plex");
  ZZPlayProvider radio2 = make("radio");
  const char *why;
  const char *id;
  const ZZPlayProviderSlot *slot;

  memset(&set, 0, sizeof(set));
  if (!zzplay_providers_add(&set, "a-radio.zzprov", &radio, NULL, &why))
    return 1;
  if (!zzplay_providers_add(&set, "plex.zzprov", &plex, NULL, &why)) return 2;
  /* A second module for the same service is refused; the first stays. */
  if (zzplay_providers_add(&set, "z-radio.zzprov", &radio2, NULL, &why))
    return 3;
  if (strcmp(why, "another module already provides this service") != 0)
    return 4;
  slot = zzplay_providers_find(&set, "radio");
  if (slot == NULL || slot->provider != &radio ||
      strcmp(slot->file, "a-radio.zzprov") != 0)
    return 5;
  slot = zzplay_providers_for_uri(&set, "plex://abc123/4567", &id);
  if (slot == NULL || slot->provider != &plex || strcmp(id, "abc123/4567") != 0)
    return 6;
  if (zzplay_providers_for_uri(&set, "subsonic://x", &id) != NULL) return 7;
  if (zzplay_providers_for_uri(&set, "http://h/a.mp3", &id) != NULL) return 8;
  if (zzplay_providers_for_uri(&set, "Work:Music/a.mp3", &id) != NULL)
    return 9;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_check();
  if (rc != 0) { printf("check %d\n", rc); return 10 + rc; }
  rc = test_registry();
  if (rc != 0) { printf("registry %d\n", rc); return 30 + rc; }
  printf("zzplay_providers_test: all checks passed\n");
  return 0;
}
