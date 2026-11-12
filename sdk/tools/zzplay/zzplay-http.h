/* HTTP/1.1 and Shoutcast protocol parsing for ZZPlay's network sources:
 * URLs, requests, response heads, chunked bodies and ICY metadata.
 *
 * Pure byte handling with no I/O, so it is host-tested. Derived from the
 * ZZTop updater's parser (zz9000-drivers common/fw_release.c), extended
 * for plain HTTP, ports, byte ranges and Shoutcast/Icecast streams.
 *
 * Sizes from the network are 32-bit on the Amiga: every bound check here
 * subtracts rather than adds, so a hostile length cannot wrap.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_HTTP_H
#define ZZPLAY_HTTP_H

#include <stddef.h>
#include <stdint.h>

#define ZZPLAY_HTTP_HOST_MAX 128U
#define ZZPLAY_HTTP_PATH_MAX 1024U
#define ZZPLAY_HTTP_URL_MAX (ZZPLAY_HTTP_HOST_MAX + ZZPLAY_HTTP_PATH_MAX + 16U)
#define ZZPLAY_HTTP_NAME_MAX 96U
#define ZZPLAY_HTTP_TITLE_MAX 128U

typedef struct ZZPlayUrl {
  int tls;                           /* https:// */
  uint16_t port;                     /* 80/443 unless given */
  char host[ZZPLAY_HTTP_HOST_MAX];
  char path[ZZPLAY_HTTP_PATH_MAX];   /* starts with '/', includes ?query */
} ZZPlayUrl;

/* True for "http://" and "https://" (any case). */
int zzplay_http_is_url(const char *text);

/* Split an http or https URL. Host names and IPv4 literals are accepted;
 * user info, IPv6 literals, other schemes and control characters are not.
 * A missing path becomes "/". Returns 1 on success. */
int zzplay_http_parse_url(const char *url, ZZPlayUrl *out);

/* Resolve a redirect target against the URL that produced it: absolute
 * http(s) URLs pass through, "//host/x" keeps the scheme, "/x" keeps
 * scheme, host and port, and "x" replaces the last path segment. Returns
 * 1 when the result fits in `out`. */
int zzplay_http_resolve(const ZZPlayUrl *base, const char *location,
                        char *out, size_t size);

/* A redirect may be followed unless it downgrades HTTPS to HTTP. */
int zzplay_http_redirect_allowed(const ZZPlayUrl *from, const ZZPlayUrl *to);

/* Same scheme, host (ignoring case) and port. Caller headers, which can
 * carry service credentials, follow a redirect only within one origin. */
int zzplay_http_same_origin(const ZZPlayUrl *a, const ZZPlayUrl *b);

/* Format an HTTP request. `method` defaults to "GET" when NULL. For a
 * body-carrying method (or when body_len > 0), a Content-Length header is
 * added, including Content-Length: 0. `range_from` asks for "bytes=N-" when
 * has_range is set; `icy` asks a Shoutcast/Icecast server to interleave
 * metadata; `extra` holds complete header lines, each ending in CRLF, or is
 * NULL. Returns the request length, or 0 when it does not fit in `size`. */
size_t zzplay_http_format_request(char *out, size_t size, const char *method,
                                  const ZZPlayUrl *url, const char *user_agent,
                                  int has_range, uint32_t range_from, int icy,
                                  size_t body_len, const char *extra);

typedef struct ZZPlayHttpHead {
  int status;                 /* 200, 206, 302, ... */
  int icy;                    /* the status line was "ICY 200 OK" */
  int has_length;             /* Content-Length present */
  uint32_t content_length;
  int chunked;                /* Transfer-Encoding: chunked */
  int accept_ranges;          /* Accept-Ranges: bytes */
  int has_range;              /* Content-Range present */
  uint32_t range_start;       /* first byte of a 206 body */
  int has_total;              /* Content-Range named the full size */
  uint32_t total;
  uint32_t icy_metaint;       /* 0 when the stream has no metadata */
  char icy_name[ZZPLAY_HTTP_NAME_MAX];
  char content_type[64];
  char location[ZZPLAY_HTTP_URL_MAX];
} ZZPlayHttpHead;

/* Parse a complete response head without its final blank line. Accepts
 * "HTTP/1.x NNN" and Shoutcast's "ICY NNN" status lines. A number too
 * large for 32 bits or a transfer coding other than chunked fails the
 * parse. Returns 1 on success. */
int zzplay_http_parse_head(const char *head, size_t len, ZZPlayHttpHead *out);

/* Incremental Transfer-Encoding: chunked decoder. */
typedef struct ZZPlayChunked {
  int state;
  uint32_t remaining;
  int digits;
} ZZPlayChunked;

typedef int (*ZZPlayHttpSink)(void *ctx, const uint8_t *data, size_t len);

#define ZZPLAY_CHUNKED_MORE 0
#define ZZPLAY_CHUNKED_DONE 1
#define ZZPLAY_CHUNKED_ERROR -1

void zzplay_chunked_init(ZZPlayChunked *c);
/* Feed body bytes; the sink receives the de-chunked data and returns 0 to
 * continue. Returns MORE, DONE (terminal chunk and trailers seen) or ERROR
 * (malformed framing or the sink refused). `used` receives the bytes of
 * `data` consumed, which is less than `len` only after DONE. */
int zzplay_chunked_feed(ZZPlayChunked *c, const uint8_t *data, size_t len,
                        ZZPlayHttpSink sink, void *ctx, size_t *used);

/* Shoutcast/Icecast metadata removal. Every `interval` audio bytes the
 * server inserts one length byte (x16) and that many metadata bytes. */
typedef struct ZZPlayIcy {
  uint32_t interval;
  uint32_t audio_left;        /* audio bytes before the next length byte */
  uint32_t meta_left;         /* metadata bytes still to skip */
  uint32_t meta_len;          /* bytes collected of the current block */
  char meta[16U * 255U + 1U];
  char title[ZZPLAY_HTTP_TITLE_MAX];
  int title_changed;          /* set when `title` changes; caller clears */
} ZZPlayIcy;

void zzplay_icy_init(ZZPlayIcy *icy, uint32_t interval);
/* Remove metadata from `len` bytes in place and return how many audio
 * bytes remain at the start of `buf`. Feed splits may fall anywhere. */
size_t zzplay_icy_strip(ZZPlayIcy *icy, uint8_t *buf, size_t len);

/* Extract StreamTitle='...'; from a metadata block. Returns 1 and the
 * title (truncated to `size`) when one is present and non-empty. */
int zzplay_icy_stream_title(const char *meta, size_t len, char *out,
                            size_t size);

#endif /* ZZPLAY_HTTP_H */
