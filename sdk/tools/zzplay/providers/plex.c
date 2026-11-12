/*
 * Plex Media Server provider for ZZPlay.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <stdint.h>

#include "../zzplay-provider.h"

#define PLEX_SCHEME "plex"
#define PLEX_NAME "Plex Media Server"
#define PLEX_PRODUCT "ZZPlay"
#define PLEX_VERSION "0.6"
#define PLEX_PLATFORM "Generic"
#define PLEX_DEFAULT_BITRATE 192U

#define PLEX_MAX_SECTIONS 16U
#define PLEX_SECTION_KEY_MAX 32U
#define PLEX_SECTION_TITLE_MAX 64U

typedef struct PlexSection {
  char key[PLEX_SECTION_KEY_MAX];
  char title[PLEX_SECTION_TITLE_MAX];
} PlexSection;

typedef struct PlexResolveXmlState {
  const ZZPlayHost *host;
  char title[96];
  char artist[96];
  char duration[32];
  char codec[32];
  char part_key[256];
  int found_track;
} PlexResolveXmlState;

#define PLEX_MAX_DISCOVERED_SERVERS 8U
#define PLEX_MAX_CONNECTIONS_PER_SERVER 4U

typedef struct PlexDiscoveredConnection {
  char uri[256];
  char address[64];
  char port[16];
  int local;
  int relay;
  int is_https;
} PlexDiscoveredConnection;

typedef struct PlexDiscoveredServer {
  char name[64];
  char access_token[128];
  int owned;
  PlexDiscoveredConnection connections[PLEX_MAX_CONNECTIONS_PER_SERVER];
  uint32_t connection_count;
} PlexDiscoveredServer;

typedef struct PlexPinXmlState {
  const ZZPlayHost *host;
  char id[32];
  char code[32];
  char auth_token[128];
  char expires_in[32];
} PlexPinXmlState;

typedef struct PlexContext {
  const ZZPlayHost *host;
  char server[256];
  char token[128];
  char client_id[64];
  uint32_t bitrate;
  uint32_t session_counter;
  PlexSection sections[PLEX_MAX_SECTIONS];
  uint32_t section_count;
  int sections_loaded;
  PlexDiscoveredServer discovered_servers[PLEX_MAX_DISCOVERED_SERVERS];
  uint32_t discovered_server_count;
  char scratch[896];
  PlexResolveXmlState rst;
} PlexContext;

typedef struct PlexResourcesXmlState {
  const ZZPlayHost *host;
  PlexContext *ctx;
  int in_server_resource;
} PlexResourcesXmlState;

typedef struct PlexXmlFeedState {
  const ZZPlayHost *host;
  void *xml;
} PlexXmlFeedState;

typedef struct PlexSectionsXmlState {
  const ZZPlayHost *host;
  PlexContext *ctx;
} PlexSectionsXmlState;

/* What a list request asks Plex for. */
#define PLEX_FETCH_ITEMS 0   /* artist, album and track rows */
#define PLEX_FETCH_LETTERS 1 /* firstCharacter letter folders */
#define PLEX_FETCH_GENRES 2  /* genre folders */
#define PLEX_FETCH_COUNT 3   /* size-0 request: only the total */

typedef struct PlexItemXmlState {
  const PlexContext *ctx;
  const ZZPlayItemSink *sink;
  int stopped;
  int mode;
  const char *folder_prefix; /* id prefix for the folder modes */
  int report_total;
  uint32_t total_bias;
  uint32_t count_total;      /* PLEX_FETCH_COUNT: totalSize seen */
} PlexItemXmlState;

typedef struct PlexListOpts {
  const char *path;
  uint32_t start;
  uint32_t count;            /* 0: no window */
  int paged;                 /* send the container headers */
  int mode;                  /* PLEX_FETCH_* */
  const char *folder_prefix; /* PLEX_FETCH_LETTERS/GENRES */
  int report_total;
  uint32_t total_bias;
} PlexListOpts;

/* --- Freestanding helpers --- */

static size_t plex_strlen(const char *s)
{
  size_t n = 0;
  if (!s) return 0;
  while (s[n]) n++;
  return n;
}

static int plex_strcmp(const char *a, const char *b)
{
  if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
  while (*a && (*a == *b)) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int plex_strncmp(const char *a, const char *b, size_t n)
{
  if (!n) return 0;
  if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
  while (--n && *a && (*a == *b)) {
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int plex_strcasecmp(const char *a, const char *b)
{
  if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
    if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    a++;
    b++;
  }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static const char *plex_strstr(const char *haystack, const char *needle)
{
  size_t nlen;
  if (!haystack || !needle) return NULL;
  nlen = plex_strlen(needle);
  if (nlen == 0U) return haystack;
  while (*haystack) {
    if (*haystack == *needle && plex_strncmp(haystack, needle, nlen) == 0) {
      return haystack;
    }
    haystack++;
  }
  return NULL;
}


static void plex_memcpy(void *dst, const void *src, size_t n)
{
  char *d = (char *)dst;
  const char *s = (const char *)src;
  while (n--) *d++ = *s++;
}

static void plex_memset(void *dst, int c, size_t n)
{
  char *d = (char *)dst;
  while (n--) *d++ = (char)c;
}

static size_t plex_strlcpy(char *dst, const char *src, size_t size)
{
  size_t srclen = plex_strlen(src);
  if (size > 0) {
    size_t copy = (srclen < size - 1) ? srclen : (size - 1);
    plex_memcpy(dst, src, copy);
    dst[copy] = '\0';
  }
  return srclen;
}

static uint32_t plex_parse_uint(const char *s)
{
  uint32_t val = 0;
  if (!s) return 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10U + (uint32_t)(*s - '0');
    s++;
  }
  return val;
}

static int plex_is_direct_codec(const char *codec)
{
  if (!codec) return 0;
  if (plex_strcasecmp(codec, "mp3") == 0) return 1;
  if (plex_strcasecmp(codec, "flac") == 0) return 1;
  if (plex_strcasecmp(codec, "vorbis") == 0) return 1;
  return 0;
}

/* Bytes that are safe to paste from a folder id into a request path:
 * no separators, spaces or controls. */
static int plex_is_safe_token(const char *s)
{
  if (!s || s[0] == '\0') return 0;
  while (*s) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20U || c == 0x7FU) return 0;
    if (c == ' ' || c == '?' || c == '#' || c == '&' || c == '/' || c == '"') {
      return 0;
    }
    s++;
  }
  return 1;
}

/* Longest ratingKey taken from a reply: "art:<key>" and friends fit a
 * 48-byte id. */
#define PLEX_KEY_MAX 40U

/* A ratingKey: short, and only characters that need no escaping in a
 * request path or an id. */
static int plex_is_key(const char *s)
{
  size_t n;

  if (!s) return 0;
  for (n = 0U; s[n] != '\0'; n++) {
    char c = s[n];
    if (n + 1U >= PLEX_KEY_MAX) return 0;
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.')) {
      return 0;
    }
  }
  return n != 0U;
}

/* Length of the first `len` bytes of `s` without a UTF-8 sequence that a
 * cut at `len` left incomplete. */
static size_t plex_utf8_cut(const char *s, size_t len)
{
  size_t i = len;
  size_t back = 0U;

  while (i > 0U && back < 3U && ((unsigned char)s[i - 1U] & 0xC0U) == 0x80U) {
    i--;
    back++;
  }
  if (i > 0U) {
    unsigned char lead = (unsigned char)s[i - 1U];
    size_t need = lead >= 0xF0U ? 4U : lead >= 0xE0U ? 3U : lead >= 0xC0U ? 2U : 1U;

    if (need > 1U && back + 1U < need) {
      return i - 1U;
    }
  }
  return len;
}

/* Copy text from a reply for showing: control characters become spaces
 * and a longer text is cut without splitting a UTF-8 sequence. */
static void plex_copy_text(char *out, size_t size, const char *src)
{
  size_t len = 0U;
  size_t i;

  if (size == 0U) return;
  if (!src) src = "";
  while (len < size - 1U && src[len] != '\0') {
    len++;
  }
  if (src[len] != '\0') {
    len = plex_utf8_cut(src, len);
  }
  for (i = 0U; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    out[i] = (c < 0x20U || c == 0x7FU) ? ' ' : (char)c;
  }
  out[len] = '\0';
}

#define PLEX_TOKEN_REJECTED \
  "Plex rejected the token; clear Token in Settings to sign in again"
#define PLEX_SIGNIN_REJECTED "plex.tv rejected the sign-in. Start sign-in again."

/* The outcome of a request to the Plex server: 401 means the token is no
 * longer accepted, any other status but 200 is named, and a transfer
 * that failed keeps the host's reason (or gets one). */
static int plex_reply_ok(const ZZPlayHost *host, int ok, int status,
                         char *err, uint32_t err_size)
{
  if (status == 401) {
    host->format(err, err_size, "%s", PLEX_TOKEN_REJECTED);
    return 0;
  }
  if (status != 200 && status != 0) {
    host->format(err, err_size, "Plex answered HTTP %d", status);
    return 0;
  }
  if (!ok) {
    if (err && err_size > 0U && err[0] == '\0') {
      host->format(err, err_size, "The Plex server did not answer");
    }
    return 0;
  }
  return 1;
}

#define PLEX_UNREADABLE "Plex sent an unreadable reply"

/* Close the scanner of a reply. Returns 1 when the reply was a whole
 * document; a host older than xml_finish cannot tell, so its replies
 * count as whole. */
static int plex_xml_done(const ZZPlayHost *host, void *xml)
{
  int whole = 1;

  if (host->size >= (uint32_t)(offsetof(ZZPlayHost, xml_finish) +
                               sizeof(host->xml_finish)) &&
      host->xml_finish != NULL) {
    whole = host->xml_finish(host, xml);
  }
  host->xml_close(host, xml);
  return whole;
}

/* Close the scanner and judge a Plex server reply: plex_reply_ok, then a
 * document cut short is unreadable. */
