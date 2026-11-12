/* Host tests for zzplay-host.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-host.h"

static void test_url_escape(void)
{
  char out[64];
  uint32_t len;

  /* Empty string */
  memset(out, 'X', sizeof(out));
  len = zzplay_host_url_escape("", out, sizeof(out));
  assert(len == 0U);
  assert(out[0] == '\0');

  /* Zero buffer size */
  len = zzplay_host_url_escape("test", out, 0U);
  assert(len == 0U);

  /* Buffer too small */
  len = zzplay_host_url_escape("hello world", out, 5U);
  assert(len == 0U);

  /* Exact buffer fit for "abc": needs 4 bytes ("abc\0") */
  len = zzplay_host_url_escape("abc", out, 4U);
  assert(len == 3U);
  assert(strcmp(out, "abc") == 0);

  /* Not enough room for terminator with exact string length */
  len = zzplay_host_url_escape("abc", out, 3U);
  assert(len == 0U);

  /* Unreserved characters remain unchanged: a-z, A-Z, 0-9, -, _, ., ~ */
  len = zzplay_host_url_escape("abcXYZ012-._~", out, sizeof(out));
  assert(len == 13U);
  assert(strcmp(out, "abcXYZ012-._~") == 0);

  /* Reserved characters and spaces percent-encoded */
  len = zzplay_host_url_escape("hello world! a=b&c=d", out, sizeof(out));
  assert(strcmp(out, "hello%20world%21%20a%3Db%26c%3Dd") == 0);
  assert(len == (uint32_t)strlen(out));

  /* Non-ASCII / high-bit characters */
  len = zzplay_host_url_escape("\xC3\xA9", out, sizeof(out));
  assert(strcmp(out, "%C3%A9") == 0);
  assert(len == 6U);
}

static void test_pref_text_get(void)
{
  static const char prefs[] =
      "; Comment at start\n"
      "\n"
      "# Another comment\n"
      "station=Blasmusikradio\n"
      "url=http://example.com/stream?q=1&a=2\n"
      "token=abc=def== ghi\n"
      "empty=\n"
      "spaced_key = value with spaces \r\n"
      "; Trailing comment\n";

  char val[128];
  int ok;

  /* Missing key */
  memset(val, 'Z', sizeof(val));
  ok = zzplay_host_pref_text_get(prefs, "nonexistent", val, sizeof(val));
  assert(!ok);
  assert(val[0] == '\0');

  /* Standard key */
  ok = zzplay_host_pref_text_get(prefs, "station", val, sizeof(val));
  assert(ok);
  assert(strcmp(val, "Blasmusikradio") == 0);

  /* Key with '=' in value */
  ok = zzplay_host_pref_text_get(prefs, "url", val, sizeof(val));
  assert(ok);
  assert(strcmp(val, "http://example.com/stream?q=1&a=2") == 0);

  /* Value with multiple '=' and spaces */
  ok = zzplay_host_pref_text_get(prefs, "token", val, sizeof(val));
  assert(ok);
  assert(strcmp(val, "abc=def== ghi") == 0);

  /* Empty value */
  ok = zzplay_host_pref_text_get(prefs, "empty", val, sizeof(val));
  assert(ok);
  assert(strcmp(val, "") == 0);

  /* Spaced key and CRLF line ending */
  ok = zzplay_host_pref_text_get(prefs, "spaced_key", val, sizeof(val));
  assert(ok);
  assert(strcmp(val, " value with spaces ") == 0);

  /* Buffer too small */
  memset(val, 'Z', sizeof(val));
  ok = zzplay_host_pref_text_get(prefs, "station", val, 5U);
  assert(!ok);
  assert(val[0] == '\0');

  /* Invalid key characters rejected */
  ok = zzplay_host_pref_text_get(prefs, "bad-key", val, sizeof(val));
  assert(!ok);
  ok = zzplay_host_pref_text_get(prefs, "bad key", val, sizeof(val));
  assert(!ok);
}

