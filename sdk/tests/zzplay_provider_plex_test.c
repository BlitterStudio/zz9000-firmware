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
  if (strstr(url, "/library/sections/3/search") || strstr(url, "/library/sections/5/search")) {
    if (strstr(url, "type=10")) {
      return load_fixture("search-tracks.xml", data, user);
    } else {
      const char empty[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><MediaContainer size=\"0\"/>";
      if (data) data(user, (const uint8_t *)empty, (uint32_t)strlen(empty));
      return 1;
    }
  }
  if (strstr(url, "/library/sections/3/all?type=8")) {
    if (headers && strstr(headers, "X-Plex-Container-Start: 2")) {
      return load_fixture("artists-page2.xml", data, user);
    }
    return load_fixture("artists-page1.xml", data, user);
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
   * 0: Audio Playlists folder
   * 1: Music (sec:3)
   * 2: Audiobooks & Talk (sec:5)
   * Movie section must be filtered out!
   */
  assert(col.count == 3);
  assert(col.total == 3);
  assert(strcmp(col.items[0].id, "playlists") == 0);
  assert(strcmp(col.items[0].title, "Audio Playlists") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);

  assert(strcmp(col.items[1].id, "sec:3") == 0);
  assert(strcmp(col.items[1].title, "Music") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);

  assert(strcmp(col.items[2].id, "sec:5") == 0);
  assert(strcmp(col.items[2].title, "Audiobooks & Talk") == 0);
  assert(col.items[2].kind == ZZPLAY_ITEM_FOLDER);

  p->close(ctx);
}

static void test_single_section_flattening(const ZZPlayProvider *p)
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

  /* sections-single.xml has 1 movie section and exactly 1 music section ("Music", key 3).
   * Artists should appear directly at top level after the playlists folder!
   * Item 0: Audio Playlists
   * Item 1: Alpha Band (art:101)
   * Item 2: Café & Co (art:102)
   * Total reported should be totalSize + 1 = 3 + 1 = 4.
   */
  assert(col.count >= 3);
  assert(col.total == 4);
  assert(strcmp(col.items[0].id, "playlists") == 0);
  assert(strcmp(col.items[1].id, "art:101") == 0);
  assert(strcmp(col.items[1].title, "Alpha Band") == 0);
  assert(col.items[1].kind == ZZPLAY_ITEM_FOLDER);

  p->close(ctx);
  g_mock_single_section = 0;
}

static void test_artist_paging(const ZZPlayProvider *p)
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

  /* Request page two: start = 2, count = 2 */
  collector_init(&col, 0);
  ok = p->list(ctx, "sec:3", 2, 2, &col.sink, err, sizeof(err));
  assert(ok == 1);

  /* Verify start header was sent */
  assert(strstr(g_last_request_headers, "X-Plex-Container-Start: 2\r\n") != NULL);
  assert(strstr(g_last_request_headers, "X-Plex-Container-Size: 2\r\n") != NULL);

  /* artists-page2.xml has totalSize="3", 1 directory "Zeta" (ratingKey 103) */
  assert(col.total == 3);
  assert(col.count == 1);
  assert(strcmp(col.items[0].id, "art:103") == 0);
  assert(strcmp(col.items[0].title, "Zeta") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);

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
  assert(strcmp(col.items[0].subtitle, "1975") == 0);
  assert(col.items[0].kind == ZZPLAY_ITEM_FOLDER);
  assert(col.items[0].playable == 0);

  assert(strcmp(col.items[1].id, "alb:202") == 0);
  assert(strcmp(col.items[1].title, "Second Wind") == 0);
  assert(strcmp(col.items[1].subtitle, "1978") == 0);

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

static void test_search(const ZZPlayProvider *p)
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
  ok = p->search(ctx, "Lossless", &col.sink, err, sizeof(err));
  assert(ok == 1);

  assert(col.count >= 1);
  assert(strcmp(col.items[0].id, "302") == 0);
  assert(strcmp(col.items[0].title, "Lossless") == 0);
  assert(strcmp(col.items[0].subtitle, "Alpha Band - First Light") == 0);
  assert(col.items[0].duration_ms == 245500);
  assert(col.items[0].kind == ZZPLAY_ITEM_TRACK);

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
  ok = p->list(ctx, "sec:3", 0, 50, &col.sink, err, sizeof(err));
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
  test_single_section_flattening(p);
  test_artist_paging(p);
  test_album_and_track_rows(p);
  test_search(p);
  test_resolve_flac(p);
  test_resolve_aac_transcode(p);
  test_error_401(p);
  test_headers_on_every_request();

  printf("All zzplay_provider_plex_test checks passed (%u HTTP requests verified).\n",
         g_total_requests);
  return 0;
}