static int plex_finish_reply(const ZZPlayHost *host, void *xml, int ok,
                             int status, char *err, uint32_t err_size)
{
  int whole = plex_xml_done(host, xml);

  if (!plex_reply_ok(host, ok, status, err, err_size)) {
    return 0;
  }
  if (!whole) {
    host->format(err, err_size, "%s", PLEX_UNREADABLE);
    return 0;
  }
  return 1;
}

/* Recognises "https://A-B-C-D.<hash>.plex.direct:<port>" and writes
 * "http://A.B.C.D:<port>" into out. IPv4 addresses only; anything else
 * returns 0 and leaves out untouched. */
static int plex_plexdirect_plain(const ZZPlayHost *host, const char *url,
                                 char *out, uint32_t out_size)
{
  static const char suffix[] = ".plex.direct";
  const char *hp;
  const char *port;
  const char *p;
  const char *quad_end;
  uint32_t octet[4];
  uint32_t octets;
  uint32_t digits;
  uint32_t port_val;
  uint32_t i;
  size_t host_len;
  size_t suffix_len = sizeof(suffix) - 1U;

  if (!host || !url || out_size == 0U) {
    return 0;
  }
  if (plex_strncmp(url, "https://", 8) != 0) {
    return 0;
  }

  /* The host runs to the port, a path or the end of the address */
  hp = url + 8;
  host_len = 0U;
  while (hp[host_len] != '\0' && hp[host_len] != ':' && hp[host_len] != '/') {
    host_len++;
  }
  port = hp + host_len;
  if (*port != ':') {
    return 0; /* a plex.direct address always carries its port */
  }
  port++;
  digits = 0U;
  port_val = 0U;
  p = port;
  while (*p >= '0' && *p <= '9') {
    port_val = port_val * 10U + (uint32_t)(*p - '0');
    p++;
    digits++;
  }
  if (digits == 0U || digits > 5U || (*p != '\0' && *p != '/')) {
    return 0;
  }

  /* The host is A-B-C-D.<hash>.plex.direct */
  if (host_len < suffix_len + 9U) {
    return 0; /* shortest quad, the separator and a one-char hash */
  }
  for (i = 0U; (size_t)i < suffix_len; i++) {
    char c1 = hp[host_len - suffix_len + (size_t)i];
    char c2 = suffix[i];
    c1 = (c1 >= 'A' && c1 <= 'Z') ? (char)(c1 + 32) : c1;
    if (c1 != c2) {
      return 0;
    }
  }

  /* Find the dot between the address and the hash; the hash needs at
   * least one character. */
  i = (uint32_t)(host_len - suffix_len);
  if (i < 2U) {
    return 0;
  }
  i--;
  while (i > 0U && hp[i] != '.') {
    i--;
  }
  if (i == 0U || hp[i] != '.') {
    return 0;
  }
  if (host_len - suffix_len - (size_t)i < 2U) {
    return 0;
  }
  quad_end = hp + (size_t)i;

  /* The address itself: four 0-255 groups separated by dashes */
  p = hp;
  octets = 0U;
  while (p < quad_end) {
    uint32_t v = 0U;
    digits = 0U;
    while (p < quad_end && *p >= '0' && *p <= '9') {
      v = v * 10U + (uint32_t)(*p - '0');
      p++;
      digits++;
    }
    if (digits == 0U || digits > 3U || v > 255U) {
      return 0;
    }
    if (octets < 4U) {
      octet[octets] = v;
    }
    octets++;
    if (p < quad_end) {
      if (*p != '-') {
        return 0;
      }
      p++;
      if (p == quad_end) {
        return 0; /* the address cannot end on a dash */
      }
    }
  }
  if (p != quad_end || octets != 4U) {
    return 0;
  }

  host->format(out, out_size, "http://%lu.%lu.%lu.%lu:%lu",
               (unsigned long)octet[0], (unsigned long)octet[1],
               (unsigned long)octet[2], (unsigned long)octet[3],
               (unsigned long)port_val);
  return 1;
}

/* Returns 0 when the address does not fit: a cut one asks for another
 * resource. */
static int plex_build_url(const PlexContext *ctx, const char *path, char *out, uint32_t size)
{
  int n;

  if (!ctx || !path || !out || size == 0) return 0;
  if (path[0] == 'h' && path[1] == 't' && path[2] == 't' && path[3] == 'p') {
    return plex_strlcpy(out, path, size) < size;
  }
  if (path[0] == '/') {
    n = ctx->host->format(out, size, "%s%s", ctx->server, path);
  } else {
    n = ctx->host->format(out, size, "%s/%s", ctx->server, path);
  }
  return n >= 0 && (uint32_t)n < size;
}

static int plex_format_headers(const PlexContext *ctx, uint32_t start, uint32_t count,
                               int paged, char *out, uint32_t size)
{
  if (paged) {
    return ctx->host->format(out, size,
      "X-Plex-Token: %s\r\n"
      "X-Plex-Client-Identifier: %s\r\n"
      "X-Plex-Product: %s\r\n"
      "X-Plex-Version: %s\r\n"
      "X-Plex-Platform: %s\r\n"
      "X-Plex-Container-Start: %lu\r\n"
      "X-Plex-Container-Size: %lu\r\n",
      ctx->token, ctx->client_id, PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM,
      (unsigned long)start, (unsigned long)count);
  }
  return ctx->host->format(out, size,
    "X-Plex-Token: %s\r\n"
    "X-Plex-Client-Identifier: %s\r\n"
    "X-Plex-Product: %s\r\n"
    "X-Plex-Version: %s\r\n"
    "X-Plex-Platform: %s\r\n",
    ctx->token, ctx->client_id, PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM);
}

static int plex_xml_data_cb(void *user, const uint8_t *bytes, uint32_t len)
{
  PlexXmlFeedState *st = (PlexXmlFeedState *)user;
  return st->host->xml_feed(st->host, st->xml, (const char *)bytes, len);
}

static int plex_discard_data_cb(void *user, const uint8_t *bytes, uint32_t len)
{
  (void)user;
  (void)bytes;
  (void)len;
  return 0;
}
/* --- Sign-in XML parsing (PIN and Resources) --- */

static int plex_pin_xml_start(void *user, const char *name,
                              const ZZPlayHostXmlAttr *attrs,
                              uint32_t count, int truncated)
{
  PlexPinXmlState *st = (PlexPinXmlState *)user;
  (void)truncated;
  if (plex_strcmp(name, "pin") == 0) {
    const char *id = st->host->xml_attr(attrs, count, "id");
    const char *code = st->host->xml_attr(attrs, count, "code");
    const char *tok = st->host->xml_attr(attrs, count, "authToken");
    const char *exp = st->host->xml_attr(attrs, count, "expiresIn");
    /* The id goes into a request path and the code is shown: anything
     * odd counts as missing. A token that is not a plain token would
     * break the header it is sent in. */
    if (plex_is_key(id) && plex_strlen(id) < sizeof(st->id)) {
      plex_strlcpy(st->id, id, sizeof(st->id));
    }
    if (plex_is_key(code) && plex_strlen(code) < sizeof(st->code)) {
      plex_strlcpy(st->code, code, sizeof(st->code));
    }
    if (plex_is_safe_token(tok) && plex_strlen(tok) < sizeof(st->auth_token)) {
      plex_strlcpy(st->auth_token, tok, sizeof(st->auth_token));
    }
    if (exp) plex_strlcpy(st->expires_in, exp, sizeof(st->expires_in));
  }
  return 0;
}

static int plex_resources_xml_start(void *user, const char *name,
                                    const ZZPlayHostXmlAttr *attrs,
                                    uint32_t count, int truncated)
{
  PlexResourcesXmlState *st = (PlexResourcesXmlState *)user;
  const ZZPlayHost *host = st->host;
  (void)truncated;

  if (plex_strcmp(name, "resource") == 0) {
    const char *provides = host->xml_attr(attrs, count, "provides");
    if (provides && plex_strstr(provides, "server") != NULL) {
      if (st->ctx->discovered_server_count < PLEX_MAX_DISCOVERED_SERVERS) {
        PlexDiscoveredServer *srv = &st->ctx->discovered_servers[st->ctx->discovered_server_count];
        const char *rname = host->xml_attr(attrs, count, "name");
        const char *tok = host->xml_attr(attrs, count, "accessToken");
        const char *owned = host->xml_attr(attrs, count, "owned");

        plex_memset(srv, 0, sizeof(*srv));
        if (rname) {
          plex_copy_text(srv->name, sizeof(srv->name), rname);
        } else {
          plex_strlcpy(srv->name, "Plex Media Server", sizeof(srv->name));
        }
        if (plex_is_safe_token(tok) && plex_strlen(tok) < sizeof(srv->access_token)) {
          plex_strlcpy(srv->access_token, tok, sizeof(srv->access_token));
        }
        if (owned && plex_strcmp(owned, "1") == 0) {
          srv->owned = 1;
        } else {
          srv->owned = 0;
        }
        st->in_server_resource = 1;
      } else {
        st->in_server_resource = 0;
      }
    } else {
      st->in_server_resource = 0;
    }
  } else if (plex_strcmp(name, "connection") == 0 && st->in_server_resource) {
    const char *local = host->xml_attr(attrs, count, "local");
    const char *relay = host->xml_attr(attrs, count, "relay");
    int is_local = (local && plex_strcmp(local, "1") == 0);
    int is_relay = (relay && plex_strcmp(relay, "1") == 0);

    if (is_local && !is_relay) {
      PlexDiscoveredServer *srv = &st->ctx->discovered_servers[st->ctx->discovered_server_count];
      if (srv->connection_count < PLEX_MAX_CONNECTIONS_PER_SERVER) {
        PlexDiscoveredConnection *conn = &srv->connections[srv->connection_count++];
        const char *proto = host->xml_attr(attrs, count, "protocol");
        const char *addr = host->xml_attr(attrs, count, "address");
        const char *port = host->xml_attr(attrs, count, "port");
        const char *uri = host->xml_attr(attrs, count, "uri");

        conn->local = 1;
        conn->relay = 0;
        if (proto && plex_strcmp(proto, "https") == 0) {
          conn->is_https = 1;
        } else if (uri && plex_strncmp(uri, "https://", 8) == 0) {
          conn->is_https = 1;
        } else {
          conn->is_https = 0;
        }
        if (addr) plex_strlcpy(conn->address, addr, sizeof(conn->address));
        if (port) plex_strlcpy(conn->port, port, sizeof(conn->port));
        if (uri) plex_strlcpy(conn->uri, uri, sizeof(conn->uri));
      }
    }
  }
  return 0;
}

