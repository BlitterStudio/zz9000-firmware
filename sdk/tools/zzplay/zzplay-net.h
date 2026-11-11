/* HTTP client over bsdsocket.library for ZZPlay's network sources.
 *
 * One connection carries one GET and its response. Each connection opens
 * its own bsdsocket.library base, because a base belongs to the process
 * that opened it: a connection must be used only by the process that
 * created it. Sockets are non-blocking and every wait polls the caller's
 * keep-running hook at least every 100 ms, so Stop and Quit take effect
 * promptly. Host-name lookups are the exception: bsdsocket has only a
 * blocking resolver.
 *
 * AmigaOS only. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_NET_H
#define ZZPLAY_NET_H

#include <stddef.h>
#include <stdint.h>

#include "zzplay-http.h"

/* Room for the longest message plus a ZZPLAY_HTTP_HOST_MAX host name. */
#define ZZPLAY_NET_ERROR_MAX 192U

typedef struct ZZPlayNet ZZPlayNet;

/* Return 0 to cancel the transfer. */
typedef int (*ZZPlayNetKeepRunning)(void *user);

typedef struct ZZPlayNetRequest {
  const char *url;
  /* Complete header lines ending in CRLF, or NULL. They can carry service
   * credentials, so they are sent only while redirects stay on the
   * origin (scheme, host and port) of `url`. */
  const char *extra_headers;
  int has_range;
  uint32_t range_from;
  int icy;                          /* ask for Shoutcast metadata */
  ZZPlayNetKeepRunning keep_running; /* may be NULL */
  void *user;
} ZZPlayNetRequest;

/* Send the request, follow up to five redirects (never from HTTPS to
 * HTTP), and return the connection positioned at the final response body.
 * `head` receives the final response head, also on failure when one
 * arrived. A status outside 200-299 is a failure. On failure returns NULL
 * and a one-line reason in `err`; `*cancelled` (when not NULL) tells a
 * cancel apart from an error. */
ZZPlayNet *zzplay_net_get(const ZZPlayNetRequest *req, ZZPlayHttpHead *head,
                          char *err, size_t err_size, int *cancelled);

/* Read body bytes with any chunked framing removed. Returns the count,
 * 0 at the end of the body, or -1 on failure or cancel. */
long zzplay_net_read(ZZPlayNet *net, uint8_t *buf, size_t size);

/* The reason for the last -1 from zzplay_net_read(). */
const char *zzplay_net_error(const ZZPlayNet *net);
int zzplay_net_cancelled(const ZZPlayNet *net);

/* The URL that produced the body, after redirects. */
const char *zzplay_net_url(const ZZPlayNet *net);

/* Close the socket and free the connection. NULL is ignored. */
void zzplay_net_close(ZZPlayNet *net);

#endif /* ZZPLAY_NET_H */
