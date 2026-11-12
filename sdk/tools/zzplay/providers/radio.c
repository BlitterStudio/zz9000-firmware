/* ZZPlay service module for internet radio via radio-browser.info.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Favourites storage format in ENV:ZZPlay/radio.prefs and ENVARC:ZZPlay/radio.prefs:
 *   fav_count=<N>       (number of favourites, up to 64)
 *   fav_<index>=<uuid>|<name>|<codec>|<bitrate>|<country>
 * Example:
 *   fav_count=2
 *   fav_0=ffc0d701-f43b-4232-8be6-f7ff72add8ef|Blasmusikradio|MP3|128|Germany
 *   fav_1=044acc55-255b-4382-af22-ccf28445bb3e|Radio Capital|AAC+|122|Italy
 */

#include "../zzplay-provider.h"

#if defined(__amigaos__) || defined(AMIGA)
void *memcpy(void *dest, const void *src, size_t n)
{
  char *d = (char *)dest;
  const char *s = (const char *)src;
  while (n--) {
    *d++ = *s++;
  }
  return dest;
}

void *memset(void *s, int c, size_t n)
{
  char *p = (char *)s;
  while (n--) {
    *p++ = (char)c;
  }
  return s;
}

size_t strlen(const char *s)
{
  const char *p = s;
  while (*p) {
    p++;
  }
  return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2)
{
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0) {
    return 0;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strncpy(char *dest, const char *src, size_t n)
{
  size_t i;
  for (i = 0U; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

char *strchr(const char *s, int c)
{
  while (*s != (char)c) {
    if (!*s++) {
      return 0;
    }
  }
  return (char *)s;
}

char *strstr(const char *haystack, const char *needle)
{
  size_t nlen;
  if (!*needle) {
    return (char *)haystack;
  }
  nlen = strlen(needle);
  while (*haystack) {
    if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0) {
      return (char *)haystack;
    }
    haystack++;
  }
  return 0;
}
#else
#include <string.h>
#endif

#define RADIO_API_BASE "http://de1.api.radio-browser.info/xml"
#define RADIO_MAX_FAVOURITES 64U
#define RADIO_SEARCH_LIMIT 100U
#define RADIO_UUID_MAX 40U

typedef struct FavouriteEntry {
  char uuid[RADIO_UUID_MAX];
  char name[ZZPLAY_PROVIDER_TEXT_MAX];
  char codec[16];
  char bitrate[16];
  char country[64];
} FavouriteEntry;

typedef struct RadioContext {
  const ZZPlayHost *host;
  uint32_t fav_count;
  FavouriteEntry favs[RADIO_MAX_FAVOURITES];
} RadioContext;

static char radio_lower(char c)
{
  if (c >= 'A' && c <= 'Z') {
    return (char)(c - 'A' + 'a');
  }
  return c;
}

static int radio_strcasecmp(const char *s1, const char *s2)
{
  while (*s1 && (radio_lower(*s1) == radio_lower(*s2))) {
    s1++;
    s2++;
  }
  return (unsigned char)radio_lower(*s1) - (unsigned char)radio_lower(*s2);
}

static int radio_is_codec_supported(const char *codec)
{
  if (!codec || !*codec) {
    return 0;
  }
  if (radio_strcasecmp(codec, "MP3") == 0) {
    return 1;
  }
  if (radio_strcasecmp(codec, "OGG") == 0 ||
      radio_strcasecmp(codec, "Vorbis") == 0 ||
      radio_strcasecmp(codec, "Ogg Vorbis") == 0) {
    return 1;
  }
  return 0;
}

static const char *radio_trim(const char *s)
{
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
    s++;
  }
  return s;
}

/* Length of the first `len` bytes of `s` without a UTF-8 sequence that a
 * cut at `len` left incomplete. */
static size_t radio_utf8_cut(const char *s, size_t len)
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

/* Copy text the directory sent for showing or storing: control characters
 * become spaces, and so does '|' when `bar` is set (it separates the
 * fields of a stored favourite). A longer text is cut without splitting a
 * UTF-8 sequence. */
static void radio_copy_text(char *out, size_t size, const char *src, int bar)
{
  size_t len = 0U;
  size_t i;

  if (size == 0U) {
    return;
  }
  if (!src) {
    src = "";
  }
  while (len < size - 1U && src[len] != '\0') {
    len++;
  }
  if (src[len] != '\0') {
    len = radio_utf8_cut(src, len);
  }
  for (i = 0U; i < len; i++) {
    unsigned char c = (unsigned char)src[i];

    out[i] = (c < 0x20U || c == 0x7FU || (bar && c == '|')) ? ' ' : (char)c;
  }
  out[len] = '\0';
}

/* radio-browser station ids are UUIDs. Anything else cannot be resolved
 * or kept as a favourite. */
static int radio_valid_uuid(const char *s)
{
  size_t n;

  if (!s) {
    return 0;
  }
  for (n = 0U; s[n] != '\0'; n++) {
    char c = s[n];

    if (n + 1U >= RADIO_UUID_MAX ||
        !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c == '-')) {
      return 0;
    }
  }
  return n != 0U;
}