static int plex_resources_xml_end(void *user, const char *name)
{
  PlexResourcesXmlState *st = (PlexResourcesXmlState *)user;
  if (plex_strcmp(name, "resource") == 0) {
    if (st->in_server_resource) {
      st->ctx->discovered_server_count++;
      st->in_server_resource = 0;
    }
  }
  return 0;
}


/* --- Sections XML parsing --- */

static int plex_sections_xml_start(void *user, const char *name,
                                   const ZZPlayHostXmlAttr *attrs,
                                   uint32_t count, int truncated)
{
  PlexSectionsXmlState *st = (PlexSectionsXmlState *)user;
  (void)truncated;

  if (plex_strcmp(name, "Directory") == 0) {
    const char *type = st->host->xml_attr(attrs, count, "type");
    if (type && plex_strcmp(type, "artist") == 0) {
      const char *key = st->host->xml_attr(attrs, count, "key");
      const char *title = st->host->xml_attr(attrs, count, "title");
      /* The key goes into ids and request paths: a cut or odd one would
       * name another library. */
      if (plex_is_key(key) && plex_strlen(key) < PLEX_SECTION_KEY_MAX &&
          title && st->ctx->section_count < PLEX_MAX_SECTIONS) {
        PlexSection *sec = &st->ctx->sections[st->ctx->section_count++];
        plex_strlcpy(sec->key, key, sizeof(sec->key));
        plex_copy_text(sec->title, sizeof(sec->title), title);
      }
    }
  }
  return 0;
}

static int plex_load_sections(PlexContext *ctx, char *err, uint32_t err_size)
{
  char url[384];
  char headers[384];
  PlexSectionsXmlState xst;
  ZZPlayHostXmlHandler h;
  void *xml;
  PlexXmlFeedState feed;
  int status = 0;
  int ok;

  if (ctx->sections_loaded) return 1;

  if (ctx->server[0] == '\0') {
    ctx->host->format(err, err_size, "Set the server in Settings");
    return 0;
  }
  if (!plex_build_url(ctx, "/library/sections", url, sizeof(url))) {
    ctx->host->format(err, err_size, "Failed to build URL");
    return 0;
  }
  plex_format_headers(ctx, 0, 0, 0, headers, sizeof(headers));

  xst.host = ctx->host;
  xst.ctx = ctx;
  ctx->section_count = 0;

  plex_memset(&h, 0, sizeof(h));
  h.start = plex_sections_xml_start;
  h.user = &xst;

  xml = ctx->host->xml_open(ctx->host, &h);
  if (!xml) {
    ctx->host->format(err, err_size, "Out of memory");
    return 0;
  }

  feed.host = ctx->host;
  feed.xml = xml;

  ok = ctx->host->http_get(ctx->host, url, headers, plex_xml_data_cb,
                           &feed, &status, err, err_size);
  if (!plex_finish_reply(ctx->host, xml, ok, status, err, err_size)) {
    return 0;
  }

  ctx->sections_loaded = 1;
  return 1;
}

/* --- Item browsing and search XML parsing --- */

/* Hand one row to the sink. `playable` marks a collection ZZPlay can play
 * whole (an artist, an album, a playlist); letter, genre and library
 * folders pass 0. Title and subtitle come from the reply and are cleaned
 * and cut here. */
static int plex_emit(PlexItemXmlState *st, const char *id, const char *title,
                     const char *subtitle, uint32_t kind, int playable,
                     uint32_t duration_ms)
{
  ZZPlayProviderItem it;
  char title_buf[ZZPLAY_PROVIDER_TEXT_MAX];
  char sub_buf[ZZPLAY_PROVIDER_TEXT_MAX];

  plex_copy_text(title_buf, sizeof(title_buf), title);
  plex_copy_text(sub_buf, sizeof(sub_buf), subtitle);
  plex_memset(&it, 0, sizeof(it));
  it.id = id;
  it.title = title_buf;
  it.subtitle = sub_buf;
  it.duration_ms = duration_ms;
  it.kind = kind;
  it.playable = playable ? 1U : 0U;
  if (st->sink && st->sink->item(st->sink->user, &it) != 0) {
    st->stopped = 1;
    return 1;
  }
  return 0;
}

/* "<prefix><key>" into `id`. Returns 0 when it does not fit: a cut id
 * would open another folder. */
static int plex_make_id(const ZZPlayHost *host, char *id, uint32_t size,
                        const char *prefix, const char *key)
{
  int n = host->format(id, size, "%s%s", prefix, key);

  return n >= 0 && (uint32_t)n < size;
}

static int plex_item_xml_start(void *user, const char *name,
                               const ZZPlayHostXmlAttr *attrs,
                               uint32_t count, int truncated)
{
  PlexItemXmlState *st = (PlexItemXmlState *)user;
  const ZZPlayHost *host = st->ctx->host;
  /* Subtitles are put together here, then cleaned and cut by plex_emit. */
  char text[256];
  char id[ZZPLAY_PROVIDER_TEXT_MAX];
  (void)truncated;

  if (st->stopped) return 1;

  if (plex_strcmp(name, "MediaContainer") == 0) {
    const char *tot = host->xml_attr(attrs, count, "totalSize");
    if (!tot) {
      tot = host->xml_attr(attrs, count, "size");
    }
    if (tot) {
      if (st->mode == PLEX_FETCH_COUNT) {
        st->count_total = plex_parse_uint(tot);
      } else if (st->report_total && st->sink && st->sink->total) {
        st->sink->total(st->sink->user, plex_parse_uint(tot) + st->total_bias);
      }
    }
  } else if (plex_strcmp(name, "Directory") == 0) {
    if (st->mode == PLEX_FETCH_LETTERS) {
      const char *key = host->xml_attr(attrs, count, "key");
      const char *title = host->xml_attr(attrs, count, "title");
      const char *size = host->xml_attr(attrs, count, "size");

      /* The key is pasted into a request path when the folder opens. */
      if (!title || !plex_is_safe_token(key) ||
          !plex_make_id(host, id, sizeof(id), st->folder_prefix, key)) {
        return 0;
      }
      if (size && size[0] != '\0') {
        host->format(text, sizeof(text), "%s (%lu)", title,
                     (unsigned long)plex_parse_uint(size));
        title = text;
      }
      return plex_emit(st, id, title, "", ZZPLAY_ITEM_FOLDER, 0, 0U);
    }
    if (st->mode == PLEX_FETCH_GENRES) {
      const char *key = host->xml_attr(attrs, count, "key");
      const char *title = host->xml_attr(attrs, count, "title");

      if (!title || !plex_is_safe_token(key) ||
          !plex_make_id(host, id, sizeof(id), st->folder_prefix, key)) {
        return 0;
      }
      return plex_emit(st, id, title, "", ZZPLAY_ITEM_FOLDER, 0, 0U);
    }
    {
      const char *type = host->xml_attr(attrs, count, "type");
      const char *rk = host->xml_attr(attrs, count, "ratingKey");
      const char *title = host->xml_attr(attrs, count, "title");

      if (!title) title = "";
      /* Without a usable ratingKey the row could not be opened. */
      if (!plex_is_key(rk)) return 0;

      if (type && plex_strcmp(type, "artist") == 0) {
        (void)plex_make_id(host, id, sizeof(id), "art:", rk);
        return plex_emit(st, id, title, "", ZZPLAY_ITEM_FOLDER, 1, 0U);
      }
      if (type && plex_strcmp(type, "album") == 0) {
        const char *year = host->xml_attr(attrs, count, "year");
        const char *parent = host->xml_attr(attrs, count, "parentTitle");

        (void)plex_make_id(host, id, sizeof(id), "alb:", rk);
        text[0] = '\0';
        if (parent && parent[0] && year && year[0]) {
          host->format(text, sizeof(text), "%s - %s", parent, year);
        } else if (year && year[0]) {
          plex_strlcpy(text, year, sizeof(text));
        } else if (parent && parent[0]) {
          plex_strlcpy(text, parent, sizeof(text));
        }
        /* An album is a collection. */
        return plex_emit(st, id, title, text, ZZPLAY_ITEM_FOLDER, 1, 0U);
      }
    }
  } else if (plex_strcmp(name, "Playlist") == 0) {
    const char *rk = host->xml_attr(attrs, count, "ratingKey");
    const char *title = host->xml_attr(attrs, count, "title");

    if (!plex_is_key(rk)) return 0;
    (void)plex_make_id(host, id, sizeof(id), "pl:", rk);
    return plex_emit(st, id, title ? title : "", "", ZZPLAY_ITEM_FOLDER, 1, 0U);
  } else if (plex_strcmp(name, "Track") == 0) {
    const char *rk = host->xml_attr(attrs, count, "ratingKey");
    const char *title = host->xml_attr(attrs, count, "title");
    const char *artist = host->xml_attr(attrs, count, "grandparentTitle");
    const char *album = host->xml_attr(attrs, count, "parentTitle");
    const char *dur = host->xml_attr(attrs, count, "duration");

    /* The id is what resolve() gets back. */
    if (!plex_is_key(rk)) return 0;
    if (!artist || artist[0] == '\0') {
      artist = host->xml_attr(attrs, count, "originalTitle");
    }
    text[0] = '\0';
    if (artist && album && artist[0] && album[0]) {
      host->format(text, sizeof(text), "%s - %s", artist, album);
    } else if (artist && artist[0]) {
      plex_strlcpy(text, artist, sizeof(text));
    } else if (album && album[0]) {
      plex_strlcpy(text, album, sizeof(text));
    }
    return plex_emit(st, rk, title ? title : "", text, ZZPLAY_ITEM_TRACK, 1,
                     dur ? plex_parse_uint(dur) : 0U);
  }
  return 0;
}

