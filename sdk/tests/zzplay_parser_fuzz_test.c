/*
 * Mutation and random-input runs over ZZPlay's network parsers: HTTP
 * response heads, URLs and redirects, chunked bodies, ICY metadata, the
 * XML scanner, and M3U/PLS playlists. Deterministic seeds; built with
 * the sanitizers, an overrun or overflow fails the run. Each input sits
 * in a heap block of exactly its size, so a read past its end is caught.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/zzplay/zzplay-http.h"
#include "../tools/zzplay/zzplay-playlist.h"
#include "../tools/zzplay/zzplay-streamlist.h"
#include "../tools/zzplay/zzplay-xml.h"

#define ROUNDS 3000U
#define WORK_MAX (96U * 1024U)

static int failures;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);     \
      failures++;                                                         \
    }                                                                     \
  } while (0)

/* --- Deterministic input generation --- */

static uint32_t rng = 0x2545F491U;

static uint32_t rnd(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}

static uint32_t below(uint32_t n)
{
  return n != 0U ? rnd() % n : 0U;
}

static uint8_t work[WORK_MAX];
static size_t work_len;

static void work_set(const char *seed, size_t len)
{
  if (len > WORK_MAX) {
    len = WORK_MAX;
  }
  memcpy(work, seed, len);
  work_len = len;
}

static void work_insert(size_t at, const uint8_t *bytes, size_t n)
{
  if (at > work_len || n > WORK_MAX - work_len) {
    return;
  }
  memmove(work + at + n, work + at, work_len - at);
  memcpy(work + at, bytes, n);
  work_len += n;
}

static const uint8_t interesting[] = {
  0x00, 0xFF, 0x7F, 0x80, 0xC3, 0xE2, 0xF0, 0xBF, '\r', '\n', '\t', ' ',
  '<', '>', '"', '\'', '&', ';', ':', '/', '%', '=', '#', '?', '-', '*',
  '0', '9', 'F', 'x', '[', ']', '!', ','
};

/* Apply a few random edits to the work buffer: bit flips, byte changes,
 * dictionary tokens, deletions, duplications, truncation and noise. */
static void mutate(const char *const *dict, size_t dict_count)
{
  uint32_t edits = 1U + below(8U);
  uint32_t e;

  for (e = 0U; e < edits; e++) {
    size_t at = below((uint32_t)work_len + 1U);

    switch (below(9U)) {
    case 0:
      if (work_len != 0U && at < work_len) {
        work[at] ^= (uint8_t)(1U << below(8U));
      }
      break;
    case 1:
      if (at < work_len) {
        work[at] = (uint8_t)rnd();
      }
      break;
    case 2:
      if (at < work_len) {
        work[at] = interesting[below(sizeof(interesting))];
      }
      break;
    case 3:
    case 4:
      if (dict_count != 0U) {
        const char *t = dict[below((uint32_t)dict_count)];
        work_insert(at, (const uint8_t *)t, strlen(t));
      }
      break;
    case 5:
      if (at < work_len) {
        size_t n = 1U + below((uint32_t)(work_len - at));
        memmove(work + at, work + at + n, work_len - at - n);
        work_len -= n;
      }
      break;
    case 6:
      if (at < work_len) {
        static uint8_t copy[512];
        size_t n = 1U + below((uint32_t)(work_len - at));
        size_t to = below((uint32_t)work_len + 1U);
        if (n > sizeof(copy)) {
          n = sizeof(copy);
        }
        memcpy(copy, work + at, n);
        work_insert(to, copy, n);
      }
      break;
    case 7:
      work_len = at;
      break;
    default: {
      uint8_t noise[16];
      size_t n = 1U + below(sizeof(noise));
      size_t i;
      for (i = 0U; i < n; i++) {
        noise[i] = (uint8_t)rnd();
      }
      work_insert(at, noise, n);
      break;
    }
    }
  }
}