static void radio_format_station_subtitle(const ZZPlayHost *host,
                                          const char *codec,
                                          const char *bitrate,
                                          const char *country,
                                          int playable,
                                          char *out,
                                          uint32_t out_size)
{
  const char *cd = (codec && *codec) ? codec : "unknown";
  const char *ct = (country && *country) ? country : "";
  const char *br = (bitrate && *bitrate && strcmp(bitrate, "0") != 0) ? bitrate : "";
  char text[256];
  const uint32_t ts = (uint32_t)sizeof(text);

  if (!playable) {
    if (*br && *ct) {
      host->format(text, ts, "Unsupported (%s), %s kbps, %s", cd, br, ct);
    } else if (*ct) {
      host->format(text, ts, "Unsupported (%s), %s", cd, ct);
    } else if (*br) {
      host->format(text, ts, "Unsupported (%s), %s kbps", cd, br);
    } else {
      host->format(text, ts, "Unsupported (%s)", cd);
    }
  } else {
    if (*br && *ct) {
      host->format(text, ts, "%s, %s kbps, %s", cd, br, ct);
    } else if (*ct) {
      host->format(text, ts, "%s, %s", cd, ct);
    } else if (*br) {
      host->format(text, ts, "%s, %s kbps", cd, br);
    } else {
      host->format(text, ts, "%s", cd);
    }
  }
  radio_copy_text(out, out_size, text, 0);
}

static void parse_subtitle(const char *sub, char *codec, size_t codec_sz,
                           char *bitrate, size_t bitrate_sz,
                           char *country, size_t country_sz)
{
  const char *p;
  char part1[64];
  char part2[64];
  char part3[64];
  size_t l;

  codec[0] = '\0';
  bitrate[0] = '\0';
  country[0] = '\0';

  if (!sub || !*sub) {
    strncpy(codec, "MP3", codec_sz - 1U);
    codec[codec_sz - 1U] = '\0';
    strncpy(bitrate, "128", bitrate_sz - 1U);
    bitrate[bitrate_sz - 1U] = '\0';
    return;
  }

  part1[0] = '\0';
  part2[0] = '\0';
  part3[0] = '\0';
  p = strstr(sub, ", ");
  if (!p) {
    strncpy(part1, sub, sizeof(part1) - 1U);
    part1[sizeof(part1) - 1U] = '\0';
  } else {
    l = (size_t)(p - sub);
    if (l >= sizeof(part1)) {
      l = sizeof(part1) - 1U;
    }
    memcpy(part1, sub, l);
    part1[l] = '\0';
    sub = p + 2;
    p = strstr(sub, ", ");
    if (!p) {
      strncpy(part2, sub, sizeof(part2) - 1U);
      part2[sizeof(part2) - 1U] = '\0';
    } else {
      l = (size_t)(p - sub);
      if (l >= sizeof(part2)) {
        l = sizeof(part2) - 1U;
      }
      memcpy(part2, sub, l);
      part2[l] = '\0';
      strncpy(part3, p + 2, sizeof(part3) - 1U);
      part3[sizeof(part3) - 1U] = '\0';
    }
  }

  if (strncmp(part1, "Unsupported (", 13) == 0) {
    const char *start = part1 + 13;
    const char *end = strchr(start, ')');
    if (end) {
      l = (size_t)(end - start);
      if (l >= codec_sz) {
        l = codec_sz - 1U;
      }
      memcpy(codec, start, l);
      codec[l] = '\0';
    } else {
      strncpy(codec, start, codec_sz - 1U);
      codec[codec_sz - 1U] = '\0';
    }
  } else {
    strncpy(codec, part1, codec_sz - 1U);
    codec[codec_sz - 1U] = '\0';
  }

  if (part2[0]) {
    char *kb = strstr(part2, " kbps");
    if (kb) {
      *kb = '\0';
      strncpy(bitrate, part2, bitrate_sz - 1U);
      bitrate[bitrate_sz - 1U] = '\0';
      if (part3[0]) {
        strncpy(country, part3, country_sz - 1U);
        country[country_sz - 1U] = '\0';
      }
    } else {
      strncpy(country, part2, country_sz - 1U);
      country[country_sz - 1U] = '\0';
    }
  }
}

