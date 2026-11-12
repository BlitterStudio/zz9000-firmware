/*
 * Unit tests for the ZZPlay Plex provider module.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-provider.h"
#include "../tools/zzplay/zzplay-xml.h"

#define TEST_SERVER "http://192.168.1.10:32400"
#define TEST_TOKEN "test-token-12345"

/* In-memory prefs storage */
typedef struct PrefEntry {
  char key[32];
  char value[128];
} PrefEntry;

static PrefEntry g_prefs[16];
static uint32_t g_pref_count = 0;

static void mock_prefs_clear(void)
{
  g_pref_count = 0;
  memset(g_prefs, 0, sizeof(g_prefs));
}

static void mock_pref_set(const char *key, const char *value)
{
  for (uint32_t i = 0; i < g_pref_count; i++) {
    if (strcmp(g_prefs[i].key, key) == 0) {
      strncpy(g_prefs[i].value, value, sizeof(g_prefs[i].value) - 1);
      g_prefs[i].value[sizeof(g_prefs[i].value) - 1] = '\0';
      return;
    }
  }
  if (g_pref_count < sizeof(g_prefs) / sizeof(g_prefs[0])) {
    strncpy(g_prefs[g_pref_count].key, key, sizeof(g_prefs[0].key) - 1);
    strncpy(g_prefs[g_pref_count].value, value, sizeof(g_prefs[0].value) - 1);
    g_pref_count++;
  }
}

static int fake_pref_get(const ZZPlayHost *host, const char *key, char *out, uint32_t size)
{
  (void)host;
  if (!out || size == 0) return 0;
  out[0] = '\0';
  for (uint32_t i = 0; i < g_pref_count; i++) {
    if (strcmp(g_prefs[i].key, key) == 0) {
      strncpy(out, g_prefs[i].value, size - 1);
      out[size - 1] = '\0';
      return 1;
    }
  }
  return 0;
}

static int fake_pref_set(const ZZPlayHost *host, const char *key, const char *value)
{
  (void)host;
  mock_pref_set(key, value);
  return 1;
}

static void *fake_alloc(const ZZPlayHost *host, uint32_t size)
{
  (void)host;
  return calloc(1, size);
}

static void fake_free(const ZZPlayHost *host, void *ptr)
{
  (void)host;
  free(ptr);
}

static int fake_format(char *out, uint32_t size, const char *fmt, ...)
{
  va_list args;
  int ret;
  va_start(args, fmt);
  ret = vsnprintf(out, size, fmt, args);
  va_end(args);
  return ret;
}

static uint32_t fake_url_escape(const char *in, char *out, uint32_t size)
{
  static const char hex[] = "0123456789ABCDEF";
  uint32_t written = 0;
  if (!in || !out || size == 0) return 0;

  while (*in) {
    unsigned char c = (unsigned char)*in++;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      if (written + 1 >= size) return 0;
      out[written++] = (char)c;
    } else {
      if (written + 3 >= size) return 0;
      out[written++] = '%';
      out[written++] = hex[c >> 4];
      out[written++] = hex[c & 0x0F];
    }
  }
  out[written] = '\0';
  return written;
}

/* Streaming XML wrappers */
static void *fake_xml_open(const ZZPlayHost *host, const ZZPlayHostXmlHandler *handler)
{
  (void)host;
  ZZPlayXml *xml = (ZZPlayXml *)calloc(1, sizeof(ZZPlayXml));
  if (!xml) return NULL;
  zzplay_xml_init(xml, (const ZZPlayXmlHandler *)handler);
  return xml;
}

static int fake_xml_feed(const ZZPlayHost *host, void *xml, const char *data, uint32_t len)
{
  (void)host;
  return zzplay_xml_feed((ZZPlayXml *)xml, data, (size_t)len);
}

static void fake_xml_close(const ZZPlayHost *host, void *xml)
{
  (void)host;
  free(xml);
}

static const char *fake_xml_attr(const ZZPlayHostXmlAttr *attrs, uint32_t count, const char *name)
{
  return zzplay_xml_attr((const ZZPlayXmlAttr *)attrs, count, name);
}

/* HTTP mock state */
static int g_mock_status = 200;
static int g_mock_single_section = 0;
static int g_mock_post_status = 201;
static int g_mock_poll_calls = 0;
static int g_mock_poll_authenticated_after = 1;
static int g_mock_sleep_cancelled = 0;
static int g_mock_sleep_calls = 0;
static int g_mock_https_identity_fails = 0;
static int g_mock_http_identity_fails = 0;
static int g_mock_two_servers = 0;
static uint32_t g_total_requests = 0;
static uint32_t g_headers_missing_count = 0;
static uint32_t g_missing_exclude_count = 0;
static char g_last_request_url[512];
static char g_last_request_headers[512];
static char g_last_decision_url[1024];

static int load_fixture(const char *rel_path,
                        int (*data)(void *user, const uint8_t *bytes, uint32_t len),
                        void *user)
{
  char path[256];
  snprintf(path, sizeof(path), "tests/fixtures/plex/%s", rel_path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "Cannot open fixture: %s\n", rel_path);
    return 0;
  }
  uint8_t buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (data && data(user, buf, (uint32_t)n) != 0) {
      break;
    }
  }
  fclose(f);
  return 1;
}

