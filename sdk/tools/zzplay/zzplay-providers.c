/* ZZPlay's service modules. SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-providers.h"

#include <stdio.h>
#include <string.h>

ZZPlayProviderCheck zzplay_provider_check(const ZZPlayProvider *provider)
{
  const char *s;
  size_t n;

  if (provider == NULL || provider->id != ZZPLAY_PROVIDER_ID) {
    return ZZPLAY_PROVIDER_NOT_A_MODULE;
  }
  if (provider->abi != ZZPLAY_PROVIDER_ABI) {
    return ZZPLAY_PROVIDER_OTHER_ABI;
  }
  if (provider->size < sizeof(ZZPlayProvider)) {
    return ZZPLAY_PROVIDER_TOO_SMALL;
  }
  if (provider->name == NULL || provider->name[0] == '\0' ||
      provider->open == NULL || provider->close == NULL ||
      provider->list == NULL || provider->search == NULL ||
      provider->resolve == NULL ||
      (provider->field_count != 0U && provider->fields == NULL)) {
    return ZZPLAY_PROVIDER_INCOMPLETE;
  }
  s = provider->scheme;
  if (s == NULL) {
    return ZZPLAY_PROVIDER_BAD_SCHEME;
  }
  for (n = 0U; s[n] != '\0'; n++) {
    if (s[n] < 'a' || s[n] > 'z' || n + 1U >= ZZPLAY_PROVIDER_SCHEME_MAX) {
      return ZZPLAY_PROVIDER_BAD_SCHEME;
    }
  }
  /* http and https are the stream layer's, not a module's. */
  if (n == 0U || strcmp(s, "http") == 0 || strcmp(s, "https") == 0) {
    return ZZPLAY_PROVIDER_BAD_SCHEME;
  }
  return ZZPLAY_PROVIDER_VALID;
}

const char *zzplay_provider_check_text(ZZPlayProviderCheck check)
{
  switch (check) {
  case ZZPLAY_PROVIDER_VALID:
    return "ready";
  case ZZPLAY_PROVIDER_NOT_A_MODULE:
    return "not a ZZPlay service module";
  case ZZPLAY_PROVIDER_OTHER_ABI:
    return "built for another version of ZZPlay";
  case ZZPLAY_PROVIDER_TOO_SMALL:
    return "built for an older version of ZZPlay";
  case ZZPLAY_PROVIDER_INCOMPLETE:
    return "incomplete module";
  case ZZPLAY_PROVIDER_BAD_SCHEME:
    return "invalid service name";
  }
  return "unknown problem";
}

const ZZPlayProviderSlot *zzplay_providers_find(const ZZPlayProviders *set,
                                                const char *scheme)
{
  uint32_t i;

  if (set == NULL || scheme == NULL) {
    return NULL;
  }
  for (i = 0U; i < set->count; i++) {
    if (strcmp(set->slot[i].provider->scheme, scheme) == 0) {
      return &set->slot[i];
    }
  }
  return NULL;
}

int zzplay_providers_add(ZZPlayProviders *set, const char *file,
                         const ZZPlayProvider *provider, void *segment,
                         const char **why)
{
  ZZPlayProviderSlot *slot;

  if (why != NULL) {
    *why = "";
  }
  if (zzplay_providers_find(set, provider->scheme) != NULL) {
    if (why != NULL) {
      *why = "another module already provides this service";
    }
    return 0;
  }
  if (set->count >= ZZPLAY_PROVIDERS_MAX) {
    if (why != NULL) {
      *why = "too many service modules";
    }
    return 0;
  }
  slot = &set->slot[set->count++];
  (void)snprintf(slot->file, sizeof(slot->file), "%s", file);
  slot->provider = provider;
  slot->segment = segment;
  return 1;
}