static int radio_is_favourite(const RadioContext *rc, const char *uuid)
{
  uint32_t i;
  if (!rc || !uuid || !*uuid) {
    return 0;
  }
  for (i = 0U; i < rc->fav_count; i++) {
    if (strcmp(rc->favs[i].uuid, uuid) == 0) {
      return 1;
    }
  }
  return 0;
}

static void radio_load_favourites(RadioContext *rc)
{
  const ZZPlayHost *host = rc->host;
  char key[32];
  char line[256];
  uint32_t i;

  rc->fav_count = 0U;
  for (i = 0U; i < RADIO_MAX_FAVOURITES; i++) {
    char *parts[5];
    char *p;
    int part_count = 0;

    host->format(key, sizeof(key), "fav_%u", i);
    if (!host->pref_get(host, key, line, sizeof(line)) || !line[0]) {
      break;
    }

    p = line;
    parts[part_count++] = p;
    while (*p && part_count < 5) {
      if (*p == '|') {
        *p = '\0';
        parts[part_count++] = p + 1;
      }
      p++;
    }

    if (part_count >= 1 && parts[0][0]) {
      const char *uuid = parts[0];
      const char *name = (part_count > 1) ? parts[1] : "";
      const char *codec = (part_count > 2) ? parts[2] : "MP3";
      const char *bitrate = (part_count > 3) ? parts[3] : "128";
      const char *country = (part_count > 4) ? parts[4] : "";

      name = radio_trim(name);
      strncpy(rc->favs[rc->fav_count].uuid, uuid,
              sizeof(rc->favs[0].uuid) - 1U);
      rc->favs[rc->fav_count].uuid[sizeof(rc->favs[0].uuid) - 1U] = '\0';

      strncpy(rc->favs[rc->fav_count].name, name,
              sizeof(rc->favs[0].name) - 1U);
      rc->favs[rc->fav_count].name[sizeof(rc->favs[0].name) - 1U] = '\0';

      strncpy(rc->favs[rc->fav_count].codec, codec,
              sizeof(rc->favs[0].codec) - 1U);
      rc->favs[rc->fav_count].codec[sizeof(rc->favs[0].codec) - 1U] = '\0';

      strncpy(rc->favs[rc->fav_count].bitrate, bitrate,
              sizeof(rc->favs[0].bitrate) - 1U);
      rc->favs[rc->fav_count].bitrate[sizeof(rc->favs[0].bitrate) - 1U] = '\0';

      strncpy(rc->favs[rc->fav_count].country, country,
              sizeof(rc->favs[0].country) - 1U);
      rc->favs[rc->fav_count].country[sizeof(rc->favs[0].country) - 1U] = '\0';

      rc->fav_count++;
    }
  }
}

/* XML streaming data receiver */
typedef struct XmlDataFeed {
  const ZZPlayHost *host;
  void *xml;
  int stopped;
} XmlDataFeed;

static int xml_data_feed_cb(void *user, const uint8_t *bytes, uint32_t len)
{
  XmlDataFeed *feed = (XmlDataFeed *)user;
  if (feed->stopped) {
    return 1;
  }
  if (feed->host->xml_feed(feed->host, feed->xml, (const char *)bytes, len) != 0) {
    feed->stopped = 1;
    return 1;
  }
  return 0;
}

/* Stations collector. `saw_root` tells a directory reply (an empty
 * <result/> included) from an error page or garbage. */
typedef struct StationCollector {
  RadioContext *rc;
  const ZZPlayHost *host;
  const ZZPlayItemSink *sink;
  uint32_t count;
  int stopped;
  int saw_root;
} StationCollector;

