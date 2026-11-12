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

typedef struct PlexItemXmlState {
  const PlexContext *ctx;
  const ZZPlayItemSink *sink;
  int stopped;
  int is_search;
  uint32_t total_bias;
} PlexItemXmlState;

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

static int plex_build_url(const PlexContext *ctx, const char *path, char *out, uint32_t size)
{
  if (!ctx || !path || !out || size == 0) return 0;
  if (path[0] == 'h' && path[1] == 't' && path[2] == 't' && path[3] == 'p') {
    plex_strlcpy(out, path, size);
    return 1;
  }
  if (path[0] == '/') {
    return ctx->host->format(out, size, "%s%s", ctx->server, path) >= 0;
  }
  return ctx->host->format(out, size, "%s/%s", ctx->server, path) >= 0;
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
    if (id) plex_strlcpy(st->id, id, sizeof(st->id));
    if (code) plex_strlcpy(st->code, code, sizeof(st->code));
    if (tok) plex_strlcpy(st->auth_token, tok, sizeof(st->auth_token));
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
          plex_strlcpy(srv->name, rname, sizeof(srv->name));
        } else {
          plex_strlcpy(srv->name, "Plex Media Server", sizeof(srv->name));
        }
        if (tok) {
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
      if (key && title && st->ctx->section_count < PLEX_MAX_SECTIONS) {
        PlexSection *sec = &st->ctx->sections[st->ctx->section_count++];
        plex_strlcpy(sec->key, key, sizeof(sec->key));
        plex_strlcpy(sec->title, title, sizeof(sec->title));
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
  ctx->host->xml_close(ctx->host, xml);

  if (status == 401) {
    ctx->host->format(err, err_size, "Plex rejected the token");
    return 0;
  }
  if (status != 200 && status != 0) {
    ctx->host->format(err, err_size, "Plex answered HTTP %d", status);
    return 0;
  }
  if (!ok) {
    return 0;
  }

  ctx->sections_loaded = 1;
  return 1;
}

/* --- Item browsing and search XML parsing --- */

static int plex_item_xml_start(void *user, const char *name,
                               const ZZPlayHostXmlAttr *attrs,
                               uint32_t count, int truncated)
{
  PlexItemXmlState *st = (PlexItemXmlState *)user;
  const ZZPlayHost *host = st->ctx->host;
  (void)truncated;

  if (st->stopped) return 1;

  if (plex_strcmp(name, "MediaContainer") == 0) {
    if (!st->is_search && st->sink && st->sink->total) {
      const char *tot = host->xml_attr(attrs, count, "totalSize");
      if (!tot) {
        tot = host->xml_attr(attrs, count, "size");
      }
      if (tot) {
        uint32_t total = plex_parse_uint(tot) + st->total_bias;
        st->sink->total(st->sink->user, total);
      }
    }
  } else if (plex_strcmp(name, "Directory") == 0) {
    const char *type = host->xml_attr(attrs, count, "type");
    const char *rk = host->xml_attr(attrs, count, "ratingKey");
    const char *title = host->xml_attr(attrs, count, "title");
    if (!title) title = "";
    if (!rk) rk = "";

    if (type && plex_strcmp(type, "artist") == 0) {
      char id[48];
      ZZPlayProviderItem it;
      host->format(id, sizeof(id), "art:%s", rk);
      plex_memset(&it, 0, sizeof(it));
      it.id = id;
      it.title = title;
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (st->sink && st->sink->item(st->sink->user, &it) != 0) {
        st->stopped = 1;
        return 1;
      }
    } else if (type && plex_strcmp(type, "album") == 0) {
      char id[48];
      const char *year = host->xml_attr(attrs, count, "year");
      const char *parent = host->xml_attr(attrs, count, "parentTitle");
      ZZPlayProviderItem it;
      host->format(id, sizeof(id), "alb:%s", rk);
      plex_memset(&it, 0, sizeof(it));
      it.id = id;
      it.title = title;
      it.subtitle = year ? year : (parent ? parent : "");
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (st->sink && st->sink->item(st->sink->user, &it) != 0) {
        st->stopped = 1;
        return 1;
      }
    }
  } else if (plex_strcmp(name, "Playlist") == 0) {
    const char *rk = host->xml_attr(attrs, count, "ratingKey");
    const char *title = host->xml_attr(attrs, count, "title");
    char id[48];
    ZZPlayProviderItem it;
    host->format(id, sizeof(id), "pl:%s", rk ? rk : "");
    plex_memset(&it, 0, sizeof(it));
    it.id = id;
    it.title = title ? title : "";
    it.subtitle = "";
    it.kind = ZZPLAY_ITEM_FOLDER;
    it.playable = 0;
    if (st->sink && st->sink->item(st->sink->user, &it) != 0) {
      st->stopped = 1;
      return 1;
    }
  } else if (plex_strcmp(name, "Track") == 0) {
    const char *rk = host->xml_attr(attrs, count, "ratingKey");
    const char *title = host->xml_attr(attrs, count, "title");
    const char *artist = host->xml_attr(attrs, count, "grandparentTitle");
    const char *album = host->xml_attr(attrs, count, "parentTitle");
    const char *dur = host->xml_attr(attrs, count, "duration");
    char sub[96];
    ZZPlayProviderItem it;

    if (!artist || artist[0] == '\0') {
      artist = host->xml_attr(attrs, count, "originalTitle");
    }
    sub[0] = '\0';
    if (artist && album && artist[0] && album[0]) {
      host->format(sub, sizeof(sub), "%s - %s", artist, album);
    } else if (artist && artist[0]) {
      host->format(sub, sizeof(sub), "%s", artist);
    } else if (album && album[0]) {
      host->format(sub, sizeof(sub), "%s", album);
    }

    plex_memset(&it, 0, sizeof(it));
    it.id = rk ? rk : "";
    it.title = title ? title : "";
    it.subtitle = sub;
    it.duration_ms = dur ? plex_parse_uint(dur) : 0;
    it.kind = ZZPLAY_ITEM_TRACK;
    it.playable = 1;
    if (st->sink && st->sink->item(st->sink->user, &it) != 0) {
      st->stopped = 1;
      return 1;
    }
  }
  return 0;
}

static int plex_fetch_items(PlexContext *ctx, const char *path,
                            uint32_t start, uint32_t count, int paged,
                            const ZZPlayItemSink *sink, int is_search,
                            uint32_t total_bias, int *stopped_out,
                            char *err, uint32_t err_size)
{
  char url[384];
  char headers[384];
  PlexItemXmlState xst;
  ZZPlayHostXmlHandler h;
  void *xml;
  PlexXmlFeedState feed;
  int status = 0;
  int ok;

  if (stopped_out) *stopped_out = 0;

  if (!plex_build_url(ctx, path, url, sizeof(url))) {
    ctx->host->format(err, err_size, "Failed to build URL");
    return 0;
  }
  plex_format_headers(ctx, start, count, paged, headers, sizeof(headers));

  plex_memset(&xst, 0, sizeof(xst));
  xst.ctx = ctx;
  xst.sink = sink;
  xst.is_search = is_search;
  xst.total_bias = total_bias;

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
  ctx->host->xml_close(ctx->host, xml);

  if (xst.stopped) {
    if (stopped_out) *stopped_out = 1;
    return 1;
  }

  if (status == 401) {
    ctx->host->format(err, err_size, "Plex rejected the token");
    return 0;
  }
  if (status != 200 && status != 0) {
    ctx->host->format(err, err_size, "Plex answered HTTP %d", status);
    return 0;
  }
  if (!ok) {
    return 0;
  }

  return 1;
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
      plex_strlcpy(st->title, title, sizeof(st->title));
    }
    if (artist && st->artist[0] == '\0') {
      plex_strlcpy(st->artist, artist, sizeof(st->artist));
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
    if (key && st->part_key[0] == '\0') {
      plex_strlcpy(st->part_key, key, sizeof(st->part_key));
    }
  }
  return 0;
}

