/* ZZPlay's service modules: checking a module's descriptor and keeping
 * the set of loaded modules.
 *
 * The descriptor check and the registry are host-tested; finding and
 * loading module files is AmigaOS-only.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PROVIDERS_H
#define ZZPLAY_PROVIDERS_H

#include <stdint.h>

#include "zzplay-provider.h"

#define ZZPLAY_PROVIDERS_MAX 16U
#define ZZPLAY_PROVIDER_FILE_MAX 64U
#define ZZPLAY_PROVIDER_SCHEME_MAX 16U

typedef enum ZZPlayProviderCheck {
  ZZPLAY_PROVIDER_VALID = 0,
  ZZPLAY_PROVIDER_NOT_A_MODULE,  /* wrong id: not a ZZPlay module */
  ZZPLAY_PROVIDER_OTHER_ABI,     /* built for another interface version */
  ZZPLAY_PROVIDER_TOO_SMALL,     /* descriptor shorter than this ABI's */
  ZZPLAY_PROVIDER_INCOMPLETE,    /* a required call or name is missing */
  ZZPLAY_PROVIDER_BAD_SCHEME     /* scheme not 1-15 lower-case letters */
} ZZPlayProviderCheck;

/* Check a descriptor before calling anything in it. */
ZZPlayProviderCheck zzplay_provider_check(const ZZPlayProvider *provider);
const char *zzplay_provider_check_text(ZZPlayProviderCheck check);

typedef struct ZZPlayProviderSlot {
  char file[ZZPLAY_PROVIDER_FILE_MAX];   /* module file name, for messages */
  const ZZPlayProvider *provider;
  void *segment;                          /* the loaded hunks (AmigaOS) */
} ZZPlayProviderSlot;

typedef struct ZZPlayProviders {
  uint32_t count;
  ZZPlayProviderSlot slot[ZZPLAY_PROVIDERS_MAX];
} ZZPlayProviders;

/* Add a module whose descriptor passed the check. Returns 1, or 0 when
 * another module already serves the scheme (the earlier one stays) or the
 * set is full; `why` then names the reason. */
int zzplay_providers_add(ZZPlayProviders *set, const char *file,
                         const ZZPlayProvider *provider, void *segment,
                         const char **why);

/* The module serving `scheme`, or NULL. */
const ZZPlayProviderSlot *zzplay_providers_find(const ZZPlayProviders *set,
                                                const char *scheme);

/* The scheme of a "<scheme>://<id>" playlist entry when a module serves
 * it, copied to `scheme`, with the id after "://" in `*id`. Returns the
 * module's slot, or NULL for any other path. */
const ZZPlayProviderSlot *zzplay_providers_for_uri(const ZZPlayProviders *set,
                                                   const char *uri,
                                                   const char **id);

#if defined(__amigaos__) || defined(AMIGA)
/* Load every PROGDIR:Providers/#?.zzprov in name order. Modules that fail
 * the check are unloaded, and the first such message is copied to `why`
 * (empty when all loaded). Returns the number loaded. */
uint32_t zzplay_providers_load(ZZPlayProviders *set, char *why,
                               uint32_t why_size);
void zzplay_providers_unload(ZZPlayProviders *set);
#endif

#endif /* ZZPLAY_PROVIDERS_H */