static int station_xml_start(void *user, const char *name,
                             const ZZPlayHostXmlAttr *attrs, uint32_t attr_count,
                             int truncated)
{
  StationCollector *col = (StationCollector *)user;
  const char *uuid;
  const char *st_name;
  const char *codec;
  const char *bitrate;
  const char *country;
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  ZZPlayProviderItem item;
  int playable;
  (void)truncated;

  if (strcmp(name, "result") == 0) {
    col->saw_root = 1;
  }
  if (col->stopped || strcmp(name, "station") != 0) {
    return col->stopped;
  }

  uuid = col->host->xml_attr(attrs, attr_count, "stationuuid");
  st_name = col->host->xml_attr(attrs, attr_count, "name");
  codec = col->host->xml_attr(attrs, attr_count, "codec");
  bitrate = col->host->xml_attr(attrs, attr_count, "bitrate");
  country = col->host->xml_attr(attrs, attr_count, "country");

  if (!radio_valid_uuid(uuid) || !st_name) {
    return 0;
  }

  st_name = radio_trim(st_name);
  radio_copy_text(title, sizeof(title), st_name, 0);

  playable = radio_is_codec_supported(codec);
  radio_format_station_subtitle(col->host, codec, bitrate, country, playable,
                                subtitle, sizeof(subtitle));

  memset(&item, 0, sizeof(item));
  item.id = uuid;
  item.title = title;
  item.subtitle = subtitle;
  item.duration_ms = 0U;
  item.kind = ZZPLAY_ITEM_STATION;
  item.playable = (uint32_t)playable;
  item.favourite = (uint32_t)radio_is_favourite(col->rc, uuid);

  if (col->sink && col->sink->item) {
    if (col->sink->item(col->sink->user, &item) != 0) {
      col->stopped = 1;
      return 1;
    }
  }
  col->count++;
  return 0;
}

/* Tags / Countries collector */
typedef struct FolderCollector {
  const ZZPlayHost *host;
  const ZZPlayItemSink *sink;
  const char *prefix; /* "tag:" or "country:" */
  uint32_t count;
  int stopped;
  int saw_root;
} FolderCollector;

static int folder_xml_start(void *user, const char *name,
                            const ZZPlayHostXmlAttr *attrs, uint32_t attr_count,
                            int truncated)
{
  FolderCollector *col = (FolderCollector *)user;
  const char *elem_name;
  const char *stationcount;
  char id[ZZPLAY_PROVIDER_TEXT_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  ZZPlayProviderItem item;
  (void)truncated;

  if (strcmp(name, "result") == 0) {
    col->saw_root = 1;
  }
  if (col->stopped) {
    return 1;
  }

  if (strcmp(name, "tag") != 0 && strcmp(name, "country") != 0 &&
      strcmp(name, "language") != 0) {
    return 0;
  }

  elem_name = col->host->xml_attr(attrs, attr_count, "name");
  stationcount = col->host->xml_attr(attrs, attr_count, "stationcount");

  if (!elem_name || !*elem_name) {
    return 0;
  }

  elem_name = radio_trim(elem_name);
  {
    /* The id comes back to list(); a cut one would name another folder. */
    int n = col->host->format(id, sizeof(id), "%s%s", col->prefix, elem_name);

    if (n < 0 || (uint32_t)n >= sizeof(id)) {
      return 0;
    }
  }
  radio_copy_text(title, sizeof(title), elem_name, 0);
  if (stationcount && *stationcount) {
    char count_text[ZZPLAY_PROVIDER_TEXT_MAX];

    col->host->format(count_text, sizeof(count_text), "%s stations",
                      stationcount);
    radio_copy_text(subtitle, sizeof(subtitle), count_text, 0);
  } else {
    subtitle[0] = '\0';
  }

  memset(&item, 0, sizeof(item));
  item.id = id;
  item.title = title;
  item.subtitle = subtitle;
  item.duration_ms = 0U;
  item.kind = ZZPLAY_ITEM_FOLDER;
  item.playable = 0U;
  item.favourite = 0U;

  if (col->sink && col->sink->item) {
    if (col->sink->item(col->sink->user, &item) != 0) {
      col->stopped = 1;
      return 1;
    }
  }
  col->count++;
  return 0;
}

/* Resolve collector */
typedef struct ResolveCollector {
  const ZZPlayHost *host;
  const char *target_uuid;
  char url[ZZPLAY_PROVIDER_URL_MAX];
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char codec[32];
  int found;
  int saw_root;
} ResolveCollector;

static int resolve_xml_start(void *user, const char *name,
                             const ZZPlayHostXmlAttr *attrs, uint32_t attr_count,
                             int truncated)
{
  ResolveCollector *col = (ResolveCollector *)user;
  const char *uuid;
  const char *st_name;
  const char *codec;
  const char *url;
  const char *url_resolved;
  (void)truncated;

  if (strcmp(name, "result") == 0) {
    col->saw_root = 1;
  }
  if (col->found || strcmp(name, "station") != 0) {
    return col->found;
  }

  uuid = col->host->xml_attr(attrs, attr_count, "stationuuid");
  st_name = col->host->xml_attr(attrs, attr_count, "name");
  codec = col->host->xml_attr(attrs, attr_count, "codec");
  url_resolved = col->host->xml_attr(attrs, attr_count, "url_resolved");
  url = col->host->xml_attr(attrs, attr_count, "url");

  if (!uuid || (col->target_uuid && strcmp(uuid, col->target_uuid) != 0)) {
    return 0;
  }

  if (st_name) {
    st_name = radio_trim(st_name);
    radio_copy_text(col->title, sizeof(col->title), st_name, 0);
  }
  if (codec) {
    radio_copy_text(col->codec, sizeof(col->codec), codec, 0);
  }
  if (!url_resolved || !*url_resolved) {
    url_resolved = url;
  }
  if (url_resolved && *url_resolved) {
    /* A cut address would be another address: keep only one that fits. */
    int n = col->host->format(col->url, sizeof(col->url), "%s", url_resolved);

    if (n < 0 || (uint32_t)n >= sizeof(col->url)) {
      col->url[0] = '\0';
    }
  }

  col->found = 1;
  return 1;
}

static void *radio_open(const ZZPlayHost *host, char *err, uint32_t err_size)
{
  RadioContext *ctx;
  if (!host) {
    if (err && err_size > 0U) {
      err[0] = '\0';
    }
    return NULL;
  }
  ctx = (RadioContext *)host->alloc(host, (uint32_t)sizeof(RadioContext));
  if (!ctx) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Out of memory");
    }
    return NULL;
  }
  ctx->host = host;
  radio_load_favourites(ctx);
  return ctx;
}