/* --- Provider API implementation --- */

static const ZZPlayProviderField plex_fields[] = {
  { "SERVER", "Server URL", ZZPLAY_FIELD_TEXT, 255U },
  { "CONNECTION", "Connection", ZZPLAY_FIELD_TEXT, 16U },
  { "TOKEN", "Token", ZZPLAY_FIELD_SECRET, 64U },
  { "BITRATE", "Bitrate (kbit/s)", ZZPLAY_FIELD_NUMBER, 6U }
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
  ctx->host->xml_close(ctx->host, xml);

  if (status == 401) {
    ctx->host->format(err, err_size, "Plex rejected the token");
    return 0;
  }
  if (!ok || status != 200) {
    if (err && err_size > 0U && err[0] == '\0') {
      ctx->host->format(err, err_size, "Plex answered HTTP %d", status);
    }
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

  /* Store both local addresses when resources list them, and SERVER for compatibility */
  if (secure_url[0] != '\0') {
    ctx->host->pref_set(ctx->host, "SERVER_SECURE", secure_url);
  }
  if (plain_url[0] != '\0') {
    ctx->host->pref_set(ctx->host, "SERVER_PLAIN", plain_url);
  }
  ctx->host->pref_set(ctx->host, "SERVER", target_url);
  ctx->host->pref_set(ctx->host, "TOKEN", srv->access_token);

  plex_strlcpy(ctx->server, target_url, sizeof(ctx->server));
  plex_strlcpy(ctx->token, srv->access_token, sizeof(ctx->token));
  ctx->sections_loaded = 0;
  ctx->section_count = 0;

  return 1;
}

static int plex_list_library_top(PlexContext *ctx, uint32_t start, uint32_t count,
                                 const ZZPlayItemSink *sink, char *err, uint32_t err_size)
{
  uint32_t i;
  int stopped = 0;

  if (ctx->server[0] == '\0') {
    ctx->host->format(err, err_size, "Set the server in Settings");
    return 0;
  }

  /* Reload sections on top level */
  ctx->sections_loaded = 0;
  if (!plex_load_sections(ctx, err, err_size)) {
    return 0;
  }

  if (ctx->section_count == 1) {
    const char *sec_key = ctx->sections[0].key;
    char path[64];
    ctx->host->format(path, sizeof(path), "/library/sections/%s/all?type=8", sec_key);

    if (start == 0) {
      ZZPlayProviderItem it;
      uint32_t artist_count;

      plex_memset(&it, 0, sizeof(it));
      it.id = "playlists";
      it.title = "Audio Playlists";
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) {
        return 1;
      }
      if (count == 1) {
        return 1;
      }
      artist_count = (count > 1) ? (count - 1) : 0;
      return plex_fetch_items(ctx, path, 0, artist_count, count > 0,
                              sink, 0, 1, &stopped, err, err_size);
    } else {
      uint32_t artist_start = start - 1;
      return plex_fetch_items(ctx, path, artist_start, count, count > 0,
                              sink, 0, 1, &stopped, err, err_size);
    }
  } else {
    uint32_t total_items = 1 + ctx->section_count;
    uint32_t emitted = 0;
    uint32_t cur = 0;

    if (sink->total) {
      sink->total(sink->user, total_items);
    }

    if (start == 0) {
      ZZPlayProviderItem it;
      plex_memset(&it, 0, sizeof(it));
      it.id = "playlists";
      it.title = "Audio Playlists";
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) {
        return 1;
      }
      emitted++;
    }
    cur++;

    for (i = 0U; i < ctx->section_count; i++) {
      if (count > 0 && emitted >= count) break;
      if (cur >= start) {
        char id[48];
        ZZPlayProviderItem it;
        ctx->host->format(id, sizeof(id), "sec:%s", ctx->sections[i].key);
        plex_memset(&it, 0, sizeof(it));
        it.id = id;
        it.title = ctx->sections[i].title;
        it.subtitle = "";
        it.kind = ZZPLAY_ITEM_FOLDER;
        it.playable = 0;
        if (sink->item(sink->user, &it) != 0) {
          return 1;
        }
        emitted++;
      }
      cur++;
    }
    return 1;
  }
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

  if (is_plain) {
    if (server_plain[0] != '\0') {
      plex_strlcpy(ctx->server, server_plain, sizeof(ctx->server));
    } else {
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

static int plex_list(void *ctx_in, const char *node, uint32_t start, uint32_t count,
                     const ZZPlayItemSink *sink, char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  if (!ctx || !sink) return 0;

  if (node == NULL || node[0] == '\0') {
    if (ctx->token[0] == '\0') {
      if (sink->total) {
        sink->total(sink->user, 1);
      }
      if (start == 0) {
        ZZPlayProviderItem it;
        plex_memset(&it, 0, sizeof(it));
        it.id = "signin";
        it.title = "Sign in with plex.tv";
        it.subtitle = "";
        it.kind = ZZPLAY_ITEM_FOLDER;
        it.playable = 0;
        sink->item(sink->user, &it);
      }
      return 1;
    }
    return plex_list_library_top(ctx, start, count, sink, err, err_size);
  }

  if (plex_strcmp(node, "signin") == 0) {
    char headers[256];
    char post_url[256];
    char code_title[64];
    char continue_id[64];
    PlexPinXmlState pst;
    ZZPlayHostXmlHandler h;
    PlexXmlFeedState feed;
    void *xml;
    int status = 0;
    int ok;

    if (!plex_host_has_signin(ctx->host)) {
      ctx->host->format(err, err_size, "ZZPlay update required for plex.tv sign-in");
      return 0;
    }

    ctx->host->format(post_url, sizeof(post_url), "https://plex.tv/api/v2/pins?strong=false");
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

    ok = ctx->host->http_post(ctx->host, post_url, headers, NULL, 0U,
                              plex_xml_data_cb, &feed, &status, err, err_size);
    ctx->host->xml_close(ctx->host, xml);

    if (!ok || status != 201 || pst.id[0] == '\0' || pst.code[0] == '\0') {
      if (err && err_size > 0U && err[0] == '\0') {
        ctx->host->format(err, err_size, "Failed to request PIN from plex.tv");
      }
      return 0;
    }

    if (sink->total) {
      sink->total(sink->user, 3);
    }

    ctx->host->format(code_title, sizeof(code_title), "and enter the code %s", pst.code);
    ctx->host->format(continue_id, sizeof(continue_id), "signin:continue:%s", pst.id);

    if (start <= 0 && (count == 0 || start + count > 0)) {
      ZZPlayProviderItem it;
      plex_memset(&it, 0, sizeof(it));
      it.id = "info:link";
      it.title = "On any device, open plex.tv/link";
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_TRACK;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) return 1;
    }
    if (start <= 1 && (count == 0 || start + count > 1)) {
      ZZPlayProviderItem it;
      plex_memset(&it, 0, sizeof(it));
      it.id = "info:code";
      it.title = code_title;
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_TRACK;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) return 1;
    }
    if (start <= 2 && (count == 0 || start + count > 2)) {
      ZZPlayProviderItem it;
      plex_memset(&it, 0, sizeof(it));
      it.id = continue_id;
      it.title = "Continue after entering the code";
      it.subtitle = "";
      it.kind = ZZPLAY_ITEM_FOLDER;
      it.playable = 0;
      if (sink->item(sink->user, &it) != 0) return 1;
    }
    return 1;
  }

  if (plex_strncmp(node, "signin:continue:", 16) == 0) {
    const char *pin_id = node + 16;
    char poll_url[256];
    char poll_headers[256];
    char account_token[128];
    uint32_t poll_count;
    int got_token = 0;

    if (!plex_host_has_signin(ctx->host)) {
      ctx->host->format(err, err_size, "ZZPlay update required for plex.tv sign-in");
      return 0;
    }

    account_token[0] = '\0';
    ctx->host->format(poll_url, sizeof(poll_url), "https://plex.tv/api/v2/pins/%s", pin_id);
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
      ctx->host->xml_close(ctx->host, xml);

      if (ok && status == 200) {
        if (pst.auth_token[0] != '\0') {
          plex_strlcpy(account_token, pst.auth_token, sizeof(account_token));
          got_token = 1;
          break;
        }
        if (pst.expires_in[0] != '\0' && plex_parse_uint(pst.expires_in) == 0U) {
          ctx->host->format(err, err_size, "The PIN has expired. Start sign-in again.");
          return 0;
        }
      } else if (status == 404) {
        ctx->host->format(err, err_size, "The PIN has expired. Start sign-in again.");
        return 0;
      }

      /* Wait 2 seconds */
      if (!ctx->host->sleep(ctx->host, 2000U)) {
        if (err && err_size > 0U) {
          ctx->host->format(err, err_size, "Cancelled");
        }
        return 0;
      }
    }

    if (!got_token) {
      ctx->host->format(err, err_size, "Sign-in timed out. Start sign-in again.");
      return 0;
    }

    /* Fetch resources */
    if (!plex_fetch_resources(ctx, account_token, err, err_size)) {
      return 0;
    }

    if (ctx->discovered_server_count == 0) {
      ctx->host->format(err, err_size, "No Plex Media Server found on this account");
      return 0;
    }

    if (ctx->discovered_server_count == 1) {
      if (!plex_select_server(ctx, &ctx->discovered_servers[0], err, err_size)) {
        return 0;
      }
      return plex_list_library_top(ctx, start, count, sink, err, err_size);
    } else {
      /* Several servers: list as folders */
      uint32_t emitted = 0;
      uint32_t i;

      if (sink->total) {
        sink->total(sink->user, ctx->discovered_server_count);
      }
      for (i = 0U; i < ctx->discovered_server_count; i++) {
        if (count > 0 && emitted >= count) break;
        if (i >= start) {
          char srv_id[32];
          ZZPlayProviderItem it;
          ctx->host->format(srv_id, sizeof(srv_id), "signin:server:%lu", (unsigned long)i);
          plex_memset(&it, 0, sizeof(it));
          it.id = srv_id;
          it.title = ctx->discovered_servers[i].name;
          it.subtitle = ctx->discovered_servers[i].owned ? "owned" : "shared";
          it.kind = ZZPLAY_ITEM_FOLDER;
          it.playable = 0;
          if (sink->item(sink->user, &it) != 0) {
            return 1;
          }
          emitted++;
        }
      }
      return 1;
    }
  }

  if (plex_strncmp(node, "signin:server:", 14) == 0) {
    uint32_t idx = plex_parse_uint(node + 14);
    if (idx >= ctx->discovered_server_count) {
      ctx->host->format(err, err_size, "Invalid server selection");
      return 0;
    }
    if (!plex_select_server(ctx, &ctx->discovered_servers[idx], err, err_size)) {
      return 0;
    }
    return plex_list_library_top(ctx, start, count, sink, err, err_size);
  }

  if (plex_strcmp(node, "playlists") == 0) {
    int stopped = 0;
    return plex_fetch_items(ctx, "/playlists?playlistType=audio",
                            start, count, count > 0, sink, 0, 0, &stopped, err, err_size);
  }

  if (plex_strncmp(node, "sec:", 4) == 0) {
    char path[64];
    int stopped = 0;
    ctx->host->format(path, sizeof(path), "/library/sections/%s/all?type=8", node + 4);
    return plex_fetch_items(ctx, path, start, count, count > 0,
                            sink, 0, 0, &stopped, err, err_size);
  }

  if (plex_strncmp(node, "art:", 4) == 0) {
    char path[64];
    int stopped = 0;
    ctx->host->format(path, sizeof(path), "/library/metadata/%s/children", node + 4);
    return plex_fetch_items(ctx, path, start, count, count > 0,
                            sink, 0, 0, &stopped, err, err_size);
  }

  if (plex_strncmp(node, "alb:", 4) == 0) {
    char path[64];
    int stopped = 0;
    ctx->host->format(path, sizeof(path), "/library/metadata/%s/children", node + 4);
    return plex_fetch_items(ctx, path, start, count, count > 0,
                            sink, 0, 0, &stopped, err, err_size);
  }

  if (plex_strncmp(node, "pl:", 3) == 0) {
    char path[64];
    int stopped = 0;
    ctx->host->format(path, sizeof(path), "/playlists/%s/items", node + 3);
    return plex_fetch_items(ctx, path, start, count, count > 0,
                            sink, 0, 0, &stopped, err, err_size);
  }

  if (node[0] == '/') {
    int stopped = 0;
    return plex_fetch_items(ctx, node, start, count, count > 0,
                            sink, 0, 0, &stopped, err, err_size);
  }

  ctx->host->format(err, err_size, "Unknown browse node");
  return 0;
}

