/*
 * The XML scanner service modules use for Plex and radio-browser.info.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-xml.h"

/* Every callback appended as one line: "+name a=v b=w" or "-name", with
 * "!" after the name when the element was truncated. */
static char log_text[8192];

static void logf_append(const char *s)
{
  strncat(log_text, s, sizeof(log_text) - strlen(log_text) - 1U);
}

static int on_start(void *user, const char *name, const ZZPlayXmlAttr *attrs,
                    uint32_t count, int truncated)
{
  uint32_t i;

  (void)user;
  logf_append("+");
  logf_append(name);
  if (truncated) logf_append("!");
  for (i = 0U; i < count; i++) {
    logf_append(" ");
    logf_append(attrs[i].name);
    logf_append("=");
    logf_append(attrs[i].value);
  }
  logf_append("\n");
  return 0;
}

static int on_end(void *user, const char *name)
{
  (void)user;
  logf_append("-");
  logf_append(name);
  logf_append("\n");
  return 0;
}

static const char *scan(const char *doc, size_t step)
{
  ZZPlayXml xml;
  ZZPlayXmlHandler h;
  size_t len = strlen(doc);
  size_t pos;

  h.start = on_start;
  h.end = on_end;
  h.user = NULL;
  log_text[0] = '\0';
  zzplay_xml_init(&xml, &h);
  for (pos = 0U; pos < len; pos += step) {
    size_t n = len - pos < step ? len - pos : step;
    (void)zzplay_xml_feed(&xml, doc + pos, n);
  }
  return log_text;
}

static const char plex[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- a comment with > and -- inside -->\n"
    "<MediaContainer size=\"2\" title1='Music'>\n"
    "  <Directory key=\"/library/metadata/10/children\" title=\"Album &amp; Co\" />\n"
    "  <Track ratingKey=\"11\" title=\"Caf&#233; &#x2603;\" duration=\"201000\">\n"
    "    <Media audioCodec=\"flac\"><Part key=\"/library/parts/5/1/file.flac\"/></Media>\n"
    "  </Track>\n"
    "  <![CDATA[ <NotAnElement a=\"1\"/> ]]>\n"
    "</MediaContainer>\n";

static const char plex_want[] =
    "+MediaContainer size=2 title1=Music\n"
    "+Directory key=/library/metadata/10/children title=Album & Co\n"
    "-Directory\n"
    "+Track ratingKey=11 title=Caf\xc3\xa9 \xe2\x98\x83 duration=201000\n"
    "+Media audioCodec=flac\n"
    "+Part key=/library/parts/5/1/file.flac\n"
    "-Part\n"
    "-Media\n"
    "-Track\n"
    "-MediaContainer\n";

static int test_plex(void)
{
  if (strcmp(scan(plex, sizeof(plex)), plex_want) != 0) return 1;
  /* The same document one byte at a time, and in odd chunks. */
  if (strcmp(scan(plex, 1U), plex_want) != 0) return 2;
  if (strcmp(scan(plex, 7U), plex_want) != 0) return 3;
  return 0;
}

static int test_entities(void)
{
  if (strcmp(scan("<a v=\"&lt;&gt;&quot;&apos;&amp;\"/>", 3U),
             "+a v=<>\"'&\n-a\n") != 0)
    return 1;
  /* Unknown or malformed entities stay as written. */
  if (strcmp(scan("<a v=\"&nbsp;&#;&#xZZ;&\"/>", 64U),
             "+a v=&nbsp;&#;&#xZZ;&\n-a\n") != 0)
    return 2;
  return 0;
}

static int test_limits(void)
{
  static char doc[12000];
  static char want_title[ZZPLAY_XML_VALUE_MAX + 1U];
  size_t n;
  const char *got;

  /* A long summary is cut to the value limit, and the attributes after it
   * survive. */
  memset(want_title, 'x', ZZPLAY_XML_VALUE_MAX);
  want_title[ZZPLAY_XML_VALUE_MAX] = '\0';
  n = (size_t)snprintf(doc, sizeof(doc), "<Directory summary=\"");
  memset(doc + n, 'x', 3000U);
  n += 3000U;
  (void)snprintf(doc + n, sizeof(doc) - n,
                 "\" ratingKey=\"42\" title=\"Album\"/>");
  got = scan(doc, 100U);
  if (strncmp(got, "+Directory! summary=", 20U) != 0) return 1;
  if (strstr(got, " ratingKey=42 title=Album\n") == NULL) return 2;
  if (strstr(got, want_title) == NULL) return 3;
  {
    /* More attributes than the limit: the rest are dropped, marked. */
    size_t i;

    n = (size_t)snprintf(doc, sizeof(doc), "<a");
    for (i = 0U; i < ZZPLAY_XML_ATTRS_MAX + 5U; i++) {
      n += (size_t)snprintf(doc + n, sizeof(doc) - n, " k%u=\"%u\"",
                            (unsigned)i, (unsigned)i);
    }
    (void)snprintf(doc + n, sizeof(doc) - n, "/><b c=\"1\"/>");
    got = scan(doc, 13U);
    if (strncmp(got, "+a! k0=0", 8U) != 0) return 4;
    if (strstr(got, " k47=47\n") == NULL || strstr(got, "k48=") != NULL)
      return 5;
    if (strstr(got, "+b c=1\n-b\n") == NULL) return 6;
  }
  return 0;
}

static int stop_after_first(void *user, const char *name,
                            const ZZPlayXmlAttr *attrs, uint32_t count,
                            int truncated)
{
  int *seen = (int *)user;

  (void)name;
  (void)attrs;
  (void)count;
  (void)truncated;
  (*seen)++;
  return 1;
}

static int test_stop_and_lookup(void)
{
  ZZPlayXml xml;
  ZZPlayXmlHandler h;
  int seen = 0;
  ZZPlayXmlAttr attrs[2];

  h.start = stop_after_first;
  h.end = NULL;
  h.user = &seen;
  zzplay_xml_init(&xml, &h);
  if (zzplay_xml_feed(&xml, "<a/><b/>", 8U) != 1 || seen != 1) return 1;
  if (zzplay_xml_feed(&xml, "<c/>", 4U) != 1 || seen != 1) return 2;
  attrs[0].name = "key";
  attrs[0].value = "1";
  attrs[1].name = "title";
  attrs[1].value = "T";
  if (strcmp(zzplay_xml_attr(attrs, 2U, "title"), "T") != 0) return 3;
  if (zzplay_xml_attr(attrs, 2U, "missing") != NULL) return 4;
  return 0;
}

int main(void)
{
  int rc;

  rc = test_plex();
  if (rc != 0) { printf("plex %d\n%s", rc, log_text); return 10 + rc; }
  rc = test_entities();
  if (rc != 0) { printf("entities %d\n%s", rc, log_text); return 30 + rc; }
  rc = test_limits();
  if (rc != 0) { printf("limits %d\n%s", rc, log_text); return 50 + rc; }
  rc = test_stop_and_lookup();
  if (rc != 0) { printf("stop %d\n", rc); return 70 + rc; }
  printf("zzplay_xml_test: all checks passed\n");
  return 0;
}