static int fake_http_post(const ZZPlayHost *host, const char *url,
                          const char *headers, const uint8_t *body,
                          uint32_t body_len,
                          int (*data)(void *user, const uint8_t *bytes, uint32_t len),
                          void *user, int *status, char *err, uint32_t err_size)
{
  (void)host;
  (void)body;
  (void)body_len;
  if (err && err_size > 0U) err[0] = '\0';
  g_total_requests++;

  strncpy(g_last_request_url, url ? url : "", sizeof(g_last_request_url) - 1);
  g_last_request_url[sizeof(g_last_request_url) - 1] = '\0';

  strncpy(g_last_request_headers, headers ? headers : "", sizeof(g_last_request_headers) - 1);
  g_last_request_headers[sizeof(g_last_request_headers) - 1] = '\0';

  if (!headers ||
      !strstr(headers, "X-Plex-Client-Identifier:") ||
      !strstr(headers, "X-Plex-Product: ZZPlay") ||
      !strstr(headers, "X-Plex-Version: 0.6") ||
      !strstr(headers, "X-Plex-Platform: Generic") ||
      !strstr(headers, "Accept: application/xml")) {
    g_headers_missing_count++;
  }

  if (status) *status = g_mock_post_status;
  if (g_mock_post_status != 201) {
    return 0;
  }

  if (url && strstr(url, "/api/v2/pins")) {
    return load_fixture("pin.xml", data, user);
  }
  return 0;
}

static int fake_sleep(const ZZPlayHost *host, uint32_t ms)
{
  (void)host;
  (void)ms;
  g_mock_sleep_calls++;
  if (g_mock_sleep_cancelled) {
    return 0;
  }
  return 1;
}