static int plex_fetch_list(PlexContext *ctx, const PlexListOpts *opts,
                           const ZZPlayItemSink *sink, uint32_t *count_out,
                           int *stopped_out, char *err, uint32_t err_size)
{
  char url[416];
  char headers[384];
  PlexItemXmlState xst;
  ZZPlayHostXmlHandler h;
  void *xml;
  PlexXmlFeedState feed;
  int status = 0;
  int ok;
  size_t ulen;

  if (stopped_out) *stopped_out = 0;
  if (count_out) *count_out = 0;

  if (!plex_build_url(ctx, opts->path, url, sizeof(url))) {
    ctx->host->format(err, err_size, "Failed to build URL");
    return 0;
  }

  /* Lists never show summaries and leaving them out makes artist and album
   * pages many times smaller. */
  ulen = plex_strlen(url);
  if (ulen + 23U <= sizeof(url)) {
    const char *suffix = plex_strstr(url, "?") ? "&excludeFields=summary"
                                               : "?excludeFields=summary";
    plex_strlcpy(url + ulen, suffix, sizeof(url) - ulen);
  }

  plex_format_headers(ctx, opts->start, opts->count, opts->paged,
                      headers, sizeof(headers));

  plex_memset(&xst, 0, sizeof(xst));
  xst.ctx = ctx;
  xst.sink = sink;
  xst.mode = opts->mode;
  xst.folder_prefix = opts->folder_prefix ? opts->folder_prefix : "";
  xst.report_total = opts->report_total;
  xst.total_bias = opts->total_bias;

  plex_memset(&h, 0, sizeof(h));
  h.start = plex_item_xml_start;
  h.user = &xst;

  xml = ctx->host->xml_open(ctx->host, &h);
  if (!xml) {
    ctx->host->format(err, err_size, "Out of memory");
    return 0;
  }

  feed.host = ctx->host;
  feed.xml = xml;

  ok = ctx->host->http_get(ctx->host, url, headers, plex_xml_data_cb,
                           &feed, &status, err, err_size);
  if (xst.stopped) {
    ctx->host->xml_close(ctx->host, xml);
    if (stopped_out) *stopped_out = 1;
    return 1;
  }

  if (!plex_finish_reply(ctx->host, xml, ok, status, err, err_size)) {
    return 0;
  }

  if (count_out) *count_out = xst.count_total;
  return 1;
}

static int plex_list_items(PlexContext *ctx, const char *path,
                           uint32_t start, uint32_t count, int paged,
                           const ZZPlayItemSink *sink, int report_total,
                           int *stopped_out, char *err, uint32_t err_size)
{
  PlexListOpts o;

  plex_memset(&o, 0, sizeof(o));
  o.path = path;
  o.start = start;
  o.count = count;
  o.paged = paged;
  o.mode = PLEX_FETCH_ITEMS;
  o.report_total = report_total;
  return plex_fetch_list(ctx, &o, sink, NULL, stopped_out, err, err_size);
}

/* A size-0 request that answers with only the container total. */
static int plex_count_items(PlexContext *ctx, const char *path,
                            uint32_t *total_out, char *err, uint32_t err_size)
{
  PlexListOpts o;

  plex_memset(&o, 0, sizeof(o));
  o.path = path;
  o.paged = 1;
  o.mode = PLEX_FETCH_COUNT;
  return plex_fetch_list(ctx, &o, NULL, total_out, NULL, err, err_size);
}

/* --- Metadata XML parsing for resolve --- */

static int plex_resolve_xml_start(void *user, const char *name,
                                  const ZZPlayHostXmlAttr *attrs,
                                  uint32_t count, int truncated)
{
  PlexResolveXmlState *st = (PlexResolveXmlState *)user;
  (void)truncated;

  if (plex_strcmp(name, "Track") == 0 || plex_strcmp(name, "Metadata") == 0) {
    const char *title = st->host->xml_attr(attrs, count, "title");
    const char *artist = st->host->xml_attr(attrs, count, "grandparentTitle");
    const char *dur = st->host->xml_attr(attrs, count, "duration");
    st->found_track = 1;
    if (!artist || artist[0] == '\0') {
      artist = st->host->xml_attr(attrs, count, "originalTitle");
    }
    if (title && st->title[0] == '\0') {
      plex_copy_text(st->title, sizeof(st->title), title);
    }
    if (artist && st->artist[0] == '\0') {
      plex_copy_text(st->artist, sizeof(st->artist), artist);
    }
    if (dur && st->duration[0] == '\0') {
      plex_strlcpy(st->duration, dur, sizeof(st->duration));
    }
  } else if (plex_strcmp(name, "Media") == 0) {
    const char *codec = st->host->xml_attr(attrs, count, "audioCodec");
    const char *dur = st->host->xml_attr(attrs, count, "duration");
    if (codec && st->codec[0] == '\0') {
      plex_strlcpy(st->codec, codec, sizeof(st->codec));
    }
    if (dur && st->duration[0] == '\0') {
      plex_strlcpy(st->duration, dur, sizeof(st->duration));
    }
  } else if (plex_strcmp(name, "Part") == 0) {
    const char *key = st->host->xml_attr(attrs, count, "key");
    /* A cut part key would ask for another file. */
    if (key && st->part_key[0] == '\0' &&
        plex_strlen(key) < sizeof(st->part_key)) {
      plex_strlcpy(st->part_key, key, sizeof(st->part_key));
    }
  }
  return 0;
}

/* --- Provider API implementation --- */

static const char *const plex_connection_values[] = { "secure", "plain", NULL };
static const char *const plex_connection_labels[] = {
  "Secure (https)", "Plain (http, faster)", NULL
};

static const ZZPlayProviderField plex_fields[] = {
  { "SERVER", "Server URL", ZZPLAY_FIELD_TEXT, 255U, NULL, NULL },
  { "CONNECTION", "Connection", ZZPLAY_FIELD_CHOICE, 6U,
    plex_connection_values, plex_connection_labels },
  { "TOKEN", "Token", ZZPLAY_FIELD_SECRET, 64U, NULL, NULL },
  { "BITRATE", "Bitrate (kbit/s)", ZZPLAY_FIELD_NUMBER, 6U, NULL, NULL }
};

static int plex_host_has_signin(const ZZPlayHost *host)
{
  if (!host) return 0;
  if (host->size < (uint32_t)(offsetof(ZZPlayHost, sleep) + sizeof(int (*)(const ZZPlayHost *, uint32_t)))) {
    return 0;
  }
  return (host->http_post != NULL && host->sleep != NULL);
}

static int plex_fetch_resources(PlexContext *ctx, const char *account_token,
                                char *err, uint32_t err_size)
{
  char headers[384];
  PlexResourcesXmlState rxst;
  ZZPlayHostXmlHandler h;
  PlexXmlFeedState feed;
  void *xml;
  int status = 0;
  int ok;
  int whole;

  ctx->discovered_server_count = 0;

  ctx->host->format(headers, sizeof(headers),
                    "X-Plex-Token: %s\r\n"
                    "X-Plex-Client-Identifier: %s\r\n"
                    "X-Plex-Product: %s\r\n"
                    "X-Plex-Version: %s\r\n"
                    "X-Plex-Platform: %s\r\n"
                    "Accept: application/xml\r\n",
                    account_token, ctx->client_id,
                    PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM);

  rxst.host = ctx->host;
  rxst.ctx = ctx;
  rxst.in_server_resource = 0;

  plex_memset(&h, 0, sizeof(h));
  h.start = plex_resources_xml_start;
  h.end = plex_resources_xml_end;
  h.user = &rxst;

  xml = ctx->host->xml_open(ctx->host, &h);
  if (!xml) {
    ctx->host->format(err, err_size, "Out of memory");
    return 0;
  }

  feed.host = ctx->host;
  feed.xml = xml;

  ok = ctx->host->http_get(ctx->host,
                           "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1",
                           headers, plex_xml_data_cb, &feed, &status, err, err_size);
  whole = plex_xml_done(ctx->host, xml);

  if (status == 401) {
    ctx->host->format(err, err_size, "%s", PLEX_SIGNIN_REJECTED);
    return 0;
  }
  if (!ok || status != 200) {
    if (err && err_size > 0U && err[0] == '\0') {
      if (status != 0) {
        ctx->host->format(err, err_size, "plex.tv answered HTTP %d", status);
      } else {
        ctx->host->format(err, err_size, "plex.tv did not answer");
      }
    }
    return 0;
  }
  if (!whole) {
    ctx->discovered_server_count = 0;
    ctx->host->format(err, err_size, "%s", PLEX_UNREADABLE);
    return 0;
  }
  return 1;
}

