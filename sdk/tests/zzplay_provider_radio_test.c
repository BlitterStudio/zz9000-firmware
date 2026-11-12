/* Host test for zzplay internet radio provider (radio.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-host.h"
#include "../tools/zzplay/zzplay-browse-model.h"
#include "../tools/zzplay/zzplay-tags.h"
#include "../tools/zzplay/zzplay-xml.h"

#define MAX_PREFS 128

typedef struct PrefEntry {
  char key[32];
  char value[256];
} PrefEntry;

static PrefEntry s_prefs[MAX_PREFS];
static int s_pref_count = 0;

static int fake_pref_get(const ZZPlayHost *host, const char *key, char *out,
                         uint32_t size)
{
  int i;
  (void)host;
  if (out && size > 0U) {
    out[0] = '\0';
  }
  if (!key || !out || size == 0U) {
    return 0;
  }
  for (i = 0; i < s_pref_count; i++) {
    if (strcmp(s_prefs[i].key, key) == 0) {
      strncpy(out, s_prefs[i].value, size - 1U);
      out[size - 1U] = '\0';
      return 1;
    }
  }
  return 0;
}

static int fake_pref_set(const ZZPlayHost *host, const char *key,
                         const char *value)
{
  int i;
  (void)host;
  if (!key || !value) {
    return 0;
  }
  for (i = 0; i < s_pref_count; i++) {
    if (strcmp(s_prefs[i].key, key) == 0) {
      strncpy(s_prefs[i].value, value, sizeof(s_prefs[i].value) - 1U);
      s_prefs[i].value[sizeof(s_prefs[i].value) - 1U] = '\0';
      return 1;
    }
  }
  if (s_pref_count < MAX_PREFS) {
    strncpy(s_prefs[s_pref_count].key, key, sizeof(s_prefs[0].key) - 1U);
    s_prefs[s_pref_count].key[sizeof(s_prefs[0].key) - 1U] = '\0';
    strncpy(s_prefs[s_pref_count].value, value, sizeof(s_prefs[0].value) - 1U);
    s_prefs[s_pref_count].value[sizeof(s_prefs[0].value) - 1U] = '\0';
    s_pref_count++;
    return 1;
  }
  return 0;
}

static void *fake_alloc(const ZZPlayHost *host, uint32_t size)
{
  (void)host;
  return calloc(1U, (size_t)size);
}

static void fake_free(const ZZPlayHost *host, void *ptr)
{
  (void)host;
  free(ptr);
}

static int fake_format(char *out, uint32_t size, const char *fmt, ...)
{
  va_list args;
  int res;
  if (!out || size == 0U || !fmt) {
    return -1;
  }
  va_start(args, fmt);
  res = vsnprintf(out, (size_t)size, fmt, args);
  va_end(args);
  return res;
}

typedef struct FakeXmlWrapper {
  ZZPlayXml xml;
  ZZPlayHostXmlHandler handler;
} FakeXmlWrapper;

static int fake_xml_start(void *user, const char *name,
                          const ZZPlayXmlAttr *attrs, uint32_t count,
                          int truncated)
{
  FakeXmlWrapper *w = (FakeXmlWrapper *)user;
  if (!w->handler.start) {
    return 0;
  }
  return w->handler.start(w->handler.user, name,
                          (const ZZPlayHostXmlAttr *)attrs, count, truncated);
}

static int fake_xml_end(void *user, const char *name)
{
  FakeXmlWrapper *w = (FakeXmlWrapper *)user;
  if (!w->handler.end) {
    return 0;
  }
  return w->handler.end(w->handler.user, name);
}

static void *fake_xml_open(const ZZPlayHost *host,
                           const ZZPlayHostXmlHandler *handler)
{
  FakeXmlWrapper *w;
  ZZPlayXmlHandler int_h;
  (void)host;
  if (!handler) {
    return NULL;
  }
  w = (FakeXmlWrapper *)calloc(1U, sizeof(FakeXmlWrapper));
  if (!w) {
    return NULL;
  }
  w->handler = *handler;
  int_h.start = fake_xml_start;
  int_h.end = fake_xml_end;
  int_h.user = w;
  zzplay_xml_init(&w->xml, &int_h);
  return w;
}

static int fake_xml_feed(const ZZPlayHost *host, void *xml, const char *data,
                         uint32_t len)
{
  FakeXmlWrapper *w = (FakeXmlWrapper *)xml;
  (void)host;
  if (!w || !data || len == 0U) {
    return 0;
  }
  return zzplay_xml_feed(&w->xml, data, (size_t)len);
}

static void fake_xml_close(const ZZPlayHost *host, void *xml)
{
  (void)host;
  free(xml);
}

static const char *fake_xml_attr(const ZZPlayHostXmlAttr *attrs, uint32_t count,
                                 const char *name)
{
  return zzplay_xml_attr((const ZZPlayXmlAttr *)attrs, count, name);
}

static char s_last_requested_url[512];
/* Faults: 1 = no answer with the transport's reason, 2 = no answer and no
 * reason at all. */