static int fake_http_get(const ZZPlayHost *host, const char *url,
                         const char *headers,
                         int (*data)(void *user, const uint8_t *bytes, uint32_t len),
                         void *user, int *status, char *err, uint32_t err_size)
{
  (void)host;
  if (err && err_size > 0U) err[0] = '\0';
  g_total_requests++;

  strncpy(g_last_request_url, url ? url : "", sizeof(g_last_request_url) - 1);
  g_last_request_url[sizeof(g_last_request_url) - 1] = '\0';

  strncpy(g_last_request_headers, headers ? headers : "", sizeof(g_last_request_headers) - 1);
  g_last_request_headers[sizeof(g_last_request_headers) - 1] = '\0';

  /* Check required headers on every request */
  if (strstr(url, "/api/v2/pins")) {
    if (!headers ||
        !strstr(headers, "X-Plex-Client-Identifier:") ||
        !strstr(headers, "X-Plex-Product: ZZPlay") ||
        !strstr(headers, "X-Plex-Version: 0.6") ||
        !strstr(headers, "X-Plex-Platform: Generic") ||
        !strstr(headers, "Accept: application/xml")) {
      g_headers_missing_count++;
    }
  } else {
    if (!headers ||
        !strstr(headers, "X-Plex-Token:") ||
        !strstr(headers, "X-Plex-Client-Identifier:") ||
        !strstr(headers, "X-Plex-Product: ZZPlay") ||
        !strstr(headers, "X-Plex-Version: 0.6") ||
        !strstr(headers, "X-Plex-Platform: Generic")) {
      g_headers_missing_count++;
    }
  }

  if (g_mock_status != 200) {
    if (status) *status = g_mock_status;
    if (g_mock_status == 401) {
      load_fixture("unauthorized.html", data, user);
    }
    return 1;
  }

  if (status) *status = 200;

  if (strstr(url, "/api/v2/pins")) {
    g_mock_poll_calls++;
    if (g_mock_poll_calls >= g_mock_poll_authenticated_after) {
      return load_fixture("pinpoll-authenticated.xml", data, user);
    }
    return load_fixture("pinpoll-waiting.xml", data, user);
  }
  if (strstr(url, "/api/v2/resources")) {
    if (g_mock_two_servers) {
      return load_fixture("resources-multi.xml", data, user);
    }
    return load_fixture("resources.xml", data, user);
  }
  if (strstr(url, "/identity")) {
    if (strncmp(url, "https://", 8) == 0 && g_mock_https_identity_fails) {
      if (status) *status = 0;
      return 0;
    }
    if (strncmp(url, "http://", 7) == 0 && g_mock_http_identity_fails) {
      if (status) *status = 0;
      return 0;
    }
    return load_fixture("identity.xml", data, user);
  }

  if (strstr(url, "/music/:/transcode/universal/decision")) {
    strncpy(g_last_decision_url, url, sizeof(g_last_decision_url) - 1);
    g_last_decision_url[sizeof(g_last_decision_url) - 1] = '\0';
    return load_fixture("decision.xml", data, user);
  }
  /* Every list request must ask Plex to leave summaries out. */
  {
    int is_list = (strstr(url, "/all?") != NULL) ||
                  (strstr(url, "/children") != NULL) ||
                  (strstr(url, "/items") != NULL) ||
                  (strstr(url, "/search?") != NULL) ||
                  (strstr(url, "/firstCharacter") != NULL) ||
                  (strstr(url, "/genre?") != NULL);
    if (is_list && !strstr(url, "excludeFields=summary")) {
      g_missing_exclude_count++;
    }
  }

  if (strstr(url, "/search?")) {
    int count_only = headers && strstr(headers, "X-Plex-Container-Size: 0");
    int is_paris = strstr(url, "query=paris") != NULL;

    if (count_only) {
      char buf[128];
      int n = 0;
      if (strstr(url, "type=9")) {
        n = is_paris ? 2 : 0;
      } else if (strstr(url, "type=10")) {
        n = is_paris ? 21 : 1;
      }
      snprintf(buf, sizeof(buf),
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<MediaContainer size=\"0\" totalSize=\"%d\"/>", n);
      if (data) data(user, (const uint8_t *)buf, (uint32_t)strlen(buf));
      return 1;
    }
    if (strstr(url, "type=9")) {
      return load_fixture("search-album-results.xml", data, user);
    }
    if (strstr(url, "type=8")) {
      const char empty[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MediaContainer size=\"0\"/>";
      if (data) data(user, (const uint8_t *)empty, (uint32_t)strlen(empty));
      return 1;
    }
    if (strstr(url, "/library/sections/5/")) {
      return load_fixture("search-tracks-5.xml", data, user);
    }
    return load_fixture("search-tracks.xml", data, user);
  }
  if (strstr(url, "/firstCharacter?")) {
    return load_fixture(strstr(url, "type=9") ? "firstchar-albums.xml"
                                              : "firstchar-artists.xml",
                        data, user);
  }
  if (strstr(url, "/firstCharacter/")) {
    if (strstr(url, "type=9")) {
      return load_fixture("letter-a-albums.xml", data, user);
    }
    if (headers && strstr(headers, "X-Plex-Container-Start: 2")) {
      return load_fixture("artists-page2.xml", data, user);
    }
    return load_fixture("letter-a-artists.xml", data, user);
  }
  if (strstr(url, "/genre?")) {
    return load_fixture("genres.xml", data, user);
  }
  if (strstr(url, "genre=9001")) {
    return load_fixture("genre-artists.xml", data, user);
  }
  if (strstr(url, "type=9&sort=addedAt:desc")) {
    return load_fixture("recent-albums.xml", data, user);
  }
  if (strstr(url, "/library/sections")) {
    if (g_mock_single_section) {
      return load_fixture("sections-single.xml", data, user);
    }
    return load_fixture("sections.xml", data, user);
  }
  if (strstr(url, "/library/metadata/101/children")) {
    return load_fixture("albums.xml", data, user);
  }
  if (strstr(url, "/library/metadata/201/children")) {
    return load_fixture("tracks.xml", data, user);
  }
  if (strstr(url, "/playlists?playlistType=audio")) {
    return load_fixture("playlists.xml", data, user);
  }
  if (strstr(url, "/playlists/601/items")) {
    return load_fixture("tracks.xml", data, user);
  }
  if (strstr(url, "/library/metadata/302")) {
    return load_fixture("metadata-302.xml", data, user);
  }
  if (strstr(url, "/library/metadata/303")) {
    return load_fixture("metadata-303.xml", data, user);
  }

  return 0;
}

static const ZZPlayHost g_test_host = {
  ZZPLAY_PROVIDER_ABI,
  (uint32_t)sizeof(ZZPlayHost),
  fake_alloc,
  fake_free,
  fake_http_get,
  fake_xml_open,
  fake_xml_feed,
  fake_xml_close,
  fake_xml_attr,
  fake_url_escape,
  fake_format,
  fake_pref_get,
  fake_pref_set,
  fake_http_post,
  fake_sleep
};

/* Item collector sink */
typedef struct CollectedItem {
  char id[96];
  char title[96];
  char subtitle[96];
  uint32_t duration_ms;
  uint32_t kind;
  uint32_t playable;
} CollectedItem;

typedef struct ItemCollector {
  ZZPlayItemSink sink;
  CollectedItem items[32];
  uint32_t count;
  uint32_t total;
  uint32_t stop_after;
} ItemCollector;

static int collector_item_cb(void *user, const ZZPlayProviderItem *item)
{
  ItemCollector *col = (ItemCollector *)user;
  if (!item || col->count >= sizeof(col->items) / sizeof(col->items[0])) {
    return 1;
  }
  CollectedItem *dst = &col->items[col->count++];
  strncpy(dst->id, item->id ? item->id : "", sizeof(dst->id) - 1);
  dst->id[sizeof(dst->id) - 1] = '\0';
  strncpy(dst->title, item->title ? item->title : "", sizeof(dst->title) - 1);
  dst->title[sizeof(dst->title) - 1] = '\0';
  strncpy(dst->subtitle, item->subtitle ? item->subtitle : "", sizeof(dst->subtitle) - 1);
  dst->subtitle[sizeof(dst->subtitle) - 1] = '\0';
  dst->duration_ms = item->duration_ms;
  dst->kind = item->kind;
  dst->playable = item->playable;

  if (col->stop_after > 0 && col->count >= col->stop_after) {
    return 1;
  }
  return 0;
}

static void collector_total_cb(void *user, uint32_t total)
{
  ItemCollector *col = (ItemCollector *)user;
  col->total = total;
}

static void collector_init(ItemCollector *col, uint32_t stop_after)
{
  memset(col, 0, sizeof(*col));
  col->sink.item = collector_item_cb;
  col->sink.total = collector_total_cb;
  col->sink.user = col;
  col->stop_after = stop_after;
}

/* --- Tests --- */

static void test_no_token_shows_signin_folder(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  /* No SERVER, no TOKEN */
  mock_prefs_clear();
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(col.total == 1);
  assert(strcmp(col.items[0].id, "signin") == 0);
  assert(strcmp(col.items[0].title, "Sign in with plex.tv") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(col.items[0].playable == 0);
  p->close(ctx);

  /* SERVER set, TOKEN empty: still lists sign-in folder */
  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(strcmp(col.items[0].id, "signin") == 0);
  p->close(ctx);

  /* TOKEN set, SERVER empty: top-level fails asking for server */
  mock_prefs_clear();
  mock_pref_set("TOKEN", TEST_TOKEN);
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strcmp(err, "Set the server in Settings") == 0);
  p->close(ctx);
}

static void test_pin_request(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  g_mock_post_status = 201;
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 3);
  assert(col.total == 3);

  assert(strcmp(col.items[0].id, "info:link") == 0);
  assert(strcmp(col.items[0].title, "On any device, open plex.tv/link") == 0);
  assert(col.items[0].playable == 0);

  assert(strcmp(col.items[1].id, "info:code") == 0);
  assert(strcmp(col.items[1].title, "and enter the code NTLQ") == 0);
  assert(col.items[1].playable == 0);

  assert(strcmp(col.items[2].id, "signin:continue:736458353") == 0);
  assert(strcmp(col.items[2].title, "Continue after entering the code") == 0);
  assert(col.items[2].kind == ZZPLAY_ITEM_FOLDER);
  assert(col.items[2].playable == 0);

  p->close(ctx);
}

static void test_pin_poll_waiting_then_token(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 2; /* Poll 1 waiting, Poll 2 authenticated */
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 0;
  g_mock_two_servers = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(g_mock_poll_calls == 2);
  assert(g_mock_sleep_calls == 1);

  p->close(ctx);
}

