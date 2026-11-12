/* ZZPlay's side of the service-module host table (zzplay-provider.h):
 * the services handed to a module, bound to one module and one process,
 * and the same settings storage for ZZPlay's own windows.
 *
 * Module settings live in ENV:ZZPlay/<scheme>.prefs (this session) and
 * ENVARC:ZZPlay/<scheme>.prefs (saved) as KEY=VALUE lines, the
 * conventions of ZZPlay.prefs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_HOST_H
#define ZZPLAY_HOST_H

#include <stdint.h>

#include "zzplay-provider.h"
#include "zzplay-providers.h"

typedef struct ZZPlayHostBinding {
  ZZPlayHost table;  /* first member: a module only ever sees this */
  char scheme[ZZPLAY_PROVIDER_SCHEME_MAX];
  /* Polled at least every 100 ms during the module's transfers; return 0
   * to cancel (the user pressed Stop). NULL never cancels. */
  int (*keep_running)(void *user);
  void *user;
  /* Told what each of the module's transfers is waiting for (see
   * ZZPlayNetStage in zzplay-net.h; an int here so host tests need no
   * network headers). NULL reports nothing. Set after zzplay_host_bind. */
  void (*stage)(void *user, int stage, const char *host);
} ZZPlayHostBinding;

/* Fill `binding` for the module serving `scheme`. A binding belongs to the
 * process that calls the module through it: network transfers run in that
 * process. */
void zzplay_host_bind(ZZPlayHostBinding *binding, const char *scheme,
                      int (*keep_running)(void *user), void *user);

/* Module settings, for ZZPlay's settings form and playlist resolution.
 * get returns 0 and "" for a missing key; set stores the value in ENV:
 * and ENVARC:, returning 0 on failure. */
int zzplay_host_pref_get(const char *scheme, const char *key, char *out,
                         uint32_t size);
int zzplay_host_pref_set(const char *scheme, const char *key,
                         const char *value);

/* Percent-encode `in` for a URL query component (unreserved characters
 * stay, everything else becomes %XX). Returns the length written, or 0
 * when it does not fit in `size` with its terminator. */
uint32_t zzplay_host_url_escape(const char *in, char *out, uint32_t size);

/* Pure text helpers for KEY=VALUE preferences parsing and updating.
 * Keys contain letters, digits, and '_'. Values may contain '=' and spaces. */
int zzplay_host_pref_text_get(const char *text, const char *key, char *out,
                              uint32_t size);
int zzplay_host_pref_text_set(const char *in_text, const char *key,
                              const char *value, char *out, uint32_t out_size);

#endif /* ZZPLAY_HOST_H */