static void radio_close(void *ctx)
{
  RadioContext *rc = (RadioContext *)ctx;
  if (rc && rc->host) {
    rc->host->free(rc->host, rc);
  }
}

static int list_favourites(RadioContext *rc, uint32_t start, uint32_t count,
                           const ZZPlayItemSink *sink)
{
  const ZZPlayHost *host = rc->host;
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
  uint32_t i;
  uint32_t delivered = 0U;

  if (sink && sink->total) {
    sink->total(sink->user, rc->fav_count);
  }

  for (i = start; i < rc->fav_count && delivered < count; i++) {
    int playable = radio_is_codec_supported(rc->favs[i].codec);
    ZZPlayProviderItem item;

    radio_copy_text(title, sizeof(title), rc->favs[i].name, 0);
    radio_format_station_subtitle(host, rc->favs[i].codec, rc->favs[i].bitrate,
                                  rc->favs[i].country, playable, subtitle,
                                  sizeof(subtitle));

    memset(&item, 0, sizeof(item));
    item.id = rc->favs[i].uuid;
    item.title = title;
    item.subtitle = subtitle;
    item.duration_ms = 0U;
    item.kind = ZZPLAY_ITEM_STATION;
    item.playable = (uint32_t)playable;
    item.favourite = 1U;

    if (sink && sink->item) {
      if (sink->item(sink->user, &item) != 0) {
        break;
      }
    }
    delivered++;
  }

  return 1;
}

/* 1 when the reply fed to `xml` was a whole document. A host older than
 * xml_finish cannot tell, so its replies count as whole. */
static int radio_xml_complete(const ZZPlayHost *host, void *xml)
{
  if (host->size < (uint32_t)(offsetof(ZZPlayHost, xml_finish) +
                              sizeof(host->xml_finish)) ||
      host->xml_finish == NULL) {
    return 1;
  }
  return host->xml_finish(host, xml);
}

/* After a directory request: a failed transfer keeps the host's reason
 * (or gets one), and a reply without the <result> root is an error page
 * or garbage, not an empty directory. */
static int radio_reply_ok(const ZZPlayHost *host, int ok, int status,
                          int saw_root, char *err, uint32_t err_size)
{
  if (!ok) {
    if (err && err_size > 0U && err[0] == '\0') {
      if (status != 0) {
        host->format(err, err_size, "The station directory answered HTTP %d",
                     status);
      } else {
        host->format(err, err_size, "The station directory did not answer");
      }
    }
    return 0;
  }
  if (!saw_root) {
    if (err && err_size > 0U) {
      host->format(err, err_size,
                   "The station directory sent an unreadable reply");
    }
    return 0;
  }
  return 1;
}