static int s_mock_fail = 0;
/* When set, every request gets this body, delivered in pieces. */
static const char *s_mock_body = NULL;
static size_t s_mock_body_len = 0U;
static uint32_t s_split_rng = 1U;

static void mock_body(const char *body, size_t len)
{
  s_mock_body = body;
  s_mock_body_len = len;
}

static int load_fixture(const char *filename, char *buf, size_t max_size)
{
  char path[256];
  FILE *f;
  size_t n;

  snprintf(path, sizeof(path), "tests/fixtures/radio/%s", filename);
  f = fopen(path, "rb");
  if (!f) {
    return 0;
  }
  n = fread(buf, 1, max_size - 1U, f);
  buf[n] = '\0';
  fclose(f);
  return 1;
}

static int fake_http_get(const ZZPlayHost *host, const char *url,
                         const char *headers,
                         int (*data)(void *user, const uint8_t *bytes,
                                     uint32_t len),
                         void *user, int *status, char *err, uint32_t err_size)
{
  static char fixture_buf[8192];
  const char *fix_file = NULL;
  (void)host;
  (void)headers;

  if (err && err_size > 0U) {
    err[0] = '\0'; /* as the real host does */
  }
  if (status) {
    *status = 200;
  }
  strncpy(s_last_requested_url, url ? url : "", sizeof(s_last_requested_url) - 1U);
  s_last_requested_url[sizeof(s_last_requested_url) - 1U] = '\0';

  if (s_mock_fail) {
    if (status) {
      *status = 0;
    }
    if (s_mock_fail == 1 && err && err_size > 0U) {
      snprintf(err, err_size, "Cannot connect to de1.api.radio-browser.info");
    }
    return 0;
  }
  if (s_mock_body != NULL) {
    size_t pos = 0U;

    while (pos < s_mock_body_len) {
      size_t piece;

      s_split_rng = s_split_rng * 1103515245U + 12345U;
      piece = 1U + (size_t)((s_split_rng >> 16) % 300U);
      if (piece > s_mock_body_len - pos) {
        piece = s_mock_body_len - pos;
      }
      if (data && data(user, (const uint8_t *)s_mock_body + pos,
                       (uint32_t)piece) != 0) {
        break;
      }
      pos += piece;
    }
    return 1;
  }

  /* Assert contracted API base */
  assert(strncmp(url, "http://de1.api.radio-browser.info/xml/", 38) == 0);

  if (strstr(url, "/stations/search?name=empty")) {
    fix_file = "empty.xml";
  } else if (strstr(url, "/stations/search?name=")) {
    fix_file = "search.xml";
  } else if (strstr(url, "/stations/byuuid/11111111-1111-1111-1111-111111111111")) {
    fix_file = "resolve_mp3.xml";
  } else if (strstr(url, "/stations/byuuid/44444444-4444-4444-4444-444444444444")) {
    fix_file = "resolve_fallback.xml";
  } else if (strstr(url, "/stations/byuuid/33333333-3333-3333-3333-333333333333")) {
    fix_file = "resolve_aac.xml";
  } else if (strstr(url, "/stations/topvote")) {
    fix_file = "top.xml";
  } else if (strstr(url, "/tags?")) {
    fix_file = "tags.xml";
  } else if (strstr(url, "/countries?")) {
    fix_file = "countries.xml";
  } else if (strstr(url, "/stations/bytag/")) {
    fix_file = "search.xml";
  } else if (strstr(url, "/stations/bycountry/")) {
    fix_file = "search.xml";
  }

  if (!fix_file || !load_fixture(fix_file, fixture_buf, sizeof(fixture_buf))) {
    if (err && err_size > 0U) {
      snprintf(err, err_size, "Fixture not found for: %s", url);
    }
    return 0;
  }

  if (data) {
    (void)data(user, (const uint8_t *)fixture_buf, (uint32_t)strlen(fixture_buf));
  }
  return 1;
}

static ZZPlayHost make_fake_host(void)
{
  ZZPlayHost h;
  memset(&h, 0, sizeof(h));
  h.abi = ZZPLAY_PROVIDER_ABI;
  h.size = sizeof(ZZPlayHost);
  h.alloc = fake_alloc;
  h.free = fake_free;
  h.http_get = fake_http_get;
  h.xml_open = fake_xml_open;
  h.xml_feed = fake_xml_feed;
  h.xml_close = fake_xml_close;
  h.xml_attr = fake_xml_attr;
  h.url_escape = zzplay_host_url_escape;
  h.format = fake_format;
  h.pref_get = fake_pref_get;
  h.pref_set = fake_pref_set;
  return h;
}

#define SINK_ITEMS_MAX 16

typedef struct TestSinkData {
  int count;
  uint32_t total;
  struct {
    char id[ZZPLAY_PROVIDER_TEXT_MAX];
    char title[ZZPLAY_PROVIDER_TEXT_MAX];
    char subtitle[ZZPLAY_PROVIDER_TEXT_MAX];
    uint32_t kind;
    uint32_t playable;
    uint32_t favourite;
  } items[SINK_ITEMS_MAX];
} TestSinkData;