const ZZPlayProviderSlot *zzplay_providers_for_uri(const ZZPlayProviders *set,
                                                   const char *uri,
                                                   const char **id)
{
  char scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  size_t n = 0U;

  if (uri == NULL) {
    return NULL;
  }
  while (uri[n] >= 'a' && uri[n] <= 'z' && n + 1U < sizeof(scheme)) {
    scheme[n] = uri[n];
    n++;
  }
  if (n == 0U || strncmp(uri + n, "://", 3U) != 0) {
    return NULL;
  }
  scheme[n] = '\0';
  if (id != NULL) {
    *id = uri + n + 3U;
  }
  return zzplay_providers_find(set, scheme);
}

#if defined(__amigaos__) || defined(AMIGA)

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

static int is_module_name(const char *name)
{
  static const char suffix[] = ".zzprov";
  size_t len = strlen(name);
  size_t s = sizeof(suffix) - 1U;
  size_t i;

  if (len <= s || len >= ZZPLAY_PROVIDER_FILE_MAX) {
    return 0;
  }
  for (i = 0U; i < s; i++) {
    char c = name[len - s + i];

    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != suffix[i]) {
      return 0;
    }
  }
  return 1;
}

uint32_t zzplay_providers_load(ZZPlayProviders *set, char *why,
                               uint32_t why_size)
{
  static char names[ZZPLAY_PROVIDERS_MAX][ZZPLAY_PROVIDER_FILE_MAX];
  static char path[ZZPLAY_PROVIDER_FILE_MAX + 24U];
  struct FileInfoBlock *fib;
  uint32_t found = 0U;
  uint32_t loaded = 0U;
  uint32_t i;
  BPTR lock;

  if (why != NULL && why_size != 0U) {
    why[0] = '\0';
  }
  lock = Lock((CONST_STRPTR)"PROGDIR:Providers", ACCESS_READ);
  if (!lock) {
    return 0U;
  }
  fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, 0);
  if (fib != NULL && Examine(lock, fib)) {
    while (found < ZZPLAY_PROVIDERS_MAX && ExNext(lock, fib)) {
      const char *name = (const char *)fib->fib_FileName;

      if (fib->fib_DirEntryType < 0 && is_module_name(name)) {
        uint32_t at = found++;

        /* Insert in name order, so the first of two modules claiming one
         * service is predictable. */
        while (at > 0U && strcmp(names[at - 1U], name) > 0) {
          memcpy(names[at], names[at - 1U], sizeof(names[at]));
          at--;
        }
        /* is_module_name() checked that the name fits. */
        memcpy(names[at], name, strlen(name) + 1U);
      }
    }
  }
  if (fib != NULL) {
    FreeDosObject(DOS_FIB, fib);
  }
  UnLock(lock);

  for (i = 0U; i < found; i++) {
    const ZZPlayProvider *provider;
    ZZPlayProviderCheck check;
    const char *reason = NULL;
    BPTR segment;

    (void)snprintf(path, sizeof(path), "PROGDIR:Providers/%s", names[i]);
    segment = LoadSeg((CONST_STRPTR)path);
    if (!segment) {
      reason = "cannot be loaded";
    } else {
      /* The first long word of a segment links to the next one; code
       * starts after it. */
      ZZPlayProviderEntry entry =
          (ZZPlayProviderEntry)((ULONG *)BADDR(segment) + 1);

      provider = entry();
      check = zzplay_provider_check(provider);
      if (check != ZZPLAY_PROVIDER_VALID) {
        reason = zzplay_provider_check_text(check);
      } else if (!zzplay_providers_add(set, names[i], provider,
                                       (void *)segment, &reason)) {
        /* reason set by add */
      } else {
        loaded++;
        continue;
      }
      UnLoadSeg(segment);
    }
    if (why != NULL && why_size != 0U && why[0] == '\0') {
      (void)snprintf(why, why_size, "%s: %s", names[i], reason);
    }
  }
  return loaded;
}

void zzplay_providers_unload(ZZPlayProviders *set)
{
  uint32_t i;

  for (i = 0U; i < set->count; i++) {
    if (set->slot[i].segment != NULL) {
      UnLoadSeg((BPTR)set->slot[i].segment);
    }
  }
  set->count = 0U;
}

#endif