static int fetch_and_scan_stations(RadioContext *rc, const char *url,
                                   const ZZPlayItemSink *sink, char *err,
                                   uint32_t err_size)
{
  const ZZPlayHost *host = rc->host;
  ZZPlayHostXmlHandler handler;
  StationCollector col;
  XmlDataFeed feed;
  void *xml;
  int status = 0;
  int ok;
  int whole;

  memset(&col, 0, sizeof(col));
  col.rc = rc;
  col.host = host;
  col.sink = sink;

  memset(&handler, 0, sizeof(handler));
  handler.start = station_xml_start;
  handler.user = &col;

  xml = host->xml_open(host, &handler);
  if (!xml) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Failed to initialize XML parser");
    }
    return 0;
  }

  feed.host = host;
  feed.xml = xml;
  feed.stopped = 0;

  ok = host->http_get(host, url, NULL, xml_data_feed_cb, &feed, &status, err,
                      err_size);
  whole = radio_xml_complete(host, xml);
  host->xml_close(host, xml);

  return radio_reply_ok(host, ok, status, col.saw_root && whole, err,
                        err_size);
}

static int fetch_and_scan_folders(RadioContext *rc, const char *url,
                                  const char *prefix,
                                  const ZZPlayItemSink *sink, char *err,
                                  uint32_t err_size)
{
  const ZZPlayHost *host = rc->host;
  ZZPlayHostXmlHandler handler;
  FolderCollector col;
  XmlDataFeed feed;
  void *xml;
  int status = 0;
  int ok;
  int whole;

  memset(&col, 0, sizeof(col));
  col.host = host;
  col.sink = sink;
  col.prefix = prefix;

  memset(&handler, 0, sizeof(handler));
  handler.start = folder_xml_start;
  handler.user = &col;

  xml = host->xml_open(host, &handler);
  if (!xml) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Failed to initialize XML parser");
    }
    return 0;
  }

  feed.host = host;
  feed.xml = xml;
  feed.stopped = 0;

  ok = host->http_get(host, url, NULL, xml_data_feed_cb, &feed, &status, err,
                      err_size);
  whole = radio_xml_complete(host, xml);
  host->xml_close(host, xml);

  return radio_reply_ok(host, ok, status, col.saw_root && whole, err,
                        err_size);
}

static int radio_list(void *ctx, const char *node, uint32_t start, uint32_t count,
                      const ZZPlayItemSink *sink, char *err, uint32_t err_size)
{
  RadioContext *rc = (RadioContext *)ctx;
  const ZZPlayHost *host;
  char url[512];

  if (!rc || !rc->host) {
    return 0;
  }
  host = rc->host;

  if (count == 0U) {
    count = 100U;
  }

  if (!node || !*node) {
    static const struct {
      const char *id;
      const char *title;
    } roots[4] = {
      {"fav", "Favourites"},
      {"top", "Top voted"},
      {"tags", "By tag"},
      {"countries", "By country"}
    };
    uint32_t i;

    if (sink && sink->total) {
      sink->total(sink->user, 4U);
    }
    for (i = start; i < 4U && (i - start) < count; i++) {
      ZZPlayProviderItem item;
      memset(&item, 0, sizeof(item));
      item.id = roots[i].id;
      item.title = roots[i].title;
      item.subtitle = "";
      item.duration_ms = 0U;
      item.kind = ZZPLAY_ITEM_FOLDER;
      item.playable = 0U;
      item.favourite = 0U;
      if (sink && sink->item) {
        if (sink->item(sink->user, &item) != 0) {
          break;
        }
      }
    }
    return 1;
  }

  if (strcmp(node, "fav") == 0) {
    return list_favourites(rc, start, count, sink);
  }

  if (strcmp(node, "top") == 0) {
    host->format(url, sizeof(url),
                 "%s/stations/topvote?offset=%u&limit=%u&hidebroken=true",
                 RADIO_API_BASE, start, count);
    return fetch_and_scan_stations(rc, url, sink, err, err_size);
  }

  if (strcmp(node, "tags") == 0) {
    host->format(url, sizeof(url),
                 "%s/tags?order=stationcount&reverse=true&offset=%u&limit=%u",
                 RADIO_API_BASE, start, count);
    return fetch_and_scan_folders(rc, url, "tag:", sink, err, err_size);
  }

  if (strncmp(node, "tag:", 4) == 0) {
    char escaped[128];
    if (host->url_escape(node + 4, escaped, sizeof(escaped)) == 0U) {
      if (err && err_size > 0U) {
        host->format(err, err_size, "The tag name is empty or too long");
      }
      return 0;
    }
    host->format(url, sizeof(url),
                 "%s/stations/bytag/%s?order=votes&reverse=true&offset=%u&limit=%u&hidebroken=true",
                 RADIO_API_BASE, escaped, start, count);
    return fetch_and_scan_stations(rc, url, sink, err, err_size);
  }

  if (strcmp(node, "countries") == 0) {
    host->format(url, sizeof(url),
                 "%s/countries?order=stationcount&reverse=true&offset=%u&limit=%u",
                 RADIO_API_BASE, start, count);
    return fetch_and_scan_folders(rc, url, "country:", sink, err, err_size);
  }

  if (strncmp(node, "country:", 8) == 0) {
    char escaped[128];
    if (host->url_escape(node + 8, escaped, sizeof(escaped)) == 0U) {
      if (err && err_size > 0U) {
        host->format(err, err_size, "The country name is empty or too long");
      }
      return 0;
    }
    host->format(url, sizeof(url),
                 "%s/stations/bycountry/%s?order=votes&reverse=true&offset=%u&limit=%u&hidebroken=true",
                 RADIO_API_BASE, escaped, start, count);
    return fetch_and_scan_stations(rc, url, sink, err, err_size);
  }

  if (err && err_size > 0U) {
    host->format(err, err_size, "Unknown folder: %s", node);
  }
  return 0;
}