static int plex_select_server(PlexContext *ctx, const PlexDiscoveredServer *srv,
                              char *err, uint32_t err_size)
{
  char test_url[384];
  char headers[384];
  char secure_url[256];
  char plain_url[256];
  char target_url[256];
  char conn_pref[16];
  uint32_t i;
  int is_plain;
  int connected = 0;
  size_t slen;

  secure_url[0] = '\0';
  plain_url[0] = '\0';
  target_url[0] = '\0';

  /* Collect both local addresses (https and http) when resources list them */
  for (i = 0U; i < srv->connection_count; i++) {
    const PlexDiscoveredConnection *conn = &srv->connections[i];
    if (secure_url[0] == '\0') {
      if (conn->uri[0] != '\0' && (conn->is_https || plex_strncmp(conn->uri, "https://", 8) == 0)) {
        plex_strlcpy(secure_url, conn->uri, sizeof(secure_url));
        slen = plex_strlen(secure_url);
        while (slen > 0 && secure_url[slen - 1] == '/') {
          secure_url[--slen] = '\0';
        }
      }
    }
    if (plain_url[0] == '\0') {
      if (conn->address[0] != '\0') {
        const char *port = (conn->port[0] != '\0') ? conn->port : "32400";
        ctx->host->format(plain_url, sizeof(plain_url), "http://%s:%s", conn->address, port);
      } else if (conn->uri[0] != '\0' && plex_strncmp(conn->uri, "http://", 7) == 0) {
        plex_strlcpy(plain_url, conn->uri, sizeof(plain_url));
        slen = plex_strlen(plain_url);
        while (slen > 0 && plain_url[slen - 1] == '/') {
          plain_url[--slen] = '\0';
        }
      }
    }
  }

  conn_pref[0] = '\0';
  ctx->host->pref_get(ctx->host, "CONNECTION", conn_pref, (uint32_t)sizeof(conn_pref));
  is_plain = (plex_strcasecmp(conn_pref, "plain") == 0);

  /* Probe only the address of the chosen kind */
  for (i = 0U; i < srv->connection_count; i++) {
    const PlexDiscoveredConnection *conn = &srv->connections[i];
    char candidate[256];
    candidate[0] = '\0';

    if (is_plain) {
      if (conn->address[0] != '\0') {
        const char *port = (conn->port[0] != '\0') ? conn->port : "32400";
        ctx->host->format(candidate, sizeof(candidate), "http://%s:%s", conn->address, port);
      } else if (conn->uri[0] != '\0' && plex_strncmp(conn->uri, "http://", 7) == 0) {
        plex_strlcpy(candidate, conn->uri, sizeof(candidate));
      }
    } else {
      if (conn->uri[0] != '\0' && (conn->is_https || plex_strncmp(conn->uri, "https://", 8) == 0)) {
        plex_strlcpy(candidate, conn->uri, sizeof(candidate));
      }
    }

    if (candidate[0] != '\0') {
      int status = 0;
      int ok;
      slen = plex_strlen(candidate);
      while (slen > 0 && candidate[slen - 1] == '/') {
        candidate[--slen] = '\0';
      }
      ctx->host->format(test_url, sizeof(test_url), "%s/identity", candidate);
      ctx->host->format(headers, sizeof(headers),
                        "X-Plex-Token: %s\r\n"
                        "X-Plex-Client-Identifier: %s\r\n"
                        "X-Plex-Product: %s\r\n"
                        "X-Plex-Version: %s\r\n"
                        "X-Plex-Platform: %s\r\n"
                        "Accept: application/xml\r\n",
                        srv->access_token, ctx->client_id,
                        PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM);
      status = 0;
      ok = ctx->host->http_get(ctx->host, test_url, headers,
                               plex_discard_data_cb, NULL, &status, err, err_size);
      if (ok && status == 200) {
        plex_strlcpy(target_url, candidate, sizeof(target_url));
        connected = 1;
        break;
      }
    }
  }

  if (!connected) {
    if (is_plain) {
      ctx->host->format(err, err_size,
                        "The server did not answer over http; set Connection to secure or check the server");
    } else {
      ctx->host->format(err, err_size,
                        "The server did not answer over https; set Connection to plain or check the server");
    }
    return 0;
  }

  if (is_plain) {
    plex_strlcpy(plain_url, target_url, sizeof(plain_url));
  } else {
    plex_strlcpy(secure_url, target_url, sizeof(secure_url));
  }

  /* Store both local addresses when resources list them, and SERVER for
   * compatibility. The token goes last: a save that stops part way leaves
   * the service signed out, never signed in only for this session. */
  if ((secure_url[0] != '\0' &&
       !ctx->host->pref_set(ctx->host, "SERVER_SECURE", secure_url)) ||
      (plain_url[0] != '\0' &&
       !ctx->host->pref_set(ctx->host, "SERVER_PLAIN", plain_url)) ||
      !ctx->host->pref_set(ctx->host, "SERVER", target_url) ||
      !ctx->host->pref_set(ctx->host, "TOKEN", srv->access_token)) {
    ctx->host->format(err, err_size, "Could not save the settings");
    return 0;
  }

  plex_strlcpy(ctx->server, target_url, sizeof(ctx->server));
  plex_strlcpy(ctx->token, srv->access_token, sizeof(ctx->token));
  ctx->sections_loaded = 0;
  ctx->section_count = 0;

  return 1;
}

static const char *plex_find_section(PlexContext *ctx, const char *key)
{
  uint32_t i;

  for (i = 0U; i < ctx->section_count; i++) {
    if (plex_strcmp(ctx->sections[i].key, key) == 0) {
      return ctx->sections[i].key;
    }
  }
  return NULL;
}

/* Emits "Artists, Albums, Genres, Recently added", plus Playlists when a
 * single library holds the whole top level. */
static int plex_emit_library_views(PlexContext *ctx, const char *sec_key,
                                   uint32_t start, uint32_t count,
                                   const ZZPlayItemSink *sink, int with_playlists)
{
  static const char *const views[4] = { "art", "alb", "gen", "new" };
  static const char *const titles[4] = {
    "Artists", "Albums", "Genres", "Recently added"
  };
  uint32_t total = with_playlists ? 5U : 4U;
  uint32_t i;

  if (sink->total) {
    sink->total(sink->user, total);
  }
  for (i = 0U; i < total; i++) {
    ZZPlayProviderItem it;
    char id[48];
    const char *row_id;

    if (i < start) continue;
    if (count > 0U && i >= start + count) break;

    plex_memset(&it, 0, sizeof(it));
    if (i < 4U) {
      ctx->host->format(id, sizeof(id), "lib:%s:%s", sec_key, views[i]);
      it.title = titles[i];
      row_id = id;
    } else {
      row_id = "playlists";
      it.title = "Playlists";
    }
    it.id = row_id;
    it.subtitle = "";
    it.kind = ZZPLAY_ITEM_FOLDER;
    it.playable = 0;
    if (sink->item(sink->user, &it) != 0) {
      return 1;
    }
  }
  return 1;
}

static int plex_list_library_top(PlexContext *ctx, uint32_t start, uint32_t count,
                                 const ZZPlayItemSink *sink, char *err, uint32_t err_size)
{
  uint32_t total;
  uint32_t cur = 0U;
  uint32_t i;

  if (ctx->server[0] == '\0') {
    ctx->host->format(err, err_size, "Set the server in Settings");
    return 0;
  }

  /* Reload sections on top level */
  ctx->sections_loaded = 0;
  if (!plex_load_sections(ctx, err, err_size)) {
    return 0;
  }

  if (ctx->section_count == 0) {
    /* Also what an answer that is not a library list looks like. */
    ctx->host->format(err, err_size, "The Plex server has no music library");
    return 0;
  }
  if (ctx->section_count == 1) {
    return plex_emit_library_views(ctx, ctx->sections[0].key, start, count, sink, 1);
  }

  /* Several libraries: Playlists stays at the top level, one folder per
   * library below it. */
  total = 1U + ctx->section_count;
  if (sink->total) {
    sink->total(sink->user, total);
  }
  for (i = 0U; i <= ctx->section_count; i++) {
    ZZPlayProviderItem it;
    char id[48];

    if (cur >= start && (count == 0U || cur < start + count)) {
      plex_memset(&it, 0, sizeof(it));
      if (i == 0U) {
        it.id = "playlists";
        it.title = "Playlists";
      } else {
        ctx->host->format(id, sizeof(id), "sec:%s", ctx->sections[i - 1U].key);
        it.id = id;
        it.title = ctx->sections[i - 1U].title;
      }
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) {
        return 1;
      }
    }
    cur++;
  }
  return 1;
}

/* Same address, ignoring trailing slashes. */
static int plex_same_server(const char *a, const char *b)
{
  size_t alen = plex_strlen(a);
  size_t blen = plex_strlen(b);

  while (alen > 0U && a[alen - 1U] == '/') alen--;
  while (blen > 0U && b[blen - 1U] == '/') blen--;
  return alen != 0U && alen == blen && plex_strncmp(a, b, alen) == 0;
}

/* Copy the section part of a node id, [start, sep), into a
 * PLEX_SECTION_KEY_MAX buffer. Node ids can come from ARexx, so a part
 * that cannot be a section key is refused rather than cut. */
static int plex_section_from_node(char *dst, const char *start,
                                  const char *sep)
{
  size_t len = (size_t)(sep - start);

  if (len == 0U || len >= PLEX_SECTION_KEY_MAX) {
    return 0;
  }
  plex_strlcpy(dst, start, len + 1U);
  return 1;
}