static int sink_item_cb(void *user, const ZZPlayProviderItem *item)
{
  TestSinkData *s = (TestSinkData *)user;
  if (s->count < SINK_ITEMS_MAX) {
    strncpy(s->items[s->count].id, item->id ? item->id : "",
            sizeof(s->items[0].id) - 1U);
    strncpy(s->items[s->count].title, item->title ? item->title : "",
            sizeof(s->items[0].title) - 1U);
    strncpy(s->items[s->count].subtitle, item->subtitle ? item->subtitle : "",
            sizeof(s->items[0].subtitle) - 1U);
    s->items[s->count].kind = item->kind;
    s->items[s->count].playable = item->playable;
    s->items[s->count].favourite = item->favourite;
    s->count++;
  }
  return 0;
}

static void sink_total_cb(void *user, uint32_t total)
{
  TestSinkData *s = (TestSinkData *)user;
  s->total = total;
}

static void test_radio_descriptor(void)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  assert(p != NULL);
  assert(p->id == ZZPLAY_PROVIDER_ID);
  assert(p->abi == ZZPLAY_PROVIDER_ABI);
  assert(p->size == sizeof(ZZPlayProvider));
  assert(strcmp(p->scheme, "radio") == 0);
  assert(strcmp(p->name, "Internet radio") == 0);
  assert(p->open != NULL);
  assert(p->close != NULL);
  assert(p->list != NULL);
  assert(p->search != NULL);
  assert(p->resolve != NULL);
  assert(p->favourite != NULL);
}

static void test_radio_search(ZZPlayHost *host)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  void *ctx;
  TestSinkData sink_data;
  ZZPlayItemSink sink;
  char err[128];
  int ok;

  s_pref_count = 0;

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&sink_data, 0, sizeof(sink_data));
  sink.item = sink_item_cb;
  sink.total = sink_total_cb;
  sink.user = &sink_data;

  ok = p->search(ctx, "rock", &sink, err, sizeof(err));
  assert(ok);
  assert(strstr(s_last_requested_url, "stations/search?name=rock") != NULL);
  assert(strstr(s_last_requested_url, "hidebroken=true") != NULL);
  assert(strstr(s_last_requested_url, "order=votes") != NULL);

  assert(sink_data.count == 3);

  /* Station 1: MP3, playable */
  assert(strcmp(sink_data.items[0].id, "11111111-1111-1111-1111-111111111111") == 0);
  assert(strcmp(sink_data.items[0].title, "Blasmusikradio") == 0);
  assert(strcmp(sink_data.items[0].subtitle, "MP3, 128 kbps, Germany") == 0);
  assert(sink_data.items[0].kind == ZZPLAY_ITEM_STATION);
  assert(sink_data.items[0].playable == 1);
  assert(sink_data.items[0].favourite == 0);

  /* Station 2: OGG, playable */
  assert(strcmp(sink_data.items[1].id, "22222222-2222-2222-2222-222222222222") == 0);
  assert(strcmp(sink_data.items[1].title, "Radio Vorbis") == 0);
  assert(strcmp(sink_data.items[1].subtitle, "OGG, 160 kbps, Sweden") == 0);
  assert(sink_data.items[1].playable == 1);
  assert(sink_data.items[1].favourite == 0);

  /* Station 3: AAC+, unsupported */
  assert(strcmp(sink_data.items[2].id, "33333333-3333-3333-3333-333333333333") == 0);
  assert(strcmp(sink_data.items[2].title, "Radio Capital") == 0);
  assert(strstr(sink_data.items[2].subtitle, "Unsupported (AAC+)") != NULL);
  assert(sink_data.items[2].playable == 0);
  assert(sink_data.items[2].favourite == 0);

  p->close(ctx);
}

static void test_radio_search_empty(ZZPlayHost *host)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  void *ctx;
  TestSinkData sink_data;
  ZZPlayItemSink sink;
  char err[128];
  int ok;

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&sink_data, 0, sizeof(sink_data));
  sink.item = sink_item_cb;
  sink.total = sink_total_cb;
  sink.user = &sink_data;

  ok = p->search(ctx, "empty", &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.count == 0);

  p->close(ctx);
}

static void test_radio_resolve(ZZPlayHost *host)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  void *ctx;
  ZZPlayResolved resolved;
  char err[128];
  int ok;

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  /* 1. MP3 resolve with url_resolved */
  ok = p->resolve(ctx, "11111111-1111-1111-1111-111111111111", &resolved, err,
                 sizeof(err));
  assert(ok);
  assert(strcmp(s_last_requested_url,
                "http://de1.api.radio-browser.info/xml/stations/byuuid/11111111-1111-1111-1111-111111111111") == 0);
  assert(strcmp(resolved.url, "http://resolved.laut.fm/blasmusik") == 0);
  assert(strcmp(resolved.title, "Blasmusikradio") == 0);
  assert(resolved.duration_ms == 0);

  /* 2. Fallback to url when url_resolved is empty */
  ok = p->resolve(ctx, "44444444-4444-4444-4444-444444444444", &resolved, err,
                 sizeof(err));
  assert(ok);
  assert(strcmp(resolved.url, "http://stream.example.org/fallback.mp3") == 0);
  assert(strcmp(resolved.title, "Fallback Radio") == 0);

  /* 3. AAC station resolve fails with reason */
  memset(err, 0, sizeof(err));
  ok = p->resolve(ctx, "33333333-3333-3333-3333-333333333333", &resolved, err,
                 sizeof(err));
  assert(!ok);
  assert(strstr(err, "Unsupported codec") != NULL);
  assert(strstr(err, "AAC+") != NULL);

  p->close(ctx);
}