static int radio_search(void *ctx, const char *query, const ZZPlayItemSink *sink,
                        char *err, uint32_t err_size)
{
  RadioContext *rc = (RadioContext *)ctx;
  const ZZPlayHost *host;
  char escaped[128];
  char url[512];

  if (!rc || !rc->host || !query) {
    return 0;
  }
  host = rc->host;

  if (host->url_escape(query, escaped, sizeof(escaped)) == 0U &&
      query[0] != '\0') {
    if (err && err_size > 0U) {
      host->format(err, err_size, "The search text is too long");
    }
    return 0;
  }
  host->format(url, sizeof(url),
               "%s/stations/search?name=%s&hidebroken=true&order=votes&reverse=true&limit=%u",
               RADIO_API_BASE, escaped, RADIO_SEARCH_LIMIT);

  return fetch_and_scan_stations(rc, url, sink, err, err_size);
}

static int radio_resolve(void *ctx, const char *item, ZZPlayResolved *out,
                         char *err, uint32_t err_size)
{
  RadioContext *rc = (RadioContext *)ctx;
  const ZZPlayHost *host;
  /* The id is a checked UUID (under 40 safe characters): these hold the
   * whole request. */
  char escaped[48];
  char url[128];
  ZZPlayHostXmlHandler handler;
  ResolveCollector col;
  XmlDataFeed feed;
  void *xml;
  int status = 0;
  int ok;
  int whole;

  if (!rc || !rc->host || !item || !out) {
    return 0;
  }
  host = rc->host;
  if (!radio_valid_uuid(item)) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Not a radio station id");
    }
    return 0;
  }

  memset(out, 0, sizeof(*out));
  memset(&col, 0, sizeof(col));
  col.host = host;
  col.target_uuid = item;

  memset(&handler, 0, sizeof(handler));
  handler.start = resolve_xml_start;
  handler.user = &col;

  xml = host->xml_open(host, &handler);
  if (!xml) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Failed to initialize XML parser");
    }
    return 0;
  }

  feed.host = host;
  feed.xml = xml;
  feed.stopped = 0;

  host->url_escape(item, escaped, sizeof(escaped));
  host->format(url, sizeof(url), "%s/stations/byuuid/%s", RADIO_API_BASE, escaped);

  ok = host->http_get(host, url, NULL, xml_data_feed_cb, &feed, &status, err,
                      err_size);
  whole = radio_xml_complete(host, xml);
  host->xml_close(host, xml);

  if (!radio_reply_ok(host, ok, status, col.saw_root && whole, err,
                      err_size)) {
    return 0;
  }

  if (!col.found) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Station not found");
    }
    return 0;
  }

  if (col.url[0] == '\0') {
    if (err && err_size > 0U) {
      host->format(err, err_size, "The station has no usable stream address");
    }
    return 0;
  }

  if (!radio_is_codec_supported(col.codec)) {
    if (err && err_size > 0U) {
      host->format(err, err_size, "Unsupported codec: %s",
                   col.codec[0] ? col.codec : "unknown");
    }
    return 0;
  }

  host->format(out->url, sizeof(out->url), "%s", col.url);
  host->format(out->title, sizeof(out->title), "%s", col.title);
  out->duration_ms = 0U;

  return 1;
}

/* Store favourite `index` as its prefs line. Returns 0 when it could not
 * be saved. */
static int radio_store_favourite(const ZZPlayHost *host, uint32_t index,
                                 const FavouriteEntry *fav)
{
  char key[32];
  char val[256];

  host->format(key, sizeof(key), "fav_%u", index);
  host->format(val, sizeof(val), "%s|%s|%s|%s|%s", fav->uuid, fav->name,
               fav->codec, fav->bitrate, fav->country);
  return host->pref_set(host, key, val) != 0;
}