static void *plex_open(const ZZPlayHost *host, char *err, uint32_t err_size)
{
  PlexContext *ctx;
  char bitrate_str[16];
  char server_secure[256];
  char server_plain[256];
  char server_manual[256];
  char conn_pref[16];
  int is_plain;
  size_t slen;

  if (!host) return NULL;

  ctx = (PlexContext *)host->alloc(host, (uint32_t)sizeof(PlexContext));
  if (!ctx) {
    host->format(err, err_size, "Out of memory");
    return NULL;
  }
  ctx->host = host;

  server_secure[0] = '\0';
  server_plain[0] = '\0';
  server_manual[0] = '\0';
  conn_pref[0] = '\0';
  ctx->server[0] = '\0';

  host->pref_get(host, "CONNECTION", conn_pref, (uint32_t)sizeof(conn_pref));
  host->pref_get(host, "SERVER_SECURE", server_secure, (uint32_t)sizeof(server_secure));
  host->pref_get(host, "SERVER_PLAIN", server_plain, (uint32_t)sizeof(server_plain));
  host->pref_get(host, "SERVER", server_manual, (uint32_t)sizeof(server_manual));
  host->pref_get(host, "TOKEN", ctx->token, (uint32_t)sizeof(ctx->token));

  is_plain = (plex_strcasecmp(conn_pref, "plain") == 0);

  /* Sign-in stores SERVER as one of the pair. A Server URL the user has
   * since changed in Settings wins over both. */
  if (server_manual[0] != '\0' &&
      !plex_same_server(server_manual, server_secure) &&
      !plex_same_server(server_manual, server_plain)) {
    server_secure[0] = '\0';
    server_plain[0] = '\0';
  }

  if (is_plain) {
    if (server_plain[0] != '\0') {
      plex_strlcpy(ctx->server, server_plain, sizeof(ctx->server));
    } else if (!plex_plexdirect_plain(host, server_secure, ctx->server, (uint32_t)sizeof(ctx->server)) &&
               !plex_plexdirect_plain(host, server_manual, ctx->server, (uint32_t)sizeof(ctx->server))) {
      /* Signed in before both addresses were stored: turn a plex.direct
       * address into its plain form, else use SERVER as before. */
      plex_strlcpy(ctx->server, server_manual, sizeof(ctx->server));
    }
  } else {
    if (server_secure[0] != '\0') {
      plex_strlcpy(ctx->server, server_secure, sizeof(ctx->server));
    } else {
      plex_strlcpy(ctx->server, server_manual, sizeof(ctx->server));
    }
  }

  slen = plex_strlen(ctx->server);
  while (slen > 0 && ctx->server[slen - 1] == '/') {
    ctx->server[--slen] = '\0';
  }

  bitrate_str[0] = '\0';
  host->pref_get(host, "BITRATE", bitrate_str, (uint32_t)sizeof(bitrate_str));
  ctx->bitrate = plex_parse_uint(bitrate_str);
  if (ctx->bitrate == 0) {
    ctx->bitrate = PLEX_DEFAULT_BITRATE;
  }

  host->pref_get(host, "CLIENTID", ctx->client_id, (uint32_t)sizeof(ctx->client_id));
  if (ctx->client_id[0] == '\0') {
    static uint32_t client_counter = 1;
    uint32_t hash = 5381U;
    const char *p = (ctx->server[0] != '\0') ? ctx->server : "zzplay-plex";
    uint32_t c = client_counter++;
    while (*p) {
      hash = ((hash << 5) + hash) + (uint8_t)*p++;
    }
    host->format(ctx->client_id, (uint32_t)sizeof(ctx->client_id),
                 "zzplay-%08lx-%08lx", (unsigned long)hash, (unsigned long)c);
    host->pref_set(host, "CLIENTID", ctx->client_id);
  }

  ctx->session_counter = 1;
  return ctx;
}

static void plex_close(void *ctx_in)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  if (!ctx) return;
  ctx->host->free(ctx->host, ctx);
}

/* Opens one search group ("sq:<type>:<escaped query>"): its results, paged,
 * with the total reported. */
static int plex_list_search_group(PlexContext *ctx, const char *type_and_query,
                                  uint32_t start, uint32_t count,
                                  const ZZPlayItemSink *sink,
                                  char *err, uint32_t err_size)
{
  const char *p = type_and_query;
  const char *esc_query;
  char path[320];
  uint32_t type = 0U;

  while (*p >= '0' && *p <= '9') {
    type = type * 10U + (uint32_t)(*p - '0');
    p++;
  }
  if (*p != ':' || (type != 8U && type != 9U && type != 10U)) {
    ctx->host->format(err, err_size, "Unknown browse node");
    return 0;
  }
  esc_query = p + 1;
  if (!plex_is_safe_token(esc_query)) {
    ctx->host->format(err, err_size, "Unknown browse node");
    return 0;
  }
  if (!plex_load_sections(ctx, err, err_size)) {
    return 0;
  }

  if (ctx->section_count == 1) {
    int stopped = 0;

    ctx->host->format(path, sizeof(path),
                      "/library/sections/%s/search?type=%lu&query=%s",
                      ctx->sections[0].key, (unsigned long)type, esc_query);
    return plex_list_items(ctx, path, start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  /* Several libraries: count each, then fetch only the asked-for window. */
  {
    uint32_t counts[PLEX_MAX_SECTIONS];
    uint32_t grand = 0U;
    uint32_t start_rem = start;
    uint32_t count_rem = (count > 0U) ? count : 0xFFFFFFFFU;
    uint32_t i;

    for (i = 0U; i < ctx->section_count; i++) {
      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/search?type=%lu&query=%s",
                        ctx->sections[i].key, (unsigned long)type, esc_query);
      if (!plex_count_items(ctx, path, &counts[i], err, err_size)) {
        return 0;
      }
      grand += counts[i];
    }
    if (sink->total) {
      sink->total(sink->user, grand);
    }

    for (i = 0U; i < ctx->section_count; i++) {
      uint32_t skip = (start_rem < counts[i]) ? start_rem : counts[i];
      uint32_t avail = counts[i] - skip;
      int stopped = 0;

      start_rem -= skip;
      if (count_rem == 0U) break;
      if (avail == 0U) continue;
      if (avail > count_rem) avail = count_rem;
      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/search?type=%lu&query=%s",
                        ctx->sections[i].key, (unsigned long)type, esc_query);
      if (!plex_list_items(ctx, path, skip, avail, 1, sink, 0,
                           &stopped, err, err_size)) {
        return 0;
      }
      count_rem -= avail;
      if (stopped) return 1;
    }
  }
  return 1;
}

static int plex_link_start(void *ctx_in, ZZPlayProviderLinkStart *out,
                           char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  char headers[256];
  PlexPinXmlState pst;
  ZZPlayHostXmlHandler h;
  PlexXmlFeedState feed;
  void *xml;
  int status = 0;
  int ok;
  int whole;

  if (!ctx || !out) return 0;
  plex_memset(out, 0, sizeof(*out));
  if (!plex_host_has_signin(ctx->host)) {
    ctx->host->format(err, err_size,
                      "ZZPlay update required for plex.tv sign-in");
    return 0;
  }
  ctx->host->format(headers, sizeof(headers),
                    "X-Plex-Product: %s\r\n"
                    "X-Plex-Version: %s\r\n"
                    "X-Plex-Platform: %s\r\n"
                    "X-Plex-Client-Identifier: %s\r\n"
                    "Accept: application/xml\r\n",
                    PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM, ctx->client_id);
  plex_memset(&pst, 0, sizeof(pst));
  pst.host = ctx->host;
  plex_memset(&h, 0, sizeof(h));
  h.start = plex_pin_xml_start;
  h.user = &pst;
  xml = ctx->host->xml_open(ctx->host, &h);
  if (!xml) {
    ctx->host->format(err, err_size, "Out of memory");
    return 0;
  }
  feed.host = ctx->host;
  feed.xml = xml;
  ok = ctx->host->http_post(
      ctx->host, "https://plex.tv/api/v2/pins?strong=false", headers,
      NULL, 0U, plex_xml_data_cb, &feed, &status, err, err_size);
  whole = plex_xml_done(ctx->host, xml);
  if (ok && status == 201 && !whole) {
    ctx->host->format(err, err_size, "%s", PLEX_UNREADABLE);
    return 0;
  }
  if (!ok || status != 201 || pst.id[0] == '\0' || pst.code[0] == '\0') {
    if (err && err_size > 0U && err[0] == '\0') {
      if (status != 0) {
        ctx->host->format(err, err_size, "plex.tv answered HTTP %d", status);
      } else {
        ctx->host->format(err, err_size, "plex.tv did not answer");
      }
    }
    return 0;
  }
  plex_strlcpy(out->url, "plex.tv/link", sizeof(out->url));
  plex_strlcpy(out->code, pst.code, sizeof(out->code));
  plex_strlcpy(out->link_id, pst.id, sizeof(out->link_id));
  return 1;
}

static int plex_link_poll(void *ctx_in, const char *link_id,
                          ZZPlayProviderLinkResult *out,
                          char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  char poll_url[256];
  char poll_headers[256];
  char account_token[128];
  uint32_t poll_count;
  int got_token = 0;

  if (!ctx || !out || !plex_is_key(link_id)) {
    if (ctx) ctx->host->format(err, err_size, "Invalid sign-in request");
    return 0;
  }
  plex_memset(out, 0, sizeof(*out));
  if (!plex_host_has_signin(ctx->host)) {
    ctx->host->format(err, err_size,
                      "ZZPlay update required for plex.tv sign-in");
    return 0;
  }
  account_token[0] = '\0';
  ctx->host->format(poll_url, sizeof(poll_url),
                    "https://plex.tv/api/v2/pins/%s", link_id);
  ctx->host->format(poll_headers, sizeof(poll_headers),
                    "X-Plex-Product: %s\r\n"
                    "X-Plex-Version: %s\r\n"
                    "X-Plex-Platform: %s\r\n"
                    "X-Plex-Client-Identifier: %s\r\n"
                    "Accept: application/xml\r\n",
                    PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM, ctx->client_id);

  for (poll_count = 0U; poll_count < 90U; poll_count++) {
    PlexPinXmlState pst;
    ZZPlayHostXmlHandler h;
    PlexXmlFeedState feed;
    void *xml;
    int status = 0;
    int ok;
    int whole;

    plex_memset(&pst, 0, sizeof(pst));
    pst.host = ctx->host;
    plex_memset(&h, 0, sizeof(h));
    h.start = plex_pin_xml_start;
    h.user = &pst;
    xml = ctx->host->xml_open(ctx->host, &h);
    if (!xml) {
      ctx->host->format(err, err_size, "Out of memory");
      return 0;
    }
    feed.host = ctx->host;
    feed.xml = xml;
    ok = ctx->host->http_get(ctx->host, poll_url, poll_headers,
                             plex_xml_data_cb, &feed, &status, err, err_size);
    whole = plex_xml_done(ctx->host, xml);
    if (ok && status == 200 && !whole) {
      ctx->host->format(err, err_size, "%s", PLEX_UNREADABLE);
      return 0;
    }
    if (ok && status == 200) {
      if (pst.auth_token[0] != '\0') {
        plex_strlcpy(account_token, pst.auth_token, sizeof(account_token));
        got_token = 1;
        break;
      }
      if (pst.expires_in[0] != '\0' &&
          plex_parse_uint(pst.expires_in) == 0U) {
        ctx->host->format(err, err_size,
                          "The PIN has expired. Start sign-in again.");
        return 0;
      }
    } else if (status == 404) {
      ctx->host->format(err, err_size,
                        "The PIN has expired. Start sign-in again.");
      return 0;
    } else if (status == 401) {
      ctx->host->format(err, err_size, "%s", PLEX_SIGNIN_REJECTED);
      return 0;
    } else if (!ok && status == 0) {
      if (err && err_size > 0U && err[0] == '\0') {
        ctx->host->format(err, err_size, "plex.tv did not answer");
      }
      return 0;
    }
    if (!ctx->host->sleep(ctx->host, 2000U)) {
      ctx->host->format(err, err_size, "Cancelled");
      return 0;
    }
  }
  if (!got_token) {
    ctx->host->format(err, err_size,
                      "Sign-in timed out. Start sign-in again.");
    return 0;
  }
  if (!plex_fetch_resources(ctx, account_token, err, err_size)) {
    return 0;
  }
  if (ctx->discovered_server_count == 0U) {
    ctx->host->format(err, err_size,
                      "No Plex Media Server found on this account");
    return 0;
  }
  if (ctx->discovered_server_count == 1U) {
    if (!plex_select_server(ctx, &ctx->discovered_servers[0],
                            err, err_size)) {
      return 0;
    }
    out->state = ZZPLAY_LINK_DONE;
    plex_strlcpy(out->signed_in_name, ctx->discovered_servers[0].name,
                 sizeof(out->signed_in_name));
    return 1;
  }
  out->state = ZZPLAY_LINK_CHOOSE;
  out->choice_count = ctx->discovered_server_count;
  if (out->choice_count > ZZPLAY_LINK_CHOICES_MAX) {
    out->choice_count = ZZPLAY_LINK_CHOICES_MAX;
  }
  for (poll_count = 0U; poll_count < out->choice_count; poll_count++) {
    ctx->host->format(out->choices[poll_count].id,
                      sizeof(out->choices[poll_count].id), "%lu",
                      (unsigned long)poll_count);
    ctx->host->format(out->choices[poll_count].label,
                      sizeof(out->choices[poll_count].label), "%s (%s)",
                      ctx->discovered_servers[poll_count].name,
                      ctx->discovered_servers[poll_count].owned
                          ? "owned" : "shared");
  }
  return 1;
}