static void test_pin_poll_cancel_stores_nothing(const ZZPlayProvider *p)
{
  char err[128];
  err[0] = '\0';
  ItemCollector col;
  void *ctx;
  int ok;
  char srv[128];
  char tok[128];

  mock_prefs_clear();
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 10;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 1; /* sleep returns 0 to cancel */

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strcmp(err, "Cancelled") == 0);

  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 0);

  p->close(ctx);
}

static void test_resources_signin_stores_both_addresses(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  char srv[256];
  char tok[128];

  /* Sign-in with default/empty CONNECTION: probes https, stores both addresses */
  mock_prefs_clear();
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 1;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 0;
  g_mock_http_identity_fails = 0;
  g_mock_two_servers = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);

  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "https://192-168-1-18.abcdef0123456789.plex.direct:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_SECURE", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "https://192-168-1-18.abcdef0123456789.plex.direct:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_PLAIN", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "http://192.168.1.18:32400") == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 1);
  assert(strcmp(tok, "server-token-456") == 0);

  /* One server lands directly in library top level */
  assert(col.count > 0);
  assert(strcmp(col.items[0].id, "playlists") == 0);
  p->close(ctx);
}

static void test_resources_signin_connection_plain(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  char srv[256];
  char tok[128];

  /* Sign-in with CONNECTION=plain: probes http, stores both addresses, SERVER is plain */
  mock_prefs_clear();
  mock_pref_set("CONNECTION", "plain");
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 1;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 1; /* https would fail, but plain probe is used */
  g_mock_http_identity_fails = 0;
  g_mock_two_servers = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);

  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "http://192.168.1.18:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_SECURE", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "https://192-168-1-18.abcdef0123456789.plex.direct:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_PLAIN", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "http://192.168.1.18:32400") == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 1);
  assert(strcmp(tok, "server-token-456") == 0);
  assert(col.count > 0);
  assert(strcmp(col.items[0].id, "playlists") == 0);
  p->close(ctx);
}

static void test_connection_selection_and_switching(const ZZPlayProvider *p)
{
  char err[128];
  void *ctx;
  int ok;
  ZZPlayResolved res;

  mock_prefs_clear();
  mock_pref_set("SERVER_SECURE", "https://192-168-1-18.abcdef0123456789.plex.direct:32400");
  mock_pref_set("SERVER_PLAIN", "http://192.168.1.18:32400");
  mock_pref_set("SERVER", "http://manual.fallback:32400");
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  /* 1. CONNECTION=plain uses SERVER_PLAIN */
  mock_pref_set("CONNECTION", "plain");
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://192.168.1.18:32400/", strlen("http://192.168.1.18:32400/")) == 0);
  p->close(ctx);

  /* 2. Switching CONNECTION to secure uses SERVER_SECURE without re-signing in */
  mock_pref_set("CONNECTION", "secure");
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "https://192-168-1-18.abcdef0123456789.plex.direct:32400/", strlen("https://192-168-1-18.abcdef0123456789.plex.direct:32400/")) == 0);
  p->close(ctx);

  /* 3. Empty CONNECTION means secure */
  mock_pref_set("CONNECTION", "");
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "https://192-168-1-18.abcdef0123456789.plex.direct:32400/", strlen("https://192-168-1-18.abcdef0123456789.plex.direct:32400/")) == 0);
  p->close(ctx);
}

static void test_manual_server_alone(const ZZPlayProvider *p)
{
  char err[128];
  void *ctx;
  int ok;
  ZZPlayResolved res;

  mock_prefs_clear();
  mock_pref_set("SERVER", "http://192.168.1.10:32400");
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  /* Empty CONNECTION */
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://192.168.1.10:32400/", strlen("http://192.168.1.10:32400/")) == 0);
  p->close(ctx);

  /* CONNECTION=plain */
  mock_pref_set("CONNECTION", "plain");
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://192.168.1.10:32400/", strlen("http://192.168.1.10:32400/")) == 0);
  p->close(ctx);

  /* CONNECTION=secure */
  mock_pref_set("CONNECTION", "secure");
  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://192.168.1.10:32400/", strlen("http://192.168.1.10:32400/")) == 0);
  p->close(ctx);
}

static void test_plain_plexdirect_fallback(const ZZPlayProvider *p)
{
  char err[128];
  void *ctx;
  int ok;
  ZZPlayResolved res;

  /* Signed in before SERVER_PLAIN was stored: plain converts the stored
   * plex.direct address */
  mock_prefs_clear();
  mock_pref_set("CONNECTION", "plain");
  mock_pref_set("SERVER_SECURE", "https://192-168-1-18.abcdef0123456789.plex.direct:32400");
  mock_pref_set("SERVER", "https://192-168-1-18.abcdef0123456789.plex.direct:32400");
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://192.168.1.18:32400/",
                 strlen("http://192.168.1.18:32400/")) == 0);
  p->close(ctx);

  /* SERVER alone converts too */
  mock_prefs_clear();
  mock_pref_set("CONNECTION", "plain");
  mock_pref_set("SERVER", "https://10-1-2-3.0123456789abcdef.plex.direct:32401");
  mock_pref_set("TOKEN", TEST_TOKEN);

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "http://10.1.2.3:32401/",
                 strlen("http://10.1.2.3:32401/")) == 0);
  p->close(ctx);

  /* A non-IPv4 plex.direct address keeps the stored address */
  mock_prefs_clear();
  mock_pref_set("CONNECTION", "plain");
  mock_pref_set("SERVER", "https://2001-db8--12.hash.plex.direct:32400");
  mock_pref_set("TOKEN", TEST_TOKEN);

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);
  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "plex://302", &res, err, sizeof(err));
  assert(ok == 1);
  assert(strncmp(res.url, "https://2001-db8--12.hash.plex.direct:32400/",
                 strlen("https://2001-db8--12.hash.plex.direct:32400/")) == 0);
  p->close(ctx);
}