static void test_radio_favourites(ZZPlayHost *host)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  void *ctx;
  TestSinkData sink_data;
  ZZPlayItemSink sink;
  ZZPlayProviderItem item1, item2, item3;
  char err[128];
  int ok;
  uint32_t i;

  s_pref_count = 0;

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&sink_data, 0, sizeof(sink_data));
  sink.item = sink_item_cb;
  sink.total = sink_total_cb;
  sink.user = &sink_data;

  /* Initially empty */
  ok = p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 0);
  assert(sink_data.count == 0);

  /* 1. Add item1 */
  memset(&item1, 0, sizeof(item1));
  item1.id = "11111111-1111-1111-1111-111111111111";
  item1.title = "Blasmusikradio";
  item1.subtitle = "MP3, 128 kbps, Germany";
  item1.kind = ZZPLAY_ITEM_STATION;
  item1.playable = 1;

  ok = p->favourite(ctx, &item1, 1, err, sizeof(err));
  assert(ok);

  /* 2. Duplicate add is a no-op success */
  ok = p->favourite(ctx, &item1, 1, err, sizeof(err));
  assert(ok);

  /* Verify Favourites folder has 1 item */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 1);
  assert(sink_data.count == 1);
  assert(strcmp(sink_data.items[0].id, item1.id) == 0);
  assert(sink_data.items[0].favourite == 1);

  /* 3. Flag set in a later search listing */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->search(ctx, "rock", &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.count == 3);
  /* item1 is in favourites */
  assert(strcmp(sink_data.items[0].id, item1.id) == 0);
  assert(sink_data.items[0].favourite == 1);
  /* other items are not in favourites */
  assert(sink_data.items[1].favourite == 0);
  assert(sink_data.items[2].favourite == 0);

  /* Add item2 (unsupported AAC) */
  memset(&item2, 0, sizeof(item2));
  item2.id = "33333333-3333-3333-3333-333333333333";
  item2.title = "Radio Capital";
  item2.subtitle = "Unsupported (AAC+), 122 kbps, Italy";
  item2.kind = ZZPLAY_ITEM_STATION;
  item2.playable = 0;

  ok = p->favourite(ctx, &item2, 1, err, sizeof(err));
  assert(ok);

  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 2);
  assert(sink_data.count == 2);

  /* 4. Remove item1, then Favourites folder no longer lists it */
  ok = p->favourite(ctx, &item1, 0, err, sizeof(err));
  assert(ok);

  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 1);
  assert(sink_data.count == 1);
  assert(strcmp(sink_data.items[0].id, item2.id) == 0);

  /* Verify in search that item1 now has favourite == 0 */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->search(ctx, "rock", &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.items[0].favourite == 0);
  assert(sink_data.items[2].favourite == 1); /* item2 still favourite */

  /* 5. Persistence across close and open */
  p->close(ctx);

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 1);
  assert(sink_data.count == 1);
  assert(strcmp(sink_data.items[0].id, item2.id) == 0);

  /* 6. Full list error: list holds at most 64 stations */
  /* Remove item2 first to start clean */
  ok = p->favourite(ctx, &item2, 0, err, sizeof(err));
  assert(ok);

  /* Add 64 items */
  for (i = 0U; i < 64U; i++) {
    char id_buf[40];
    char title_buf[40];
    ZZPlayProviderItem fill_item;

    snprintf(id_buf, sizeof(id_buf), "station-uuid-%04u", i);
    snprintf(title_buf, sizeof(title_buf), "Station %u", i);

    memset(&fill_item, 0, sizeof(fill_item));
    fill_item.id = id_buf;
    fill_item.title = title_buf;
    fill_item.subtitle = "MP3, 128 kbps";
    fill_item.kind = ZZPLAY_ITEM_STATION;
    fill_item.playable = 1;

    ok = p->favourite(ctx, &fill_item, 1, err, sizeof(err));
    assert(ok);
  }

  /* Adding 65th item fails with specified message */
  memset(&item3, 0, sizeof(item3));
  item3.id = "station-overflow";
  item3.title = "Overflow Station";
  item3.subtitle = "MP3, 128 kbps";
  item3.kind = ZZPLAY_ITEM_STATION;
  item3.playable = 1;

  memset(err, 0, sizeof(err));
  ok = p->favourite(ctx, &item3, 1, err, sizeof(err));
  assert(!ok);
  assert(strcmp(err, "The favourites list is full (64 stations)") == 0);

  p->close(ctx);
}