static int plex_search(void *ctx_in, const char *query, const ZZPlayItemSink *sink,
                      char *err, uint32_t err_size)
{
  PlexContext *ctx = (PlexContext *)ctx_in;
  char esc_query[256];
  uint32_t esc_len;
  static const int types[3] = {10, 9, 8};

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

  for (uint32_t s = 0; s < ctx->section_count; s++) {
    const char *sec_key = ctx->sections[s].key;
    for (int t = 0; t < 3; t++) {
      char path[256];
      int stopped = 0;
      ctx->host->format(path, sizeof(path),
                        "/library/sections/%s/search?type=%d&query=%s",
                        sec_key, types[t], esc_query);
      if (!plex_fetch_items(ctx, path, 0, 0, 0, sink, 1, 0, &stopped, err, err_size)) {
        return 0;
      }
      if (stopped) {
        return 1;
      }
    }
  }
  return 1;
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
  ctx->host->xml_close(ctx->host, xml);

  if (status == 401) {
    ctx->host->format(err, err_size, "Plex rejected the token");
    return 0;
  }
  if (status != 200 && status != 0) {
    ctx->host->format(err, err_size, "Plex answered HTTP %d", status);
    return 0;
  }
  if (!ok) {
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
  if (status == 401) {
    ctx->host->format(err, err_size, "Plex rejected the token");
    return 0;
  }
  if (status != 200 && status != 0) {
    ctx->host->format(err, err_size, "Plex answered HTTP %d", status);
    return 0;
  }
  if (!ok) {
    return 0;
  }

  ulen = ctx->host->format(out->url, (uint32_t)sizeof(out->url),
    "%s/music/:/transcode/universal/start.mp3?%s",
    ctx->server, query);
  if (ulen < 0 || (uint32_t)ulen >= sizeof(out->url)) {
    ctx->host->format(err, err_size, "Resolved URL is too long");
    return 0;
  }

  if (ctx->rst.artist[0] != '\0') {
    ctx->host->format(out->title, sizeof(out->title), "%s - %s", ctx->rst.artist, ctx->rst.title);
  } else {
    plex_strlcpy(out->title, ctx->rst.title, sizeof(out->title));
  }
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
  if (*rk == '\0') {
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

    if (ctx->rst.artist[0] != '\0') {
      ctx->host->format(out->title, sizeof(out->title), "%s - %s", ctx->rst.artist, ctx->rst.title);
    } else {
      plex_strlcpy(out->title, ctx->rst.title, sizeof(out->title));
    }
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
  .resolve = plex_resolve
};

const ZZPlayProvider *zzplay_provider_entry(void)
{
  return &plex_provider;
}