static void test_failed_probe_stores_nothing_and_names_connection(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  char srv[256];
  char tok[128];

  /* Case A: HTTPS identity check fails on secure connection */
  mock_prefs_clear();
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 1;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 1;
  g_mock_http_identity_fails = 0;
  g_mock_two_servers = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strstr(err, "Connection") != NULL);
  assert(strcmp(err, "The server did not answer over https; set Connection to plain or check the server") == 0);

  /* Nothing stored in prefs */
  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_SECURE", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_PLAIN", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 0);
  p->close(ctx);

  /* Case B: HTTP identity check fails when CONNECTION=plain */
  mock_prefs_clear();
  mock_pref_set("CONNECTION", "plain");
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 1;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 0;
  g_mock_http_identity_fails = 1;
  g_mock_two_servers = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strstr(err, "Connection") != NULL);
  assert(strcmp(err, "The server did not answer over http; set Connection to secure or check the server") == 0);

  /* Only CONNECTION was in prefs; no server or token stored */
  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_SECURE", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_PLAIN", srv, sizeof(srv)) == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 0);
  p->close(ctx);
}

static void test_resources_two_servers(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  char srv[256];
  char tok[128];

  mock_prefs_clear();
  g_mock_poll_calls = 0;
  g_mock_poll_authenticated_after = 1;
  g_mock_sleep_calls = 0;
  g_mock_sleep_cancelled = 0;
  g_mock_https_identity_fails = 0;
  g_mock_two_servers = 1; /* returns resources-multi.xml with 2 servers */

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "signin:continue:736458353", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);

  assert(strcmp(col.items[0].id, "signin:server:0") == 0);
  assert(strcmp(col.items[0].title, "Server One") == 0);
  assert(strcmp(col.items[0].subtitle, "owned") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);

  assert(strcmp(col.items[1].id, "signin:server:1") == 0);
  assert(strcmp(col.items[1].title, "Server Two") == 0);
  assert(strcmp(col.items[1].subtitle, "shared") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);

  /* Neither server stored yet */
  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 0);

  /* Select Server Two */
  collector_init(&col, 0);
  ok = p->list(ctx, "signin:server:1", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);

  assert(fake_pref_get(&g_test_host, "SERVER", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "https://192-168-1-20.1234567890abcdef.plex.direct:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_SECURE", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "https://192-168-1-20.1234567890abcdef.plex.direct:32400") == 0);
  assert(fake_pref_get(&g_test_host, "SERVER_PLAIN", srv, sizeof(srv)) == 1);
  assert(strcmp(srv, "http://192.168.1.20:32400") == 0);
  assert(fake_pref_get(&g_test_host, "TOKEN", tok, sizeof(tok)) == 1);
  assert(strcmp(tok, "server-two-token") == 0);

  /* Lands in library top level */
  assert(col.count > 0);
  assert(strcmp(col.items[0].id, "playlists") == 0);

  p->close(ctx);
}

static void test_older_host_fails_signin_gracefully(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  ZZPlayHost old_host;

  old_host = g_test_host;
  old_host.size = (uint32_t)offsetof(ZZPlayHost, http_post);
  old_host.http_post = NULL;
  old_host.sleep = NULL;

  /* Without token, signin folder fails gracefully */
  mock_prefs_clear();
  ctx = p->open(&old_host, err, sizeof(err));
  assert(ctx != NULL);
  collector_init(&col, 0);
  ok = p->list(ctx, "signin", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strstr(err, "ZZPlay update required") != NULL);
  p->close(ctx);

  /* With manual settings, older host still works as today */
  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  ctx = p->open(&old_host, err, sizeof(err));
  assert(ctx != NULL);
  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 10, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count > 0);
  p->close(ctx);
}

static void test_sections_listing(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;
  g_mock_single_section = 0;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);

  /* sections.xml has 1 movie (key 1), 2 artist sections (key 3 "Music", key 5 "Audiobooks & Talk")
   * Top level should output:
   * 0: Playlists folder
   * 1: Music (sec:3)
   * 2: Audiobooks & Talk (sec:5)
   * Movie section must be filtered out!
   */
  assert(col.count == 3);
  assert(col.total == 3);
  assert(strcmp(col.items[0].id, "playlists") == 0);
  assert(strcmp(col.items[0].title, "Playlists") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);

  assert(strcmp(col.items[1].id, "sec:3") == 0);
  assert(strcmp(col.items[1].title, "Music") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);

  assert(strcmp(col.items[2].id, "sec:5") == 0);
  assert(strcmp(col.items[2].title, "Audiobooks & Talk") == 0);
  assert(col.items[2].kind == ZZPLAY_ITEM_FOLDER);

  /* A library folder opens the four views; Playlists stays at the top level */
  collector_init(&col, 0);
  ok = p->list(ctx, "sec:3", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 4);
  assert(col.total == 4);
  assert(strcmp(col.items[0].id, "lib:3:art") == 0);
  assert(strcmp(col.items[0].title, "Artists") == 0);
  assert(strcmp(col.items[1].id, "lib:3:alb") == 0);
  assert(strcmp(col.items[1].title, "Albums") == 0);
  assert(strcmp(col.items[2].id, "lib:3:gen") == 0);
  assert(strcmp(col.items[2].title, "Genres") == 0);
  assert(strcmp(col.items[3].id, "lib:3:new") == 0);
  assert(strcmp(col.items[3].title, "Recently added") == 0);
  for (int i = 0; i < 4; i++) {
    assert(col.items[i].kind == ZZPLAY_ITEM_FOLDER);
    assert(col.items[i].playable == 0);
  }

  p->close(ctx);
}