static void test_radio_folders(ZZPlayHost *host)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  void *ctx;
  TestSinkData sink_data;
  ZZPlayItemSink sink;
  char err[128];
  int ok;

  s_pref_count = 0;

  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);

  sink.item = sink_item_cb;
  sink.total = sink_total_cb;
  sink.user = &sink_data;

  /* 1. Root folder listing */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(sink_data.total == 4);
  assert(sink_data.count == 4);
  assert(strcmp(sink_data.items[0].id, "fav") == 0);
  assert(strcmp(sink_data.items[0].title, "Favourites") == 0);
  assert(sink_data.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(sink_data.items[0].favourite == 0);
  assert(strcmp(sink_data.items[1].id, "top") == 0);
  assert(strcmp(sink_data.items[1].title, "Top voted") == 0);
  assert(sink_data.items[1].favourite == 0);
  assert(strcmp(sink_data.items[2].id, "tags") == 0);
  assert(strcmp(sink_data.items[2].title, "By tag") == 0);
  assert(sink_data.items[2].favourite == 0);
  assert(strcmp(sink_data.items[3].id, "countries") == 0);
  assert(strcmp(sink_data.items[3].title, "By country") == 0);
  assert(sink_data.items[3].favourite == 0);

  /* 2. Top voted */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "top", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(strstr(s_last_requested_url, "stations/topvote") != NULL);
  assert(sink_data.count == 1);
  assert(strcmp(sink_data.items[0].title, "Blasmusikradio") == 0);
  assert(sink_data.items[0].favourite == 0);

  /* 3. Tags list */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "tags", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(strstr(s_last_requested_url, "tags?order=stationcount") != NULL);
  assert(sink_data.count == 2);
  assert(strcmp(sink_data.items[0].id, "tag:pop") == 0);
  assert(strcmp(sink_data.items[0].title, "pop") == 0);
  assert(sink_data.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(sink_data.items[0].favourite == 0);
  assert(strcmp(sink_data.items[1].id, "tag:rock") == 0);
  assert(sink_data.items[1].favourite == 0);

  /* 4. Countries list */
  memset(&sink_data, 0, sizeof(sink_data));
  ok = p->list(ctx, "countries", 0, 10, &sink, err, sizeof(err));
  assert(ok);
  assert(strstr(s_last_requested_url, "countries?order=stationcount") != NULL);
  assert(sink_data.count == 2);
  assert(strcmp(sink_data.items[0].id, "country:Germany") == 0);
  assert(strcmp(sink_data.items[0].title, "Germany") == 0);
  assert(sink_data.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(sink_data.items[0].favourite == 0);

  p->close(ctx);
}

static int has_control(const char *s)
{
  for (; *s != '\0'; s++) {
    if ((unsigned char)*s < 0x20U || (unsigned char)*s == 0x7FU) {
      return 1;
    }
  }
  return 0;
}

/* What the module hands a sink keeps to the limits whatever the reply
 * held; rows are kept the way the Browse worker stores them. */
typedef struct CheckSink {
  ZZPlayBrowseItem rows[16];
  size_t raw_title_len[16];   /* as the module gave it, before Latin-1 */
  uint32_t count;
  uint32_t seen;
} CheckSink;

static int check_sink_item(void *user, const ZZPlayProviderItem *item)
{
  CheckSink *s = (CheckSink *)user;
  ZZPlayBrowseItem *dst;

  assert(item->id && item->title && item->subtitle);
  assert(item->id[0] != '\0' && strlen(item->id) < ZZPLAY_PROVIDER_TEXT_MAX);
  assert(strlen(item->title) < ZZPLAY_PROVIDER_TEXT_MAX);
  assert(strlen(item->subtitle) < ZZPLAY_PROVIDER_TEXT_MAX);
  assert(!has_control(item->title) && !has_control(item->subtitle));
  if (item->kind == ZZPLAY_ITEM_STATION) {
    assert(strspn(item->id, "0123456789abcdefghijklmnopqrstuvwxyz"
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZ-") == strlen(item->id));
  }
  s->seen++;
  if (s->count < sizeof(s->rows) / sizeof(s->rows[0])) {
    s->raw_title_len[s->count] = strlen(item->title);
    dst = &s->rows[s->count++];
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->id, sizeof(dst->id), "%s", item->id);
    snprintf(dst->title, sizeof(dst->title), "%s", item->title);
    zzplay_tags_text_to_latin1(dst->title);
    snprintf(dst->subtitle, sizeof(dst->subtitle), "%s", item->subtitle);
    zzplay_tags_text_to_latin1(dst->subtitle);
    dst->kind = item->kind;
    dst->playable = item->playable;
  }
  return 0;
}

static ZZPlayItemSink check_sink(CheckSink *s)
{
  ZZPlayItemSink sink;

  memset(s, 0, sizeof(*s));
  sink.item = check_sink_item;
  sink.total = NULL;
  sink.user = s;
  return sink;
}

/* An empty or garbled directory reply, or no reply, ends in a message
 * instead of an empty list. */
static void test_radio_faults(ZZPlayHost *host)
{
  static const char *const garbled[] = {
    "", "<html><body>502 Bad Gateway</body></html>", "<result",
    "{\"json\": true}", "\xff\xfe<\x00>"
  };
  const ZZPlayProvider *p = zzplay_provider_entry();
  CheckSink got;
  ZZPlayItemSink sink;
  ZZPlayResolved resolved;
  char err[128];
  void *ctx;
  size_t i;

  s_pref_count = 0;
  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);
  for (i = 0U; i < sizeof(garbled) / sizeof(garbled[0]); i++) {
    mock_body(garbled[i], strlen(garbled[i]));
    sink = check_sink(&got);
    err[0] = '\0';
    assert(!p->search(ctx, "rock", &sink, err, sizeof(err)));
    assert(strcmp(err, "The station directory sent an unreadable reply") == 0);
    assert(!p->list(ctx, "tags", 0, 10, &sink, err, sizeof(err)));
    assert(strcmp(err, "The station directory sent an unreadable reply") == 0);
    assert(!p->list(ctx, "top", 0, 10, &sink, err, sizeof(err)));
    assert(!p->resolve(ctx, "11111111-1111-1111-1111-111111111111", &resolved,
                       err, sizeof(err)));
    assert(strcmp(err, "The station directory sent an unreadable reply") == 0);
    assert(got.seen == 0U);
  }
  /* An empty directory is a list, not an error. */
  mock_body("<result/>", 9U);
  sink = check_sink(&got);
  assert(p->search(ctx, "rock", &sink, err, sizeof(err)) && got.seen == 0U);
  /* A station without an address cannot be played. */
  {
    static const char no_url[] =
      "<result><station stationuuid=\"11111111-1111-1111-1111-111111111111\" "
      "name=\"Silent\" codec=\"MP3\" url=\"\"/></result>";
    mock_body(no_url, sizeof(no_url) - 1U);
    assert(!p->resolve(ctx, "11111111-1111-1111-1111-111111111111", &resolved,
                       err, sizeof(err)));
    assert(strcmp(err, "The station has no usable stream address") == 0);
  }
  mock_body(NULL, 0U);

  /* No answer: the transport's reason, or one of the module's own. */
  s_mock_fail = 1;
  sink = check_sink(&got);
  assert(!p->search(ctx, "rock", &sink, err, sizeof(err)));
  assert(strcmp(err, "Cannot connect to de1.api.radio-browser.info") == 0);
  s_mock_fail = 2;
  assert(!p->list(ctx, "countries", 0, 10, &sink, err, sizeof(err)));
  assert(strcmp(err, "The station directory did not answer") == 0);
  assert(!p->resolve(ctx, "11111111-1111-1111-1111-111111111111", &resolved,
                     err, sizeof(err)));
  assert(strcmp(err, "The station directory did not answer") == 0);
  s_mock_fail = 0;

  /* Text that does not fit a request is refused, not sent cut short. */
  {
    char query[200];

    memset(query, '%', sizeof(query) - 1U);
    query[sizeof(query) - 1U] = '\0';
    assert(!p->search(ctx, query, &sink, err, sizeof(err)));
    assert(strcmp(err, "The search text is too long") == 0);
    memcpy(query, "tag:", 4U);
    assert(!p->list(ctx, query, 0, 10, &sink, err, sizeof(err)));
    assert(strcmp(err, "The tag name is empty or too long") == 0);
  }
  assert(!p->resolve(ctx, "../../etc", &resolved, err, sizeof(err)));
  assert(strcmp(err, "Not a radio station id") == 0);
  p->close(ctx);
}

/* Station names with control characters, far past every limit, or not
 * UTF-8 come out cut and clean through the module and the Browse model;
 * stations whose ids could not be used are left out, and a stored
 * favourite keeps its fields apart. */
static void test_radio_odd_text(ZZPlayHost *host)
{
  static char body[12288];
  static char long_name[3000];
  const ZZPlayProvider *p = zzplay_provider_entry();
  CheckSink got;
  ZZPlayItemSink sink;
  ZZPlayBrowseModel model;
  ZZPlayBrowseRequest req;
  ZZPlayBrowseReply reply;
  ZZPlayProviderItem fav;
  char err[128];
  void *ctx;
  size_t i;
  uint32_t k;
  int found = 0;

  long_name[0] = '\0';
  for (i = 0U; i < 900U; i++) {
    strcat(long_name, "\xe2\x82\xac"); /* euro sign, three bytes */
  }
  snprintf(body, sizeof(body),
           "<result>"
           "<station stationuuid=\"aaaa-1\" name=\"Line&#10;Break|Bar&#9;Tab\" "
           "codec=\"MP3\" bitrate=\"128\" country=\"Nowhere&#13;&#10;fav_count=9\"/>"
           "<station stationuuid=\"aaaa-2\" name=\"%s\" codec=\"MP3\"/>"
           "<station stationuuid=\"aaaa-3\" name=\"bad \xff\xfe\x80\" codec=\"MP3\"/>"
           "<station stationuuid=\"../etc/passwd\" name=\"bad id\" codec=\"MP3\"/>"
           "<station stationuuid=\"0123456789012345678901234567890123456789\" "
           "name=\"long id\" codec=\"MP3\"/>"
           "<tag name=\"%s\" stationcount=\"1\"/>"
           "</result>",
           long_name, long_name);

  s_pref_count = 0;
  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);
  mock_body(body, strlen(body));
  sink = check_sink(&got);
  assert(p->search(ctx, "odd", &sink, err, sizeof(err)));
  assert(got.seen == 3U);
  assert(strcmp(got.rows[0].title, "Line Break|Bar Tab") == 0);
  /* 31 whole euro signs fit in 95 bytes; the 32nd would be cut. Shown in
   * Latin-1, each is a '?'. */
  assert(got.raw_title_len[1] == 93U);
  assert(strspn(got.rows[1].title, "?") == 31U && strlen(got.rows[1].title) == 31U);
  /* A tag whose id would not fit is left out. */
  sink = check_sink(&got);
  assert(p->list(ctx, "tags", 0, 10, &sink, err, sizeof(err)));
  assert(got.seen == 0U);

  /* The rows in the Browse model and out as a playlist entry. */
  sink = check_sink(&got);
  assert(p->search(ctx, "odd", &sink, err, sizeof(err)));
  zzplay_browse_model_init(&model);
  assert(zzplay_browse_model_add_service(&model, "radio", "Radio", NULL));
  assert(zzplay_browse_model_select_service(&model, 0, &req));
  memset(&reply, 0, sizeof(reply));
  reply.kind = ZZPLAY_BROWSE_REQ_LIST;
  reply.generation = req.generation;
  reply.success = 1;
  reply.items = got.rows;
  reply.item_count = got.count;
  assert(zzplay_browse_model_apply_reply(&model, &reply) == 1);
  for (k = 0U; k < model.item_count; k++) {
    char uri[ZZPLAY_PROVIDER_URL_MAX];
    char title[16];

    assert(!has_control(model.items[k].title));
    if (strcmp(model.items[k].id, "aaaa-2") != 0) continue;
    found = 1;
    assert(zzplay_browse_model_format_uri(&model, (int32_t)k, uri, sizeof(uri),
                                          title, sizeof(title)));
    assert(strcmp(uri, "radio://aaaa-2") == 0 && strlen(title) == 15U);
  }
  assert(found);
  zzplay_browse_model_free(&model);

  /* A favourite with a '|' and line breaks in its fields is stored as one
   * line of five fields and reads back the same after a restart. */
  memset(&fav, 0, sizeof(fav));
  fav.id = "aaaa-1";
  fav.title = "Line\nBreak|Bar";
  fav.subtitle = "MP3, 128 kbps, Nowhere\r\nfav_count=9|x";
  fav.kind = ZZPLAY_ITEM_STATION;
  assert(p->favourite(ctx, &fav, 1, err, sizeof(err)));
  for (k = 0U; k < (uint32_t)s_pref_count; k++) {
    const char *v = s_prefs[k].value;
    size_t bars = 0U;

    assert(!has_control(v));
    if (strncmp(s_prefs[k].key, "fav_", 4) == 0 &&
        strcmp(s_prefs[k].key, "fav_count") != 0 && v[0] != '\0') {
      for (; *v != '\0'; v++) {
        bars += *v == '|';
      }
      assert(bars == 4U);
    }
  }
  p->close(ctx);
  mock_body(NULL, 0U);
  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);
  sink = check_sink(&got);
  assert(p->list(ctx, "fav", 0, 10, &sink, err, sizeof(err)));
  assert(got.seen == 1U && strcmp(got.rows[0].id, "aaaa-1") == 0);
  assert(strcmp(got.rows[0].title, "Line Break Bar") == 0);
  /* An id that is no station is not stored. */
  fav.id = "x|y";
  assert(!p->favourite(ctx, &fav, 1, err, sizeof(err)));
  assert(strcmp(err, "Not a radio station id") == 0);
  fav.id = "aaaa-1";
  assert(p->favourite(ctx, &fav, 0, err, sizeof(err)));
  p->close(ctx);
}