static void test_pref_text_set(void)
{
  char out[512];
  char val[128];
  int ok;

  /* 1. Add key to empty input */
  ok = zzplay_host_pref_text_set("", "key1", "val1", out, sizeof(out));
  assert(ok);
  assert(strcmp(out, "key1=val1\n") == 0);

  /* 2. Add second key to existing */
  ok = zzplay_host_pref_text_set(out, "key2", "val2=with=equals and spaces",
                                out, sizeof(out));
  assert(ok);
  ok = zzplay_host_pref_text_get(out, "key1", val, sizeof(val));
  assert(ok && strcmp(val, "val1") == 0);
  ok = zzplay_host_pref_text_get(out, "key2", val, sizeof(val));
  assert(ok && strcmp(val, "val2=with=equals and spaces") == 0);

  /* 3. Replace existing key, keeping comments and blank lines */
  {
    static const char original[] =
        "; Config file\n"
        "\n"
        "# Comment before target\n"
        "target=old_value\n"
        "# Comment after target\n"
        "other=keep_me\n";

    ok = zzplay_host_pref_text_set(original, "target", "new_value", out,
                                   sizeof(out));
    assert(ok);

    /* Verify structure */
    static const char expected[] =
        "; Config file\n"
        "\n"
        "# Comment before target\n"
        "target=new_value\n"
        "# Comment after target\n"
        "other=keep_me\n";
    assert(strcmp(out, expected) == 0);

    /* Verify get */
    ok = zzplay_host_pref_text_get(out, "target", val, sizeof(val));
    assert(ok && strcmp(val, "new_value") == 0);
    ok = zzplay_host_pref_text_get(out, "other", val, sizeof(val));
    assert(ok && strcmp(val, "keep_me") == 0);
  }

  /* 4. Append new key when not found, preserving existing comments */
  {
    static const char original[] =
        "# Header\n"
        "first=1\n";

    ok = zzplay_host_pref_text_set(original, "second", "2", out, sizeof(out));
    assert(ok);
    static const char expected[] =
        "# Header\n"
        "first=1\n"
        "second=2\n";
    assert(strcmp(out, expected) == 0);
  }

  /* 5. Input without trailing newline gets newline before appended key */
  {
    static const char original[] = "first=1";

    ok = zzplay_host_pref_text_set(original, "second", "2", out, sizeof(out));
    assert(ok);
    static const char expected[] =
        "first=1\n"
        "second=2\n";
    assert(strcmp(out, expected) == 0);
  }

  /* 6. Buffer too small fails gracefully */
  {
    char small[8];
    ok = zzplay_host_pref_text_set("a=1\n", "longkey", "longvalue", small,
                                   sizeof(small));
    assert(!ok);
  }
}

static int dummy_keep_running_cancel(void *user)
{
  int *count = (int *)user;
  (*count)--;
  return *count > 0 ? 1 : 0;
}

static void test_host_binding_and_table(void)
{
  ZZPlayHostBinding binding;
  char buf[64];
  void *mem;

  zzplay_host_bind(&binding, "test_scheme", NULL, NULL);

  assert(binding.table.abi == ZZPLAY_PROVIDER_ABI);
  assert(binding.table.size == (uint32_t)sizeof(ZZPlayHost));
  assert(strcmp(binding.scheme, "test_scheme") == 0);

  /* Alloc and free via host table */
  mem = binding.table.alloc(&binding.table, 128U);
  assert(mem != NULL);
  /* Allocated memory is cleared */
  assert(((const char *)mem)[0] == 0);
  assert(((const char *)mem)[127] == 0);
  binding.table.free(&binding.table, mem);

  /* Format via host table */
  binding.table.format(buf, sizeof(buf), "%s: %d", "count", 42);
  assert(strcmp(buf, "count: 42") == 0);

  /* URL escape via host table */
  binding.table.url_escape("a b", buf, sizeof(buf));
  assert(strcmp(buf, "a%20b") == 0);

  /* http_post and sleep pointers are filled */
  assert(binding.table.http_post != NULL);
  assert(binding.table.sleep != NULL);

  /* Sleep without cancel */
  assert(binding.table.sleep(&binding.table, 500U) == 1);

  /* Sleep with cancel */
  {
    int cancel_after = 2;
    ZZPlayHostBinding cancel_binding;
    zzplay_host_bind(&cancel_binding, "test_scheme", dummy_keep_running_cancel, &cancel_after);
    assert(cancel_binding.table.sleep(&cancel_binding.table, 500U) == 0);
  }
}

typedef struct XmlTestState {
  int start_count;
  int end_count;
  char last_element[32];
  char last_attr_val[32];
} XmlTestState;

static int xml_start_cb(void *user, const char *name,
                        const ZZPlayHostXmlAttr *attrs, uint32_t count,
                        int truncated)
{
  XmlTestState *st = (XmlTestState *)user;
  (void)truncated;
  st->start_count++;
  strncpy(st->last_element, name, sizeof(st->last_element) - 1U);
  if (count > 0 && attrs) {
    strncpy(st->last_attr_val, attrs[0].value, sizeof(st->last_attr_val) - 1U);
  }
  return 0;
}

static int xml_end_cb(void *user, const char *name)
{
  XmlTestState *st = (XmlTestState *)user;
  (void)name;
  st->end_count++;
  return 0;
}

static void test_host_xml_wrapper(void)
{
  ZZPlayHostBinding binding;
  ZZPlayHostXmlHandler handler;
  XmlTestState state;
  void *xml;
  static const char sample[] = "<station name=\"Rock FM\" codec=\"MP3\"></station>";

  zzplay_host_bind(&binding, "radio", NULL, NULL);

  memset(&state, 0, sizeof(state));
  handler.start = xml_start_cb;
  handler.end = xml_end_cb;
  handler.user = &state;

  xml = binding.table.xml_open(&binding.table, &handler);
  assert(xml != NULL);

  binding.table.xml_feed(&binding.table, xml, sample, (uint32_t)strlen(sample));
  binding.table.xml_close(&binding.table, xml);

  assert(state.start_count == 1);
  assert(state.end_count == 1);
  assert(strcmp(state.last_element, "station") == 0);
  assert(strcmp(state.last_attr_val, "Rock FM") == 0);
}

int main(void)
{
  test_url_escape();
  test_pref_text_get();
  test_pref_text_set();
  test_host_binding_and_table();
  test_host_xml_wrapper();

  printf("zzplay_host_test: all assertions passed\n");
  return 0;
}