static int radio_save_failed(const ZZPlayHost *host, char *err,
                             uint32_t err_size)
{
  if (err && err_size > 0U) {
    host->format(err, err_size, "Could not save the settings");
  }
  return 0;
}

static int radio_favourite(void *ctx, const ZZPlayProviderItem *item, int add,
                           char *err, uint32_t err_size)
{
  RadioContext *rc = (RadioContext *)ctx;
  const ZZPlayHost *host;
  uint32_t i;
  char key[32];
  char val[256];

  if (!rc || !rc->host || !item || !item->id || !*item->id) {
    return 0;
  }
  host = rc->host;

  if (add) {
    char codec[16];
    char bitrate[16];
    char country[64];
    const char *name;

    for (i = 0U; i < rc->fav_count; i++) {
      if (strcmp(rc->favs[i].uuid, item->id) == 0) {
        return 1;
      }
    }

    if (rc->fav_count >= RADIO_MAX_FAVOURITES) {
      if (err && err_size > 0U) {
        host->format(err, err_size, "The favourites list is full (64 stations)");
      }
      return 0;
    }
    if (!radio_valid_uuid(item->id)) {
      if (err && err_size > 0U) {
        host->format(err, err_size, "Not a radio station id");
      }
      return 0;
    }

    parse_subtitle(item->subtitle, codec, sizeof(codec), bitrate, sizeof(bitrate),
                   country, sizeof(country));

    name = item->title ? item->title : "";
    name = radio_trim(name);

    /* Stored as one prefs line of '|'-separated fields: a line break or a
     * '|' in a field would corrupt it. */
    radio_copy_text(rc->favs[rc->fav_count].uuid,
                    sizeof(rc->favs[0].uuid), item->id, 1);
    radio_copy_text(rc->favs[rc->fav_count].name,
                    sizeof(rc->favs[0].name), name, 1);
    radio_copy_text(rc->favs[rc->fav_count].codec,
                    sizeof(rc->favs[0].codec), codec, 1);
    radio_copy_text(rc->favs[rc->fav_count].bitrate,
                    sizeof(rc->favs[0].bitrate), bitrate, 1);
    radio_copy_text(rc->favs[rc->fav_count].country,
                    sizeof(rc->favs[0].country), country, 1);

    /* Shown as saved only once it is: the count stays until both lines
     * are stored. */
    if (!radio_store_favourite(host, rc->fav_count,
                               &rc->favs[rc->fav_count])) {
      return radio_save_failed(host, err, err_size);
    }
    host->format(val, sizeof(val), "%u", rc->fav_count + 1U);
    if (!host->pref_set(host, "fav_count", val)) {
      host->format(key, sizeof(key), "fav_%u", rc->fav_count);
      (void)host->pref_set(host, key, "");
      return radio_save_failed(host, err, err_size);
    }
    rc->fav_count++;
    return 1;
  } else {
    int found_idx = -1;

    for (i = 0U; i < rc->fav_count; i++) {
      if (strcmp(rc->favs[i].uuid, item->id) == 0) {
        found_idx = (int)i;
        break;
      }
    }

    if (found_idx < 0) {
      return 1;
    }

    /* Store the shortened list first; the list in memory changes only
     * when every line was saved. */
    for (i = (uint32_t)found_idx; i + 1U < rc->fav_count; i++) {
      if (!radio_store_favourite(host, i, &rc->favs[i + 1U])) {
        return radio_save_failed(host, err, err_size);
      }
    }
    host->format(key, sizeof(key), "fav_%u", rc->fav_count - 1U);
    host->format(val, sizeof(val), "%u", rc->fav_count - 1U);
    if (!host->pref_set(host, key, "") ||
        !host->pref_set(host, "fav_count", val)) {
      return radio_save_failed(host, err, err_size);
    }

    for (i = (uint32_t)found_idx; i + 1U < rc->fav_count; i++) {
      rc->favs[i] = rc->favs[i + 1U];
    }
    rc->fav_count--;
    return 1;
  }
}

static const ZZPlayProvider radio_provider = {
  ZZPLAY_PROVIDER_ID,
  ZZPLAY_PROVIDER_ABI,
  (uint32_t)sizeof(ZZPlayProvider),
  "radio",
  "Internet radio",
  NULL,
  0U,
  radio_open,
  radio_close,
  radio_list,
  radio_search,
  radio_resolve,
  radio_favourite
};

const ZZPlayProvider *zzplay_provider_entry(void)
{
  return &radio_provider;
}