static int plex_link_choose(void *ctx_in, const char *choice_id,
                            ZZPlayProviderLinkResult *out,
                            char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  uint32_t idx;
  const char *p;

  if (!ctx || !out || !choice_id || choice_id[0] == '\0') return 0;
  for (p = choice_id; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') {
      ctx->host->format(err, err_size, "Invalid server selection");
      return 0;
    }
  }
  idx = plex_parse_uint(choice_id);
  if (idx >= ctx->discovered_server_count ||
      idx >= ZZPLAY_LINK_CHOICES_MAX) {
    ctx->host->format(err, err_size, "Invalid server selection");
    return 0;
  }
  plex_memset(out, 0, sizeof(*out));
  if (!plex_select_server(ctx, &ctx->discovered_servers[idx],
                          err, err_size)) {
    return 0;
  }
  out->state = ZZPLAY_LINK_DONE;
  plex_strlcpy(out->signed_in_name, ctx->discovered_servers[idx].name,
               sizeof(out->signed_in_name));
  return 1;
}

static int plex_list(void *ctx_in, const char *node, uint32_t start, uint32_t count,
                     const ZZPlayItemSink *sink, char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  if (!ctx || !sink) return 0;

  if (node == NULL || node[0] == '\0') {
    if (ctx->token[0] == '\0') {
      if (sink->total) {
        sink->total(sink->user, 1U);
      }
      if (start == 0U) {
        ZZPlayProviderItem it;
        plex_memset(&it, 0, sizeof(it));
        it.id = ZZPLAY_PROVIDER_SETTINGS_ID;
        it.title = "Not signed in. Open Settings to sign in.";
        it.subtitle = "";
        it.kind = ZZPLAY_ITEM_TRACK;
        it.playable = 0U;
        sink->item(sink->user, &it);
      }
      return 1;
    }
    return plex_list_library_top(ctx, start, count, sink, err, err_size);
  }


  if (plex_strcmp(node, "playlists") == 0) {
    int stopped = 0;
    return plex_list_items(ctx, "/playlists?playlistType=audio",
                           start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  if (plex_strncmp(node, "sec:", 4) == 0) {
    const char *sec;

    if (!plex_load_sections(ctx, err, err_size)) {
      return 0;
    }
    sec = plex_find_section(ctx, node + 4);
    if (!sec) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    return plex_emit_library_views(ctx, sec, start, count, sink, 0);
  }

  if (plex_strncmp(node, "lib:", 4) == 0) {
    const char *sep = plex_strstr(node + 4, ":");
    const char *view;
    const char *sec;
    char sec_buf[PLEX_SECTION_KEY_MAX];
    char path[160];
    char prefix[48];

    if (!sep) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    view = sep + 1;
    if (!plex_load_sections(ctx, err, err_size)) {
      return 0;
    }
    if (!plex_section_from_node(sec_buf, node + 4, sep)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    sec = plex_find_section(ctx, sec_buf);
    if (!sec) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }

    if (plex_strcmp(view, "art") == 0 || plex_strcmp(view, "alb") == 0) {
      PlexListOpts o;
      int is_album = (view[0] == 'a' && view[1] == 'l');

      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/firstCharacter?type=%d",
                        sec, is_album ? 9 : 8);
      ctx->host->format(prefix, sizeof(prefix), "ltr:%s:%d:",
                        sec, is_album ? 9 : 8);
      plex_memset(&o, 0, sizeof(o));
      o.path = path;
      o.start = start;
      o.count = count;
      o.paged = count > 0;
      o.mode = PLEX_FETCH_LETTERS;
      o.folder_prefix = prefix;
      o.report_total = 1;
      return plex_fetch_list(ctx, &o, sink, NULL, NULL, err, err_size);
    }
    if (plex_strcmp(view, "gen") == 0) {
      PlexListOpts o;

      ctx->host->format(path, sizeof(path), "/library/sections/%s/genre?type=8", sec);
      ctx->host->format(prefix, sizeof(prefix), "gen:%s:", sec);
      plex_memset(&o, 0, sizeof(o));
      o.path = path;
      o.start = start;
      o.count = count;
      o.paged = count > 0;
      o.mode = PLEX_FETCH_GENRES;
      o.folder_prefix = prefix;
      o.report_total = 1;
      return plex_fetch_list(ctx, &o, sink, NULL, NULL, err, err_size);
    }
    if (plex_strcmp(view, "new") == 0) {
      int stopped = 0;

      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/all?type=9&sort=addedAt:desc", sec);
      return plex_list_items(ctx, path, start, count, count > 0, sink, 1,
                             &stopped, err, err_size);
    }
    ctx->host->format(err, err_size, "Unknown browse node");
    return 0;
  }

  if (plex_strncmp(node, "ltr:", 4) == 0) {
    /* ltr:<section>:<8|9>:<letter key> */
    const char *sep = plex_strstr(node + 4, ":");
    const char *key;
    const char *sec;
    char sec_buf[PLEX_SECTION_KEY_MAX];
    char path[192];
    char type_char;
    int stopped = 0;

    if (!sep || sep == node + 4) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    if (!plex_section_from_node(sec_buf, node + 4, sep)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    type_char = sep[1];
    key = sep + 3;
    if ((type_char != '8' && type_char != '9') || sep[2] != ':' ||
        !plex_is_safe_token(key)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    if (!plex_load_sections(ctx, err, err_size)) {
      return 0;
    }
    sec = plex_find_section(ctx, sec_buf);
    if (!sec) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    ctx->host->format(path, sizeof(path),
                      "/library/sections/%s/firstCharacter/%s?type=%c",
                      sec, key, type_char);
    return plex_list_items(ctx, path, start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  if (plex_strncmp(node, "gen:", 4) == 0) {
    /* gen:<section>:<genre id> */
    const char *sep = plex_strstr(node + 4, ":");
    const char *gid;
    const char *sec;
    char sec_buf[PLEX_SECTION_KEY_MAX];
    char path[160];
    int stopped = 0;

    if (!sep || sep == node + 4) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    if (!plex_section_from_node(sec_buf, node + 4, sep)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    gid = sep + 1;
    if (!plex_is_safe_token(gid)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    if (!plex_load_sections(ctx, err, err_size)) {
      return 0;
    }
    sec = plex_find_section(ctx, sec_buf);
    if (!sec) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    ctx->host->format(path, sizeof(path),
                      "/library/sections/%s/all?type=8&genre=%s", sec, gid);
    return plex_list_items(ctx, path, start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  if (plex_strncmp(node, "sq:", 3) == 0) {
    return plex_list_search_group(ctx, node + 3, start, count, sink, err, err_size);
  }

  if (plex_strncmp(node, "art:", 4) == 0 || plex_strncmp(node, "alb:", 4) == 0 ||
      plex_strncmp(node, "pl:", 3) == 0) {
    /* Ids can come from a saved list or ARexx too: only a plain key goes
     * into the request path. */
    const char *rk = node + (node[0] == 'p' ? 3 : 4);
    char path[96];
    int stopped = 0;

    if (!plex_is_key(rk)) {
      ctx->host->format(err, err_size, "Unknown browse node");
      return 0;
    }
    ctx->host->format(path, sizeof(path),
                      node[0] == 'p' ? "/playlists/%s/items"
                                     : "/library/metadata/%s/children",
                      rk);
    return plex_list_items(ctx, path, start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  if (node[0] == '/') {
    int stopped = 0;
    return plex_list_items(ctx, node, start, count, count > 0, sink, 1,
                           &stopped, err, err_size);
  }

  ctx->host->format(err, err_size, "Unknown browse node");
  return 0;
}

static int plex_search(void *ctx_in, const char *query, const ZZPlayItemSink *sink,
                       char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  static const int group_types[3] = {8, 9, 10};
  static const char *const group_titles[3] = {"Artists", "Albums", "Tracks"};
  static const int flat_types[3] = {10, 9, 8};
  char esc_query[256];
  uint32_t esc_len;
  uint32_t counts[3];
  uint32_t s;
  int t;

  if (!ctx || !query || !sink) return 0;

  esc_len = ctx->host->url_escape(query, esc_query, sizeof(esc_query));
  if (esc_len == 0 && query[0] != '\0') {
    ctx->host->format(err, err_size, "Query too long");
    return 0;
  }

  if (!plex_load_sections(ctx, err, err_size)) {
    return 0;
  }
  if (ctx->section_count == 0) {
    return 1;
  }

  /* The group folders carry the query in their id; fall back to one mixed
   * list when it does not fit. */
  if (esc_len + 7U > ZZPLAY_PROVIDER_TEXT_MAX) {
    for (s = 0U; s < ctx->section_count; s++) {
      for (t = 0; t < 3; t++) {
        char path[320];
        int stopped = 0;

        ctx->host->format(path, sizeof(path),
                          "/library/sections/%s/search?type=%d&query=%s",
                          ctx->sections[s].key, flat_types[t], esc_query);
        if (!plex_list_items(ctx, path, 0, 0, 0, sink, 0,
                             &stopped, err, err_size)) {
          return 0;
        }
        if (stopped) {
          return 1;
        }
      }
    }
    return 1;
  }

  plex_memset(counts, 0, sizeof(counts));
  for (s = 0U; s < ctx->section_count; s++) {
    for (t = 0; t < 3; t++) {
      char path[320];
      uint32_t n = 0U;

      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/search?type=%d&query=%s",
                        ctx->sections[s].key, group_types[t], esc_query);
      if (!plex_count_items(ctx, path, &n, err, err_size)) {
        return 0;
      }
      counts[t] += n;
    }
  }

  {
    uint32_t groups = 0U;

    for (t = 0; t < 3; t++) {
      if (counts[t] > 0U) groups++;
    }
    if (sink->total) {
      sink->total(sink->user, groups);
    }
    for (t = 0; t < 3; t++) {
      ZZPlayProviderItem it;
      char id[96];
      char title[96];

      if (counts[t] == 0U) continue;
      ctx->host->format(id, sizeof(id), "sq:%d:%s", group_types[t], esc_query);
      ctx->host->format(title, sizeof(title), "%s (%lu)",
                        group_titles[t], (unsigned long)counts[t]);
      plex_memset(&it, 0, sizeof(it));
      it.id = id;
      it.title = title;
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) {
        return 1;
      }
    }
  }
  return 1;
}

/* "Artist - Title" of the track metadata just read, cleaned and cut to
 * the resolved title. */
static void plex_resolved_title(const PlexContext *ctx, ZZPlayResolved *out)
{
  char text[sizeof(ctx->rst.artist) + sizeof(ctx->rst.title) + 4U];

  if (ctx->rst.artist[0] != '\0') {
    ctx->host->format(text, sizeof(text), "%s - %s", ctx->rst.artist,
                      ctx->rst.title);
  } else {
    plex_strlcpy(text, ctx->rst.title, sizeof(text));
  }
  plex_copy_text(out->title, sizeof(out->title), text);
}

static int plex_fetch_metadata(PlexContext *ctx, const char *rk,
                               char *err, uint32_t err_size)
{
  char path[64];
  char *url = ctx->scratch;
  char headers[256];
  ZZPlayHostXmlHandler h;
  void *xml;
  PlexXmlFeedState feed;
  int status = 0;
  int ok;

  ctx->host->format(path, sizeof(path), "/library/metadata/%s", rk);
  if (!plex_build_url(ctx, path, url, (uint32_t)sizeof(ctx->scratch))) {
    ctx->host->format(err, err_size, "Failed to build URL");
    return 0;
  }
  plex_format_headers(ctx, 0, 0, 0, headers, sizeof(headers));

  plex_memset(&ctx->rst, 0, sizeof(ctx->rst));
  ctx->rst.host = ctx->host;

  plex_memset(&h, 0, sizeof(h));
  h.start = plex_resolve_xml_start;
  h.user = &ctx->rst;

  xml = ctx->host->xml_open(ctx->host, &h);
  if (!xml) {
    ctx->host->format(err, err_size, "Out of memory");
    return 0;
  }

  feed.host = ctx->host;
  feed.xml = xml;

  ok = ctx->host->http_get(ctx->host, url, headers, plex_xml_data_cb,
                           &feed, &status, err, err_size);
  if (!plex_finish_reply(ctx->host, xml, ok, status, err, err_size)) {
    return 0;
  }

  if (!ctx->rst.found_track) {
    ctx->host->format(err, err_size, "Track metadata missing from Plex");
    return 0;
  }
  return 1;
}

static int plex_resolve_transcode(PlexContext *ctx, const char *rk,
                                  ZZPlayResolved *out,
                                  char *err, uint32_t err_size)
{
  char session_id[48];
  char headers[256];
  char *query = ctx->scratch;
  int status = 0;
  int ok;
  int qlen;
  int dlen;
  int ulen;

  ctx->host->format(session_id, sizeof(session_id), "zzsess-%lu-%lu",
                    (unsigned long)plex_parse_uint(rk),
                    (unsigned long)ctx->session_counter++);

  qlen = ctx->host->format(query, (uint32_t)sizeof(ctx->scratch),
    "path=%%2Flibrary%%2Fmetadata%%2F%s"
    "&mediaIndex=0&partIndex=0&protocol=http&directPlay=0&directStream=0"
    "&musicBitrate=%lu"
    "&session=%s&X-Plex-Session-Identifier=%s"
    "&X-Plex-Client-Identifier=%s"
    "&X-Plex-Product=%s"
    "&X-Plex-Version=%s"
    "&X-Plex-Platform=%s"
    "&X-Plex-Client-Profile-Extra=add-transcode-target"
    "(type%%3DmusicProfile%%26context%%3Dstreaming%%26protocol%%3Dhttp%%26container%%3Dmp3%%26audioCodec%%3Dmp3)"
    "&X-Plex-Token=%s",
    rk, (unsigned long)ctx->bitrate, session_id, session_id,
    ctx->client_id, PLEX_PRODUCT, PLEX_VERSION, PLEX_PLATFORM,
    ctx->token);
  if (qlen < 0 || (uint32_t)qlen >= sizeof(ctx->scratch)) {
    ctx->host->format(err, err_size, "Transcode query too long");
    return 0;
  }

  dlen = ctx->host->format(out->url, (uint32_t)sizeof(out->url),
    "%s/music/:/transcode/universal/decision?%s",
    ctx->server, query);
  if (dlen < 0 || (uint32_t)dlen >= sizeof(out->url)) {
    ctx->host->format(err, err_size, "Decision URL too long");
    return 0;
  }

  plex_format_headers(ctx, 0, 0, 0, headers, sizeof(headers));
  ok = ctx->host->http_get(ctx->host, out->url, headers, plex_discard_data_cb,
                           NULL, &status, err, err_size);
  if (!plex_reply_ok(ctx->host, ok, status, err, err_size)) {
    return 0;
  }

  ulen = ctx->host->format(out->url, (uint32_t)sizeof(out->url),
    "%s/music/:/transcode/universal/start.mp3?%s",
    ctx->server, query);
  if (ulen < 0 || (uint32_t)ulen >= sizeof(out->url)) {
    ctx->host->format(err, err_size, "Resolved URL is too long");
    return 0;
  }

  plex_resolved_title(ctx, out);
  out->duration_ms = plex_parse_uint(ctx->rst.duration);
  return 1;
}

static int plex_resolve(void *ctx_in, const char *item, ZZPlayResolved *out,
                       char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  const char *rk;

  if (!ctx || !item || !out) return 0;

  rk = item;
  if (plex_strncmp(rk, "plex://", 7) == 0) {
    rk += 7;
  }
  while (*rk == '/') {
    rk++;
  }
  if (!plex_is_key(rk)) {
    ctx->host->format(err, err_size, "Invalid Plex item ID");
    return 0;
  }

  if (!plex_fetch_metadata(ctx, rk, err, err_size)) {
    return 0;
  }

  if (plex_is_direct_codec(ctx->rst.codec)) {
    int has_q = 0;
    const char *p;
    int ulen;

    if (ctx->rst.part_key[0] == '\0') {
      ctx->host->format(err, err_size, "Track part key missing from Plex");
      return 0;
    }

    p = ctx->rst.part_key;
    while (*p) {
      if (*p == '?') { has_q = 1; break; }
      p++;
    }

    ulen = ctx->host->format(out->url, sizeof(out->url),
      "%s%s%cX-Plex-Token=%s",
      ctx->server, ctx->rst.part_key, has_q ? '&' : '?', ctx->token);
    if (ulen < 0 || (uint32_t)ulen >= sizeof(out->url)) {
      ctx->host->format(err, err_size, "Resolved URL is too long");
      return 0;
    }

    plex_resolved_title(ctx, out);
    out->duration_ms = plex_parse_uint(ctx->rst.duration);
    return 1;
  }

  return plex_resolve_transcode(ctx, rk, out, err, err_size);
}

static const ZZPlayProvider plex_provider = {
  .id = ZZPLAY_PROVIDER_ID,
  .abi = ZZPLAY_PROVIDER_ABI,
  .size = (uint32_t)sizeof(ZZPlayProvider),
  .scheme = PLEX_SCHEME,
  .name = PLEX_NAME,
  .fields = plex_fields,
  .field_count = (uint32_t)(sizeof(plex_fields) / sizeof(plex_fields[0])),
  .open = plex_open,
  .close = plex_close,
  .list = plex_list,
  .search = plex_search,
  .resolve = plex_resolve,
  .link_name = "Plex",
  .link_host = "plex.tv",
  .link_start = plex_link_start,
  .link_poll = plex_link_poll,
  .link_choose = plex_link_choose
};

const ZZPlayProvider *zzplay_provider_entry(void)
{
  return &plex_provider;
}