/* Fill the work buffer with plain random bytes. */
static void random_input(size_t max)
{
  size_t i;

  work_len = below((uint32_t)max + 1U);
  for (i = 0U; i < work_len; i++) {
    work[i] = (uint8_t)(below(4U) == 0U ? interesting[below(sizeof(interesting))]
                                        : rnd());
  }
}

/* A heap copy of exactly the input; `nul` adds a terminator past it. */
static char *input_copy(int nul)
{
  char *p = (char *)malloc(work_len + (nul ? 1U : 0U) + (work_len == 0U));
  if (p == NULL) {
    abort();
  }
  memcpy(p, work, work_len);
  if (nul) {
    p[work_len] = '\0';
  }
  return p;
}

static void next_input(const char *const *seeds, size_t seed_count,
                       const char *const *dict, size_t dict_count,
                       size_t random_max)
{
  if (below(8U) == 0U) {
    random_input(random_max);
    return;
  }
  {
    const char *s = seeds[below((uint32_t)seed_count)];
    work_set(s, strlen(s));
  }
  mutate(dict, dict_count);
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

/* True when `s` does not end inside a UTF-8 sequence its lead byte
 * started. */
static int utf8_tail_complete(const char *s)
{
  size_t len = strlen(s);
  size_t back = 0U;

  while (len > 0U && back < 3U && ((unsigned char)s[len - 1U] & 0xC0U) == 0x80U) {
    len--;
    back++;
  }
  if (len > 0U) {
    unsigned char lead = (unsigned char)s[len - 1U];
    size_t need = lead >= 0xF0U ? 4U : lead >= 0xE0U ? 3U : lead >= 0xC0U ? 2U : 1U;
    return need == 1U || back + 1U >= need;
  }
  return 1;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* --- HTTP response heads --- */

static const char *const head_seeds[] = {
  "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nAccept-Ranges: bytes\r\n"
  "Content-Type: audio/mpeg\r\n",
  "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 100-199/1000\r\n"
  "Content-Length: 100\r\n",
  "ICY 200 OK\r\nicy-name: Test FM\r\nicy-metaint: 16000\r\n",
  "HTTP/1.0 302 Found\r\nLocation: http://example.com/a/b?c=d\r\n",
  "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n",
  "HTTP/1.1 206 x\r\nContent-Range: bytes 0-0/*\r\n"
};

static const char *const head_dict[] = {
  "\r\n", "\n", "Content-Length: ", "Content-Range: bytes ", "4294967295",
  "4294967296", "99999999999", "-", "/", "*", "icy-metaint:", "icy-name:",
  "Location: ", "Transfer-Encoding: chunked", "Transfer-Encoding: gzip",
  "content-type:", "HTTP/1.1 ", "ICY ", " \t ", ":"
};

static void fuzz_head(void)
{
  uint32_t r;

  for (r = 0U; r < ROUNDS; r++) {
    ZZPlayHttpHead head;
    char *in;

    next_input(head_seeds, COUNT(head_seeds), head_dict, COUNT(head_dict),
               512U);
    in = input_copy(0);
    if (zzplay_http_parse_head(in, work_len, &head)) {
      CHECK(head.status >= 100 && head.status <= 599);
      CHECK(memchr(head.icy_name, 0, sizeof(head.icy_name)) != NULL);
      CHECK(memchr(head.content_type, 0, sizeof(head.content_type)) != NULL);
      CHECK(memchr(head.location, 0, sizeof(head.location)) != NULL);
      if (head.has_total) {
        CHECK(head.has_range && head.total > head.range_start);
      }
    }
    free(in);
  }
}

/* --- URLs and redirects --- */

static const char *const url_seeds[] = {
  "http://example.com/a/b.mp3", "https://host:8443/x?y=z#frag",
  "http://1.2.3.4", "HTTP://H/ a b", "http://h/%C3%A9", "https://a.b/?q",
  "http://h:1/", "http://h:65535/x/y/z"
};

static const char *const loc_seeds[] = {
  "/abs/path", "rel.mp3", "//other.host/x", "http://a/b", "?q=1", "../x",
  "https://secure/x", "a/b/c?d"
};

static const char *const url_dict[] = {
  "http://", "https://", "//", "/", ":", "@", "[", "]", "?", "#", "%",
  "65536", "0", ":80", "..", " ", "\x01", "\x7f", "\xc3\xa9"
};

static void fuzz_url(void)
{
  static char request[4096];
  uint32_t r;

  for (r = 0U; r < ROUNDS; r++) {
    ZZPlayUrl url;
    ZZPlayUrl target;
    char resolved[ZZPLAY_HTTP_URL_MAX];
    char *in;

    next_input(url_seeds, COUNT(url_seeds), url_dict, COUNT(url_dict), 300U);
    in = input_copy(1);
    (void)zzplay_http_is_url(in);
    if (zzplay_http_parse_url(in, &url)) {
      size_t n;

      CHECK(url.host[0] != '\0');
      CHECK(memchr(url.host, 0, sizeof(url.host)) != NULL);
      CHECK(url.path[0] == '/');
      CHECK(memchr(url.path, 0, sizeof(url.path)) != NULL);
      CHECK(url.port != 0U);
      n = zzplay_http_format_request(request, sizeof(request),
                                     below(2U) ? "POST" : NULL, &url,
                                     "ZZPlay fuzz", (int)below(2U), rnd(),
                                     (int)below(2U), below(100U), NULL);
      CHECK(n == 0U || n == strlen(request));

      free(in);
      next_input(loc_seeds, COUNT(loc_seeds), url_dict, COUNT(url_dict),
                 300U);
      in = input_copy(1);
      if (zzplay_http_resolve(&url, in, resolved, sizeof(resolved))) {
        CHECK(strlen(resolved) < sizeof(resolved));
        if (zzplay_http_parse_url(resolved, &target)) {
          (void)zzplay_http_redirect_allowed(&url, &target);
          (void)zzplay_http_same_origin(&url, &target);
        }
      }
      /* Small output buffers must refuse, never overrun. */
      {
        char tiny[8];
        (void)zzplay_http_resolve(&url, in, tiny, below(sizeof(tiny)) + 1U);
      }
    }
    free(in);
  }
}

/* --- Chunked bodies --- */

typedef struct ChunkSink {
  size_t total;
  int refuse_at;
  int calls;
} ChunkSink;

static int chunk_sink(void *ctx, const uint8_t *data, size_t len)
{
  ChunkSink *s = (ChunkSink *)ctx;

  if (len != 0U) {
    volatile uint8_t touch = data[0];
    touch = data[len - 1U];
    (void)touch;
  }
  s->total += len;
  s->calls++;
  return s->refuse_at != 0 && s->calls >= s->refuse_at;
}

static const char *const chunk_seeds[] = {
  "5\r\nhello\r\n0\r\n\r\n",
  "a;ext=1\r\n0123456789\r\n3\r\nabc\r\n0\r\nTrailer: x\r\n\r\n",
  "FFFFFFF\r\nxyz",
  "1\r\nx\r\n1\r\ny\r\n0\r\n\r\n"
};

static const char *const chunk_dict[] = {
  "\r\n", "0\r\n\r\n", ";", " ", "FFFFFFFF", "fffffff", "\r", "\n", "1",
  "Trailer: y\r\n"
};

static void fuzz_chunked(void)
{
  uint32_t r;

  for (r = 0U; r < ROUNDS; r++) {
    ZZPlayChunked c;
    ChunkSink sink;
    size_t pos = 0U;
    int rc = ZZPLAY_CHUNKED_MORE;
    uint8_t *in;

    next_input(chunk_seeds, COUNT(chunk_seeds), chunk_dict,
               COUNT(chunk_dict), 400U);
    in = (uint8_t *)input_copy(0);
    memset(&sink, 0, sizeof(sink));
    sink.refuse_at = below(20U) == 0U ? (int)(1U + below(3U)) : 0;
    zzplay_chunked_init(&c);
    while (pos < work_len) {
      size_t piece = 1U + below((uint32_t)(work_len - pos));
      size_t used = (size_t)-1;
      uint8_t *chunk = (uint8_t *)malloc(piece);

      memcpy(chunk, in + pos, piece);
      rc = zzplay_chunked_feed(&c, chunk, piece, chunk_sink, &sink, &used);
      free(chunk);
      CHECK(used <= piece);
      if (rc == ZZPLAY_CHUNKED_MORE) {
        CHECK(used == piece);
      }
      pos += used;
      if (rc != ZZPLAY_CHUNKED_MORE) {
        break;
      }
    }
    CHECK(sink.total <= work_len);
    if (rc != ZZPLAY_CHUNKED_MORE) {
      /* Once finished, further bytes are left alone. */
      size_t used = 1U;
      int again = zzplay_chunked_feed(&c, (const uint8_t *)"x", 1U, chunk_sink,
                                      &sink, &used);
      CHECK(again == rc && used == 0U);
    }
    free(in);
  }
}

/* --- ICY metadata --- */

static const char *const meta_seeds[] = {
  "StreamTitle='Artist - Title';StreamUrl='';",
  "StreamTitle='Guns N' Roses - Patience';",
  "StreamTitle='';", "StreamTitle='x",
  "StreamTitle='\xc3\xa9t\xc3\xa9 \xe2\x80\x94 caf\xc3\xa9';"
};

static const char *const meta_dict[] = {
  "StreamTitle='", "';", "'", ";", "\r\n", "\x01", "\x7f", "\xc3", "\xe2\x80",
  "\xf0\x9f\x8e\xb5", "  "
};

static void check_title(const char *title, size_t size)
{
  CHECK(memchr(title, 0, size) != NULL);
  CHECK(!has_control(title));
}

static void fuzz_icy(void)
{
  uint32_t r;

  for (r = 0U; r < ROUNDS; r++) {
    static const size_t sizes[] = { 1U, 2U, 3U, 5U, 16U, 128U, 4081U };
    size_t size = sizes[below(COUNT(sizes))];
    char *out = (char *)malloc(size);
    char *in;

    next_input(meta_seeds, COUNT(meta_seeds), meta_dict, COUNT(meta_dict),
               600U);
    in = input_copy(0);
    if (zzplay_icy_stream_title(in, work_len, out, size)) {
      static char full[8192];

      check_title(out, size);
      /* A title that had to be cut ends on a whole UTF-8 sequence. */
      CHECK(zzplay_icy_stream_title(in, work_len, full, sizeof(full)));
      if (strlen(full) > strlen(out)) {
        CHECK(utf8_tail_complete(out));
        CHECK(strncmp(full, out, strlen(out)) == 0);
      }
    }
    free(in);
    free(out);
  }

  /* A stream: audio runs, length bytes and metadata blocks, fed in
   * random splits, and plain noise. */
  for (r = 0U; r < ROUNDS; r++) {
    static ZZPlayIcy icy;
    uint32_t interval = 1U + below(300U);
    size_t total = 0U;
    size_t audio = 0U;
    size_t pos;
    uint8_t *stream;

    work_len = 0U;
    while (work_len < 6000U) {
      size_t n = interval;
      uint8_t len_byte;

      while (n-- != 0U && work_len < WORK_MAX) {
        work[work_len++] = (uint8_t)rnd();
      }
      audio += interval;
      if (below(3U) == 0U) {
        static uint8_t block[16U * 255U];
        const char *s = meta_seeds[below(COUNT(meta_seeds))];
        size_t sl = strlen(s);
        size_t blocks = 1U + below(255U);
        size_t i;

        memset(block, 0, sizeof(block));
        for (i = 0U; i < blocks * 16U; i++) {
          block[i] = below(4U) == 0U ? (uint8_t)rnd() : (uint8_t)s[i % (sl + 1U)];
        }
        len_byte = (uint8_t)blocks;
        work_insert(work_len, &len_byte, 1U);
        work_insert(work_len, block, blocks * 16U);
      } else {
        len_byte = 0U;
        work_insert(work_len, &len_byte, 1U);
      }
    }
    if (below(4U) == 0U) {
      random_input(4000U); /* noise: length bytes land anywhere */
      audio = work_len;
    }
    stream = (uint8_t *)input_copy(0);
    zzplay_icy_init(&icy, interval);
    pos = 0U;
    while (pos < work_len) {
      size_t piece = 1U + below((uint32_t)(work_len - pos));
      uint8_t *buf = (uint8_t *)malloc(piece);
      size_t kept;

      memcpy(buf, stream + pos, piece);
      kept = zzplay_icy_strip(&icy, buf, piece);
      CHECK(kept <= piece);
      total += kept;
      if (icy.title_changed) {
        icy.title_changed = 0;
        check_title(icy.title, sizeof(icy.title));
      }
      free(buf);
      pos += piece;
    }
    CHECK(total <= audio);
    free(stream);
  }
}

/* --- XML scanner --- */

typedef struct XmlCheck {
  ZZPlayXml *xml;
  uint32_t starts;
  uint32_t ends;
  uint32_t stop_after;
} XmlCheck;

/* Every name and value must lie in the scanner's text buffer and end
 * there. */
static void check_in_text(const ZZPlayXml *xml, const char *s)
{
  const char *lo = xml->text;
  const char *hi = xml->text + sizeof(xml->text);

  CHECK(s >= lo && s < hi);
  if (s >= lo && s < hi) {
    CHECK(memchr(s, 0, (size_t)(hi - s)) != NULL);
  }
}

static int xml_start(void *user, const char *name, const ZZPlayXmlAttr *attrs,
                     uint32_t count, int truncated)
{
  XmlCheck *c = (XmlCheck *)user;
  uint32_t i;

  (void)truncated;
  CHECK(strlen(name) < ZZPLAY_XML_NAME_MAX);
  CHECK(count <= ZZPLAY_XML_ATTRS_MAX);
  for (i = 0U; i < count; i++) {
    check_in_text(c->xml, attrs[i].name);
    check_in_text(c->xml, attrs[i].value);
    CHECK(strlen(attrs[i].value) <= ZZPLAY_XML_VALUE_MAX);
  }
  if (count != 0U) {
    (void)zzplay_xml_attr(attrs, count, attrs[count - 1U].name);
  }
  c->starts++;
  return c->stop_after != 0U && c->starts >= c->stop_after;
}

static int xml_end(void *user, const char *name)
{
  XmlCheck *c = (XmlCheck *)user;

  CHECK(strlen(name) < ZZPLAY_XML_NAME_MAX);
  c->ends++;
  return 0;
}

static void xml_run(const uint8_t *data, size_t len, uint32_t stop_after)
{
  ZZPlayXml *xml = (ZZPlayXml *)malloc(sizeof(ZZPlayXml));
  ZZPlayXmlHandler h;
  XmlCheck c;
  size_t pos = 0U;

  memset(&c, 0, sizeof(c));
  c.xml = xml;
  c.stop_after = stop_after;
  h.start = xml_start;
  h.end = xml_end;
  h.user = &c;
  zzplay_xml_init(xml, &h);
  while (pos < len) {
    size_t piece = 1U + below((uint32_t)(len - pos));
    char *chunk = (char *)malloc(piece);
    int stopped;

    memcpy(chunk, data + pos, piece);
    stopped = zzplay_xml_feed(xml, chunk, piece);
    free(chunk);
    pos += piece;
    if (stopped) {
      CHECK(zzplay_xml_feed(xml, "<a/>", 4U) == 1);
      break;
    }
  }
  free(xml);
}

static const char *const xml_seeds[] = {
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<MediaContainer size=\"2\" "
  "totalSize=\"40\"><Directory ratingKey=\"101\" type=\"artist\" "
  "title=\"Ab &amp; Cd\"/><Track ratingKey=\"7\" title='It&apos;s' "
  "duration=\"1000\"><Media audioCodec=\"mp3\"><Part key=\"/a/b.mp3\"/>"
  "</Media></Track></MediaContainer>",
  "<result><station stationuuid=\"1-2\" name=\"Radio &#x263A; &#9731;\" "
  "codec=\"MP3\"/></result>",
  "<!DOCTYPE x><!-- c -- > --><![CDATA[ <a> ]]><a b=c d e = 'f' g=\"\"/>",
  "<a x=\"&#0;&#x110000;&#xD800;&#99999999999;&#;&#x;&amp&lt;&gt;&quot;\"/>",
  "<a\tb\n=\r\"v\"/ ></a ><a/x>"
};

static const char *const xml_dict[] = {
  "<", ">", "/>", "</", "=\"", "\"", "'", "&amp;", "&#x10FFFF;", "&#1;",
  "&#", "<!--", "-->", "<![CDATA[", "]]>", "<?", "?>", "<!", " ", "\xc3\xa9",
  "<Track ratingKey=\"1\" title=\"t\">", "<station name=\"n\" stationuuid=\"u\">"
};

static void fill_text(uint8_t *p, size_t n, char c)
{
  memset(p, c, n);
}

/* Generated shapes the mutator rarely reaches. */
static void xml_generated(void)
{
  uint32_t r;

  /* Deep nesting. */
  work_len = 0U;
  while (work_len + 3U <= 60000U) {
    work_insert(work_len, (const uint8_t *)"<a>", 3U);
  }
  xml_run(work, work_len, 0U);

  /* Huge attributes, many attributes, overlong names. */
  for (r = 0U; r < 200U; r++) {
    uint32_t attrs = 1U + below(120U);
    uint32_t a;

    work_len = 0U;
    work_insert(0U, (const uint8_t *)"<e", 2U);
    for (a = 0U; a < attrs && work_len < WORK_MAX - 7000U; a++) {
      size_t name_len = 1U + below(a % 7U == 0U ? 300U : 8U);
      size_t value_len = below(a % 5U == 0U ? 6000U : 40U);
      uint8_t quote = below(2U) ? '"' : '\'';

      work[work_len++] = ' ';
      fill_text(work + work_len, name_len, (char)('a' + (char)(a % 26U)));
      work_len += name_len;
      work[work_len++] = '=';
      work[work_len++] = quote;
      fill_text(work + work_len, value_len, below(3U) == 0U ? '&' : 'v');
      work_len += value_len;
      work[work_len++] = quote;
    }
    work_insert(work_len, (const uint8_t *)"/>", 2U);
    xml_run(work, work_len, 0U);
  }

  /* Truncated documents: every prefix of a seed. */
  for (r = 0U; r < COUNT(xml_seeds); r++) {
    size_t len = strlen(xml_seeds[r]);
    size_t cut;

    for (cut = 0U; cut <= len; cut++) {
      work_set(xml_seeds[r], cut);
      xml_run(work, work_len, 0U);
    }
  }
}

/* An attribute name that ends exactly where the text buffer is full used
 * to make the scanner write its value's terminator one byte past the
 * buffer. Four attributes of 2 + 1 + 1019 + 1 bytes bring the text to
 * 4092 bytes, then a three-letter name takes the last four. */
static void xml_regression_value_at_text_end(void)
{
  uint32_t a;
  size_t i;
  int saw = 0;

  work_len = 0U;
  work_insert(0U, (const uint8_t *)"<e", 2U);
  for (a = 0U; a < 4U; a++) {
    work[work_len++] = ' ';
    work[work_len++] = 'a';
    work[work_len++] = (uint8_t)('0' + a);
    work[work_len++] = '=';
    work[work_len++] = '"';
    for (i = 0U; i < 1019U; i++) {
      work[work_len++] = 'v';
    }
    work[work_len++] = '"';
  }
  work_insert(work_len, (const uint8_t *)" xyz=\"v\"/>", 10U);
  CHECK(4U * (2U + 1U + 1019U + 1U) == ZZPLAY_XML_TEXT_MAX - 4U);
  {
    /* One feed, so the overrun would hit the heap block's end. */
    ZZPlayXml *xml = (ZZPlayXml *)malloc(sizeof(ZZPlayXml));
    ZZPlayXmlHandler h;
    XmlCheck c;

    memset(&c, 0, sizeof(c));
    c.xml = xml;
    h.start = xml_start;
    h.end = NULL;
    h.user = &c;
    zzplay_xml_init(xml, &h);
    (void)zzplay_xml_feed(xml, (const char *)work, work_len);
    saw = c.starts == 1U;
    free(xml);
  }
  CHECK(saw);
}

static void fuzz_xml(void)
{
  uint32_t r;

  xml_regression_value_at_text_end();
  xml_generated();
  for (r = 0U; r < ROUNDS; r++) {
    next_input(xml_seeds, COUNT(xml_seeds), xml_dict, COUNT(xml_dict), 2000U);
    xml_run(work, work_len, below(10U) == 0U ? 1U + below(3U) : 0U);
  }
}

/* --- M3U playlists --- */

static const char *const m3u_seeds[] = {
  "#EXTM3U\n#EXTINF:123,Artist - Title\nmusic/a.mp3\n#EXTINF:-1,Radio\n"
  "http://s/x\n",
  "\xef\xbb\xbf#EXTINF:-1,Radio\r\nhttp://x/y\r\nWork:abs.flac\r\n",
  "a\rb\r\nc\n  #comment\n\t d.mp3 \t\n",
  "#EXTINF:99999999999,Long\nx.mp3\n#EXTINF:,\n#EXTINF:5\ny.mp3"
};

static const char *const m3u_dict[] = {
  "#EXTINF:", ",", "\n", "\r", "\r\n", "-1", "4294967296", ":", "/",
  "http://h/", "#EXTM3U", "\x01", "\t", "\xef\xbb\xbf"
};

static void check_list(const ZZPlayPlaylist *list)
{
  uint32_t i;

  CHECK(list->count <= ZZPLAY_PLAYLIST_MAX_ENTRIES);
  for (i = 0U; i < list->count; i++) {
    CHECK(strlen(list->entries[i].path) < ZZPLAY_PLAYLIST_PATH_MAX);
    CHECK(memchr(list->entries[i].title, 0, ZZPLAY_PLAYLIST_TITLE_MAX) != NULL);
    CHECK(!has_control(list->entries[i].title));
  }
}

/* A line longer than a playlist path used to be cut and added as a
 * different file. */
static void m3u_regression_long_line(void)
{
  ZZPlayPlaylist list;
  size_t i;

  work_len = 0U;
  for (i = 0U; i < ZZPLAY_PLAYLIST_PATH_MAX + 20U; i++) {
    work[work_len++] = 'a';
  }
  work_insert(work_len, (const uint8_t *)"\nok.mp3\n", 8U);
  zzplay_playlist_init(&list);
  CHECK(zzplay_playlist_parse_m3u(&list, (const char *)work, work_len,
                                  "Work:") == 1U);
  CHECK(list.count == 1U && strcmp(list.entries[0].path, "Work:ok.mp3") == 0);
  zzplay_playlist_free(&list);
}

/* Titles with line breaks used to be saved verbatim, so the next load
 * read the rest of the title as a file. */
static void m3u_regression_title_break(void)
{
  ZZPlayPlaylist list;
  ZZPlayPlaylist again;
  char *text;
  size_t need;

  zzplay_playlist_init(&list);
  zzplay_playlist_init(&again);
  CHECK(zzplay_playlist_add(&list, "Work:a.mp3") == 0);
  CHECK(zzplay_playlist_set_title(&list, 0U, "Line one\nWork:evil.mp3"));
  need = zzplay_playlist_format_m3u(&list, NULL, 0U);
  text = (char *)malloc(need + 1U);
  (void)zzplay_playlist_format_m3u(&list, text, need + 1U);
  CHECK(zzplay_playlist_parse_m3u(&again, text, need, "") == 1U);
  CHECK(again.count == 1U && strcmp(again.entries[0].path, "Work:a.mp3") == 0);
  free(text);
  zzplay_playlist_free(&list);
  zzplay_playlist_free(&again);
}

static void fuzz_m3u(void)
{
  uint32_t r;

  m3u_regression_long_line();
  m3u_regression_title_break();
  for (r = 0U; r < ROUNDS; r++) {
    ZZPlayPlaylist list;
    ZZPlayPlaylist again;
    const char *drawer;
    char *in;

    next_input(m3u_seeds, COUNT(m3u_seeds), m3u_dict, COUNT(m3u_dict), 1500U);
    in = input_copy(0);
    zzplay_playlist_init(&list);
    zzplay_playlist_init(&again);
    drawer = below(2U) ? "Work:Music" : "";
    (void)zzplay_playlist_parse_m3u(&list, in, work_len, drawer);
    check_list(&list);
    /* Saving and loading again keeps the entries. */
    {
      size_t need = zzplay_playlist_format_m3u(&list, NULL, 0U);
      char *text = (char *)malloc(need + 1U);

      CHECK(zzplay_playlist_format_m3u(&list, text, need + 1U) == need);
      CHECK(strlen(text) == need);
      (void)zzplay_playlist_parse_m3u(&again, text, need, drawer);
      CHECK(again.count == list.count);
      free(text);
    }
    zzplay_playlist_free(&list);
    zzplay_playlist_free(&again);
    free(in);
  }
}

/* --- Stream playlists (PLS and M3U from a URL) --- */

static const char *const pls_seeds[] = {
  "[playlist]\nFile1=http://a/b\nTitle1=x\nFile2=rel.mp3\n"
  "NumberOfEntries=2\nVersion=2\n",
  "#EXTM3U\nhttp://s/x\n/abs\nrel\nWork:no\nftp://no\n",
  "\xef\xbb\xbf  [PLAYLIST]\r\nfile8=//h/x\r\nFile99=http://z/\r\nFile0=x\r\n"
};

static const char *const pls_dict[] = {
  "File", "=", "1", "8", "9", "\n", "\r\n", "http://", "https://", "//", "/",
  ":", "[playlist]", " ", "\t"
};

static void fuzz_streamlist(void)
{
  static ZZPlayStreamList out;
  static const char *const bases[] = {
    "http://radio.example/list.pls", "https://h:8443/a/b.m3u?x", "bogus",
    NULL
  };
  uint32_t r;

  for (r = 0U; r < ROUNDS; r++) {
    uint32_t n;
    uint32_t i;
    char *in;

    next_input(pls_seeds, COUNT(pls_seeds), pls_dict, COUNT(pls_dict), 1500U);
    in = input_copy(0);
    n = zzplay_streamlist_parse(in, work_len, bases[below(COUNT(bases))],
                                &out);
    CHECK(n == out.count && n <= ZZPLAY_STREAMLIST_MAX);
    for (i = 0U; i < out.count; i++) {
      CHECK(memchr(out.url[i], 0, sizeof(out.url[i])) != NULL);
      CHECK(zzplay_http_is_url(out.url[i]));
    }
    free(in);
  }
  {
    /* A huge playlist still keeps to its limit. */
    static char big[ZZPLAY_STREAMLIST_TEXT_MAX];
    size_t len = 0U;

    while (len + 16U < sizeof(big)) {
      memcpy(big + len, "http://h/xxxxx\n", 15U);
      len += 15U;
    }
    CHECK(zzplay_streamlist_parse(big, len, NULL, &out) ==
          ZZPLAY_STREAMLIST_MAX);
  }
}

int main(void)
{
  fuzz_head();
  fuzz_url();
  fuzz_chunked();
  fuzz_icy();
  fuzz_xml();
  fuzz_m3u();
  fuzz_streamlist();
  if (failures != 0) {
    printf("zzplay_parser_fuzz_test: %d checks failed\n", failures);
    return 1;
  }
  printf("zzplay_parser_fuzz_test: all checks passed\n");
  return 0;
}