static void test_single_library_top(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;
  g_mock_single_section = 1;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);

  /* sections-single.xml has 1 movie section and exactly 1 music section
   * ("Music", key 3), so the top level holds the library's views directly:
   * Artists, Albums, Genres, Recently added, Playlists. */
  assert(col.count == 5);
  assert(col.total == 5);
  assert(strcmp(col.items[0].id, "lib:3:art") == 0);
  assert(strcmp(col.items[0].title, "Artists") == 0);
  assert(strcmp(col.items[1].id, "lib:3:alb") == 0);
  assert(strcmp(col.items[1].title, "Albums") == 0);
  assert(strcmp(col.items[2].id, "lib:3:gen") == 0);
  assert(strcmp(col.items[2].title, "Genres") == 0);
  assert(strcmp(col.items[3].id, "lib:3:new") == 0);
  assert(strcmp(col.items[3].title, "Recently added") == 0);
  assert(strcmp(col.items[4].id, "playlists") == 0);
  assert(strcmp(col.items[4].title, "Playlists") == 0);
  for (int i = 0; i < 5; i++) {
    assert(col.items[i].kind == ZZPLAY_ITEM_FOLDER);
  }

  p->close(ctx);
  g_mock_single_section = 0;
}

static void test_letter_folders(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* Artists opens to letter folders with counts, "#" first */
  collector_init(&col, 0);
  ok = p->list(ctx, "lib:3:art", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 3);
  assert(col.total == 3);
  assert(strcmp(col.items[0].id, "ltr:3:8:%23") == 0);
  assert(strcmp(col.items[0].title, "# (3)") == 0);
  assert(strcmp(col.items[1].id, "ltr:3:8:A") == 0);
  assert(strcmp(col.items[1].title, "A (3)") == 0);
  assert(strcmp(col.items[2].id, "ltr:3:8:B") == 0);
  assert(strcmp(col.items[2].title, "B (1)") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strstr(g_last_request_url, "/firstCharacter?type=8") != NULL);

  /* A letter opens its artists in Plex's order, paged */
  collector_init(&col, 0);
  ok = p->list(ctx, "ltr:3:8:A", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 3); /* letter-a-artists.xml: totalSize 3, 2 on the page */
  assert(strcmp(col.items[0].id, "art:101") == 0);
  assert(strcmp(col.items[0].title, "Alpha Band") == 0);
  assert(strcmp(col.items[1].id, "art:102") == 0);
  assert(strcmp(col.items[1].title, "Caf\xc3\xa9 & Co") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strstr(g_last_request_url, "/firstCharacter/A?type=8") != NULL);

  /* Request page two of a letter: start = 2, count = 2 */
  collector_init(&col, 0);
  ok = p->list(ctx, "ltr:3:8:A", 2, 2, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(strstr(g_last_request_headers, "X-Plex-Container-Start: 2\r\n") != NULL);
  assert(strstr(g_last_request_headers, "X-Plex-Container-Size: 2\r\n") != NULL);
  assert(col.total == 3);
  assert(col.count == 1);
  assert(strcmp(col.items[0].id, "art:103") == 0);
  assert(strcmp(col.items[0].title, "Zeta") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);

  /* Album letters and an album letter's rows, with the year in the subtitle */
  collector_init(&col, 0);
  ok = p->list(ctx, "lib:3:alb", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(strcmp(col.items[0].id, "ltr:3:9:%23") == 0);
  assert(strcmp(col.items[0].title, "# (4)") == 0);
  assert(strcmp(col.items[1].id, "ltr:3:9:A") == 0);
  assert(strcmp(col.items[1].title, "A (2)") == 0);

  collector_init(&col, 0);
  ok = p->list(ctx, "ltr:3:9:A", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "alb:203") == 0);
  assert(strcmp(col.items[0].title, "Aria") == 0);
  assert(strcmp(col.items[0].subtitle, "Alpha Band - 1981") == 0);
  assert(strcmp(col.items[1].id, "alb:204") == 0);
  assert(strcmp(col.items[1].title, "Ave Maria") == 0);
  assert(strcmp(col.items[1].subtitle, "Caf\xc3\xa9 & Co") == 0);

  p->close(ctx);
}

static void test_genres(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* Genres opens the genre list, without counts */
  collector_init(&col, 0);
  ok = p->list(ctx, "lib:3:gen", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 3);
  assert(col.total == 3);
  assert(strcmp(col.items[0].id, "gen:3:9001") == 0);
  assert(strcmp(col.items[0].title, "Jazz") == 0);
  assert(strcmp(col.items[1].id, "gen:3:9002") == 0);
  assert(strcmp(col.items[1].title, "Blues") == 0);
  assert(strcmp(col.items[2].id, "gen:3:9003") == 0);
  assert(strcmp(col.items[2].title, "Ambient") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strstr(g_last_request_url, "/genre?type=8") != NULL);

  /* A genre opens the artists in it */
  collector_init(&col, 0);
  ok = p->list(ctx, "gen:3:9001", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "art:104") == 0);
  assert(strcmp(col.items[0].title, "Blue Note Trio") == 0);
  assert(strcmp(col.items[1].id, "art:105") == 0);
  assert(strcmp(col.items[1].title, "Delta Five") == 0);
  assert(strstr(g_last_request_url, "type=8&genre=9001") != NULL);

  p->close(ctx);
}

static void test_recently_added(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "lib:3:new", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "alb:210") == 0);
  assert(strcmp(col.items[0].title, "Night Works") == 0);
  assert(strcmp(col.items[0].subtitle, "Delta Five - 2024") == 0);
  assert(strcmp(col.items[1].id, "alb:211") == 0);
  assert(strcmp(col.items[1].title, "Old Songs") == 0);
  assert(strcmp(col.items[1].subtitle, "1999") == 0);
  assert(strstr(g_last_request_url, "type=9&sort=addedAt:desc") != NULL);

  p->close(ctx);
}