/* --- Mutated directory replies --- */

static uint32_t fz_state = 0x9E3779B9U;

static uint32_t fz(void)
{
  fz_state ^= fz_state << 13;
  fz_state ^= fz_state >> 17;
  fz_state ^= fz_state << 5;
  return fz_state;
}

/* A few random edits: byte changes, XML tokens, cuts and copies. */
static size_t fz_mutate(char *buf, size_t len, size_t cap)
{
  static const char *const tokens[] = {
    "<", ">", "/>", "</", "\"", "'", "=", "&amp;", "&#10;", "&#x1;", "&#0;",
    "&#x10FFFF;", "<station stationuuid=\"a-b\" name=\"", "<result>",
    "</result>", "<tag name=\"", "|", "\n", "\xc3", "\xff", " codec=\"MP3\"",
    " url=\"http://x/\"", " name=\""
  };
  uint32_t edits = 1U + fz() % 6U;
  uint32_t e;

  for (e = 0U; e < edits; e++) {
    size_t at = len != 0U ? fz() % (len + 1U) : 0U;

    switch (fz() % 6U) {
    case 0:
      if (at < len) buf[at] = (char)fz();
      break;
    case 1:
    case 2: {
      const char *t = tokens[fz() % (sizeof(tokens) / sizeof(tokens[0]))];
      size_t n = strlen(t);
      if (len + n <= cap) {
        memmove(buf + at + n, buf + at, len - at);
        memcpy(buf + at, t, n);
        len += n;
      }
      break;
    }
    case 3:
      if (at < len) {
        size_t n = 1U + fz() % (len - at);
        memmove(buf + at, buf + at + n, len - at - n);
        len -= n;
      }
      break;
    case 4:
      len = at;
      break;
    default:
      if (at < len) {
        size_t n = 1U + fz() % (len - at);
        size_t to = fz() % (len + 1U);
        if (n > 200U) n = 200U;
        if (len + n <= cap) {
          char copy[200];
          memcpy(copy, buf + at, n);
          memmove(buf + to + n, buf + to, len - to);
          memcpy(buf + to, copy, n);
          len += n;
        }
      }
      break;
    }
  }
  return len;
}

