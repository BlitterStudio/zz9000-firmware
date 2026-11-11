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
#include "../tools/zzplay/zzplay-xml.h"

extern const ZZPlayProvider *zzplay_provider_radio(void);

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
  (void)err;
  (void)err_size;

  if (status) {
    *status = 200;
  }
  strncpy(s_last_requested_url, url ? url : "", sizeof(s_last_requested_url) - 1U);
  s_last_requested_url[sizeof(s_last_requested_url) - 1U] = '\0';

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
  const ZZPlayProvider *p = zzplay_provider_radio();
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
  const ZZPlayProvider *p = zzplay_provider_radio();
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
  const ZZPlayProvider *p = zzplay_provider_radio();
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
  const ZZPlayProvider *p = zzplay_provider_radio();
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
  const ZZPlayProvider *p = zzplay_provider_radio();
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
  const ZZPlayProvider *p = zzplay_provider_radio();
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

int main(void)
{
  ZZPlayHost fake_host = make_fake_host();

  test_radio_descriptor();
  test_radio_search(&fake_host);
  test_radio_search_empty(&fake_host);
  test_radio_resolve(&fake_host);
  test_radio_favourites(&fake_host);
  test_radio_folders(&fake_host);

  printf("zzplay_provider_radio_test: all assertions passed\n");
  return 0;
}