static void test_album_and_track_rows(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* 1. Artist lists albums */
  collector_init(&col, 0);
  ok = p->list(ctx, "art:101", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(strcmp(col.items[0].id, "alb:201") == 0);
  assert(strcmp(col.items[0].title, "First Light") == 0);
  assert(strcmp(col.items[0].subtitle, "Alpha Band - 1975") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(col.items[0].playable == 0);

  assert(strcmp(col.items[1].id, "alb:202") == 0);
  assert(strcmp(col.items[1].title, "Second Wind") == 0);
  assert(strcmp(col.items[1].subtitle, "Alpha Band - 1978") == 0);

  /* 2. Album lists tracks */
  collector_init(&col, 0);
  ok = p->list(ctx, "alb:201", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 3);

  /* Track 301: Opening */
  assert(strcmp(col.items[0].id, "301") == 0);
  assert(strcmp(col.items[0].title, "Opening") == 0);
  assert(strcmp(col.items[0].subtitle, "Alpha Band - First Light") == 0);
  assert(col.items[0].duration_ms == 201000);
  assert(col.items[0].kind == ZZPLAY_ITEM_TRACK);
  assert(col.items[0].playable == 1);

  /* Track 302: Lossless */
  assert(strcmp(col.items[1].id, "302") == 0);
  assert(strcmp(col.items[1].title, "Lossless") == 0);
  assert(strcmp(col.items[1].subtitle, "Alpha Band - First Light") == 0);
  assert(col.items[1].duration_ms == 245500);
  assert(col.items[1].kind == ZZPLAY_ITEM_TRACK);
  assert(col.items[1].playable == 1);

  /* Track 303: Apple Format */
  assert(strcmp(col.items[2].id, "303") == 0);
  assert(strcmp(col.items[2].title, "Apple Format") == 0);
  assert(strcmp(col.items[2].subtitle, "Alpha Band - First Light") == 0);
  assert(col.items[2].duration_ms == 180000);
  assert(col.items[2].kind == ZZPLAY_ITEM_TRACK);
  assert(col.items[2].playable == 1);

  p->close(ctx);
}

static void test_search_grouped(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;
  g_mock_single_section = 1;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* "paris": artists 0 (left out), albums 2, tracks 21 */
  collector_init(&col, 0);
  ok = p->search(ctx, "paris", &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "sq:9:paris") == 0);
  assert(strcmp(col.items[0].title, "Albums (2)") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(strcmp(col.items[1].id, "sq:10:paris") == 0);
  assert(strcmp(col.items[1].title, "Tracks (21)") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);

  /* The album group opens its results, with the year in the subtitle */
  collector_init(&col, 0);
  ok = p->list(ctx, "sq:9:paris", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "alb:401") == 0);
  assert(strcmp(col.items[0].title, "Paris Concert") == 0);
  assert(strcmp(col.items[0].subtitle, "Blue Note Trio - 1990") == 0);
  assert(strcmp(col.items[1].id, "alb:402") == 0);
  assert(strcmp(col.items[1].subtitle, "Alpha Band - 2001") == 0);
  assert(strstr(g_last_request_url, "/search?type=9&query=paris") != NULL);

  /* "Lossless": only the track group has hits */
  collector_init(&col, 0);
  ok = p->search(ctx, "Lossless", &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(col.total == 1);
  assert(strcmp(col.items[0].id, "sq:10:Lossless") == 0);
  assert(strcmp(col.items[0].title, "Tracks (1)") == 0);

  /* The track group opens its results */
  collector_init(&col, 0);
  ok = p->list(ctx, "sq:10:Lossless", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(col.total == 1);
  assert(strcmp(col.items[0].id, "302") == 0);
  assert(strcmp(col.items[0].title, "Lossless") == 0);
  assert(strcmp(col.items[0].subtitle, "Alpha Band - First Light") == 0);
  assert(col.items[0].duration_ms == 245500);
  assert(col.items[0].kind == ZZPLAY_ITEM_TRACK);

  p->close(ctx);
  g_mock_single_section = 0;
}

static void test_search_group_across_libraries(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;
  g_mock_single_section = 0; /* sections.xml: music sections 3 and 5 */

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* Each library has one hit, so the group counts both */
  collector_init(&col, 0);
  ok = p->search(ctx, "Lossless", &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(col.total == 1);
  assert(strcmp(col.items[0].id, "sq:10:Lossless") == 0);
  assert(strcmp(col.items[0].title, "Tracks (2)") == 0);

  /* Opening it walks both libraries */
  collector_init(&col, 0);
  ok = p->list(ctx, "sq:10:Lossless", 0, 100, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 2);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "302") == 0);
  assert(strcmp(col.items[1].id, "309") == 0);
  assert(strcmp(col.items[1].title, "Lossless Too") == 0);

  /* A window inside the second library only */
  collector_init(&col, 0);
  ok = p->list(ctx, "sq:10:Lossless", 1, 1, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 1);
  assert(col.total == 2);
  assert(strcmp(col.items[0].id, "309") == 0);
  assert(strstr(g_last_request_url, "/library/sections/5/search?type=10") != NULL);
  assert(strstr(g_last_request_headers, "X-Plex-Container-Start: 0\r\n") != NULL);
  assert(strstr(g_last_request_headers, "X-Plex-Container-Size: 1\r\n") != NULL);

  p->close(ctx);
}

static void test_search_flat_when_query_too_long(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;
  char query[100];

  memset(query, 'L', sizeof(query));
  query[sizeof(query) - 1] = '\0';

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;
  g_mock_single_section = 1;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  /* The query does not fit in a folder id, so search stays one mixed list */
  collector_init(&col, 0);
  ok = p->search(ctx, query, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(col.count == 3);
  assert(strcmp(col.items[0].id, "302") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_TRACK);
  assert(strcmp(col.items[1].id, "alb:401") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);
  assert(strcmp(col.items[2].id, "alb:402") == 0);

  p->close(ctx);
  g_mock_single_section = 0;
}

static void test_list_requests_exclude_summaries(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  collector_init(&col, 0);
  ok = p->list(ctx, "ltr:3:8:A", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 1);
  assert(strstr(g_last_request_url, "excludeFields=summary") != NULL);

  assert(g_missing_exclude_count == 0);
  p->close(ctx);
}

static void test_resolve_flac(const ZZPlayProvider *p)
{
  char err[128];
  ZZPlayResolved res;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "302", &res, err, sizeof(err));
  assert(ok == 1);

  /* FLAC should direct play with byte range part URL + token */
  assert(strcmp(res.url, "http://192.168.1.10:32400/library/parts/502/1653313047/file.flac?X-Plex-Token=" TEST_TOKEN) == 0);
  assert(strcmp(res.title, "Alpha Band - Lossless") == 0);
  assert(res.duration_ms == 245500);

  p->close(ctx);
}

static void test_resolve_aac_transcode(const ZZPlayProvider *p)
{
  char err[128];
  ZZPlayResolved res;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", TEST_TOKEN);
  mock_pref_set("BITRATE", "192");
  g_mock_status = 200;
  g_last_decision_url[0] = '\0';

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  memset(&res, 0, sizeof(res));
  ok = p->resolve(ctx, "303", &res, err, sizeof(err));
  assert(ok == 1);

  /* Decision request must have been made */
  assert(strstr(g_last_decision_url, "/music/:/transcode/universal/decision?") != NULL);
  assert(strstr(g_last_decision_url, "path=%2Flibrary%2Fmetadata%2F303") != NULL);
  assert(strstr(g_last_decision_url, "musicBitrate=192") != NULL);

  /* start.mp3 URL must have Generic platform, profile extra, session id */
  assert(strstr(res.url, "http://192.168.1.10:32400/music/:/transcode/universal/start.mp3?") != NULL);
  assert(strstr(res.url, "X-Plex-Platform=Generic") != NULL);
  assert(strstr(res.url, "X-Plex-Client-Profile-Extra=add-transcode-target") != NULL);
  assert(strstr(res.url, "session=zzsess-303-") != NULL);
  assert(strstr(res.url, "X-Plex-Session-Identifier=zzsess-303-") != NULL);
  assert(strstr(res.url, "X-Plex-Token=" TEST_TOKEN) != NULL);
  assert(strstr(res.url, "musicBitrate=192") != NULL);

  assert(strcmp(res.title, "Alpha Band - Apple Format") == 0);
  assert(res.duration_ms == 180000);

  p->close(ctx);
}

static void test_error_401(const ZZPlayProvider *p)
{
  char err[128];
  ItemCollector col;
  void *ctx;
  int ok;

  mock_prefs_clear();
  mock_pref_set("SERVER", TEST_SERVER);
  mock_pref_set("TOKEN", "bad-token");
  g_mock_status = 200;

  ctx = p->open(&g_test_host, err, sizeof(err));
  assert(ctx != NULL);

  g_mock_status = 401;
  collector_init(&col, 0);
  ok = p->list(ctx, "playlists", 0, 50, &col.sink, err, sizeof(err));
  assert(ok == 0);
  assert(strcmp(err, "Plex rejected the token") == 0);

  p->close(ctx);
}

static void test_headers_on_every_request(void)
{
  assert(g_total_requests > 0);
  assert(g_headers_missing_count == 0);
}

int main(void)
{
  const ZZPlayProvider *p = zzplay_provider_entry();
  assert(p != NULL);
  assert(p->id == ZZPLAY_PROVIDER_ID);
  assert(p->abi == ZZPLAY_PROVIDER_ABI);
  assert(p->size == sizeof(ZZPlayProvider));
  assert(strcmp(p->scheme, "plex") == 0);
  assert(strcmp(p->name, "Plex Media Server") == 0);
  assert(p->field_count == 4);
  assert(strcmp(p->fields[0].key, "SERVER") == 0);
  assert(strcmp(p->fields[1].key, "CONNECTION") == 0);
  assert(strcmp(p->fields[1].label, "Connection") == 0);
  assert(p->fields[1].kind == ZZPLAY_FIELD_CHOICE);
  assert(p->fields[1].choice_values != NULL && p->fields[1].choice_labels != NULL);
  assert(strcmp(p->fields[1].choice_values[0], "secure") == 0);
  assert(strcmp(p->fields[1].choice_values[1], "plain") == 0);
  assert(p->fields[1].choice_values[2] == NULL);
  assert(strcmp(p->fields[1].choice_labels[0], "Secure (https)") == 0);
  assert(strcmp(p->fields[1].choice_labels[1], "Plain (http, faster)") == 0);
  assert(p->fields[1].choice_labels[2] == NULL);
  assert(p->fields[0].choice_values == NULL && p->fields[0].choice_labels == NULL);
  assert(p->fields[2].choice_values == NULL && p->fields[2].choice_labels == NULL);
  assert(p->fields[3].choice_values == NULL && p->fields[3].choice_labels == NULL);
  assert(strcmp(p->fields[2].key, "TOKEN") == 0);
  assert(strcmp(p->fields[3].key, "BITRATE") == 0);

  test_no_token_shows_signin_folder(p);
  test_pin_request(p);
  test_pin_poll_waiting_then_token(p);
  test_pin_poll_cancel_stores_nothing(p);
  test_resources_signin_stores_both_addresses(p);
  test_resources_signin_connection_plain(p);
  test_connection_selection_and_switching(p);
  test_manual_server_alone(p);
  test_plain_plexdirect_fallback(p);
  test_failed_probe_stores_nothing_and_names_connection(p);
  test_resources_two_servers(p);
  test_older_host_fails_signin_gracefully(p);
  test_sections_listing(p);
  test_single_library_top(p);
  test_letter_folders(p);
  test_genres(p);
  test_recently_added(p);
  test_album_and_track_rows(p);
  test_search_grouped(p);
  test_search_group_across_libraries(p);
  test_search_flat_when_query_too_long(p);
  test_list_requests_exclude_summaries(p);
  test_resolve_flac(p);
  test_resolve_aac_transcode(p);
  test_error_401(p);
  test_headers_on_every_request();

  printf("All zzplay_provider_plex_test checks passed (%u HTTP requests verified).\n",
         g_total_requests);
  return 0;
}