static void test_radio_reply_fuzz(ZZPlayHost *host)
{
  static const char *const fixtures[] = {
    "search.xml", "top.xml", "tags.xml", "countries.xml", "resolve_mp3.xml",
    "empty.xml"
  };
  static char seeds[6][8192];
  static char buf[24576];
  const ZZPlayProvider *p = zzplay_provider_entry();
  CheckSink got;
  ZZPlayItemSink sink;
  ZZPlayResolved resolved;
  char err[128];
  void *ctx;
  uint32_t r;
  size_t i;

  for (i = 0U; i < 6U; i++) {
    assert(load_fixture(fixtures[i], seeds[i], sizeof(seeds[i])));
  }
  s_pref_count = 0;
  ctx = p->open(host, err, sizeof(err));
  assert(ctx != NULL);
  for (r = 0U; r < 3000U; r++) {
    size_t which = fz() % 6U;
    size_t len = strlen(seeds[which]);
    int ok;

    memcpy(buf, seeds[which], len);
    len = fz_mutate(buf, len, sizeof(buf));
    mock_body(buf, len);
    sink = check_sink(&got);
    err[0] = '\0';
    switch (which) {
    case 2:
      ok = p->list(ctx, "tags", 0, 50, &sink, err, sizeof(err));
      break;
    case 3:
      ok = p->list(ctx, "countries", 0, 50, &sink, err, sizeof(err));
      break;
    case 4:
      memset(&resolved, 0, sizeof(resolved));
      ok = p->resolve(ctx, "11111111-1111-1111-1111-111111111111", &resolved,
                      err, sizeof(err));
      if (ok) {
        assert(resolved.url[0] != '\0' &&
               strlen(resolved.url) < sizeof(resolved.url));
        assert(strlen(resolved.title) < sizeof(resolved.title));
        assert(!has_control(resolved.title));
      }
      break;
    case 1:
      ok = p->list(ctx, "top", 0, 50, &sink, err, sizeof(err));
      break;
    default:
      ok = p->search(ctx, "x", &sink, err, sizeof(err));
      break;
    }
    if (!ok) {
      assert(err[0] != '\0' && strlen(err) < sizeof(err));
    }
    /* Now and then keep a fuzzed station as a favourite and read it back
     * the way a restart does. */
    if (ok && got.count != 0U && got.rows[0].kind == ZZPLAY_ITEM_STATION &&
        r % 25U == 0U) {
      ZZPlayProviderItem fav;
      CheckSink back;
      ZZPlayItemSink back_sink;
      void *again;

      memset(&fav, 0, sizeof(fav));
      fav.id = got.rows[0].id;
      fav.title = got.rows[0].title;
      fav.subtitle = got.rows[0].subtitle;
      fav.kind = ZZPLAY_ITEM_STATION;
      assert(p->favourite(ctx, &fav, 1, err, sizeof(err)));
      again = p->open(host, err, sizeof(err));
      assert(again != NULL);
      back_sink = check_sink(&back);
      assert(p->list(again, "fav", 0, 70, &back_sink, err, sizeof(err)));
      assert(back.seen >= 1U);
      p->close(again);
      assert(p->favourite(ctx, &fav, 0, err, sizeof(err)));
    }
  }
  mock_body(NULL, 0U);
  p->close(ctx);
}

int main(void)
{
  ZZPlayHost fake_host = make_fake_host();

  test_radio_descriptor();
  test_radio_search(&fake_host);
  test_radio_search_empty(&fake_host);
  test_radio_resolve(&fake_host);
  test_radio_favourites(&fake_host);
  test_radio_folders(&fake_host);
  test_radio_faults(&fake_host);
  test_radio_odd_text(&fake_host);
  test_radio_reply_fuzz(&fake_host);

  printf("zzplay_provider_radio_test: all assertions passed\n");
  return 0;
}
