/* A push scanner for service-module XML. SPDX-License-Identifier:
 * GPL-3.0-or-later */

#include "zzplay-xml.h"

#include <string.h>

enum {
  X_TEXT,
  X_TAG,          /* after '<' */
  X_BANG,         /* after "<!" */
  X_COMMENT,
  X_CDATA,
  X_SKIP,         /* <?...> and <!DOCTYPE...> */
  X_NAME,
  X_ATTRS,
  X_ANAME,
  X_AEQ_WAIT,     /* attribute name ended, '=' expected */
  X_AEQ,          /* after '=', quote expected */
  X_AVAL,
  X_SELF,         /* after '/' inside a start tag */
  X_END_NAME,
  X_END_REST
};

static int is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
         c == ':' || (unsigned char)c >= 0x80U;
}

void zzplay_xml_init(ZZPlayXml *xml, const ZZPlayXmlHandler *handler)
{
  memset(xml, 0, sizeof(*xml));
  xml->handler = *handler;
  xml->state = X_TEXT;
}

static void begin_element(ZZPlayXml *xml)
{
  xml->name_len = 0U;
  xml->name[0] = '\0';
  xml->count = 0U;
  xml->used = 0U;
  xml->truncated = 0;
}

static void name_push(ZZPlayXml *xml, char c)
{
  if (xml->name_len + 1U < sizeof(xml->name)) {
    xml->name[xml->name_len++] = c;
    xml->name[xml->name_len] = '\0';
  } else {
    xml->truncated = 1;
  }
}

/* Room for one more byte of text plus the terminator it will need. */
static int text_push(ZZPlayXml *xml, char c)
{
  if (xml->used + 2U > sizeof(xml->text)) {
    return 0;
  }
  xml->text[xml->used++] = c;
  return 1;
}

/* Start a new attribute; returns 0 when it cannot be stored. */
static int attr_begin(ZZPlayXml *xml)
{
  if (xml->count >= ZZPLAY_XML_ATTRS_MAX || xml->used + 4U > sizeof(xml->text)) {
    xml->truncated = 1;
    return 0;
  }
  xml->name_off[xml->count] = xml->used;
  xml->value_off[xml->count] = 0xFFFFFFFFUL;
  return 1;
}

static void put_utf8(char **out, uint32_t cp)
{
  char *o = *out;

  if (cp < 0x80U) {
    *o++ = (char)cp;
  } else if (cp < 0x800U) {
    *o++ = (char)(0xC0U | (cp >> 6));
    *o++ = (char)(0x80U | (cp & 0x3FU));
  } else if (cp < 0x10000U) {
    *o++ = (char)(0xE0U | (cp >> 12));
    *o++ = (char)(0x80U | ((cp >> 6) & 0x3FU));
    *o++ = (char)(0x80U | (cp & 0x3FU));
  } else {
    *o++ = (char)(0xF0U | (cp >> 18));
    *o++ = (char)(0x80U | ((cp >> 12) & 0x3FU));
    *o++ = (char)(0x80U | ((cp >> 6) & 0x3FU));
    *o++ = (char)(0x80U | (cp & 0x3FU));
  }
  *out = o;
}

/* Decode entities of the NUL-terminated value in place; the result is
 * never longer than the input. Unknown entities stay as written. */
static void decode_entities(char *s)
{
  static const struct { const char *name; char c; } named[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
    { "quot;", '"' }, { "apos;", '\'' }
  };
  char *in = s;
  char *out = s;

  while (*in != '\0') {
    int done = 0;

    if (*in == '&') {
      size_t i;

      for (i = 0U; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t n = strlen(named[i].name);

        if (strncmp(in + 1, named[i].name, n) == 0) {
          *out++ = named[i].c;
          in += 1U + n;
          done = 1;
          break;
        }
      }
      if (!done && in[1] == '#') {
        char *p = in + 2;
        uint32_t cp = 0U;
        int hex = 0;
        int digits = 0;

        if (*p == 'x' || *p == 'X') {
          hex = 1;
          p++;
        }
        for (; digits < 8; p++, digits++) {
          char c = *p;

          if (c >= '0' && c <= '9') {
            cp = cp * (hex ? 16U : 10U) + (uint32_t)(c - '0');
          } else if (hex && c >= 'a' && c <= 'f') {
            cp = cp * 16U + (uint32_t)(c - 'a' + 10);
          } else if (hex && c >= 'A' && c <= 'F') {
            cp = cp * 16U + (uint32_t)(c - 'A' + 10);
          } else {
            break;
          }
        }
        /* "&#1;" is four bytes and its UTF-8 at most one; "&#x10FFFF;"
         * is ten and its UTF-8 four, so the output never overtakes the
         * input. */
        if (digits != 0 && *p == ';' && cp != 0U && cp <= 0x10FFFFUL) {
          put_utf8(&out, cp);
          in = p + 1;
          done = 1;
        }
      }
    }
    if (!done) {
      *out++ = *in++;
    }
  }
  *out = '\0';
}

static int emit_start(ZZPlayXml *xml)
{
  ZZPlayXmlAttr attrs[ZZPLAY_XML_ATTRS_MAX];
  uint32_t i;
  uint32_t n = 0U;

  for (i = 0U; i < xml->count; i++) {
    if (xml->value_off[i] == 0xFFFFFFFFUL) {
      continue; /* a name without a value */
    }
    attrs[n].name = xml->text + xml->name_off[i];
    attrs[n].value = xml->text + xml->value_off[i];
    n++;
  }
  if (xml->handler.start != NULL &&
      xml->handler.start(xml->handler.user, xml->name, attrs, n,
                         xml->truncated) != 0) {
    xml->stopped = 1;
  }
  return xml->stopped;
}

static int emit_end(ZZPlayXml *xml)
{
  if (xml->handler.end != NULL &&
      xml->handler.end(xml->handler.user, xml->name) != 0) {
    xml->stopped = 1;
  }
  return xml->stopped;
}

/* Close the attribute being collected: terminate its name or value. */
static void attr_name_end(ZZPlayXml *xml, int stored)
{
  if (stored) {
    xml->text[xml->used++] = '\0';
  }
}

int zzplay_xml_feed(ZZPlayXml *xml, const char *data, size_t len)
{
  size_t i;
  /* Whether the attribute being read is stored (room was left for it). */
  int stored;

  if (xml->stopped) {
    return 1;
  }
  stored = xml->attr_stored;
  for (i = 0U; i < len && !xml->stopped; i++) {
    char c = data[i];

    switch (xml->state) {
    case X_TEXT:
      if (c == '<') {
        xml->state = X_TAG;
      }
      break;
    case X_TAG:
      if (c == '/') {
        begin_element(xml);
        xml->state = X_END_NAME;
      } else if (c == '!') {
        xml->bang_len = 0U;
        xml->state = X_BANG;
      } else if (c == '?') {
        xml->state = X_SKIP;
      } else if (is_name_char(c)) {
        begin_element(xml);
        name_push(xml, c);
        xml->state = X_NAME;
      } else {
        xml->state = X_TEXT; /* a stray '<' in text */
      }
      break;
    case X_BANG:
      if (xml->bang_len < sizeof(xml->bang)) {
        xml->bang[xml->bang_len++] = c;
      }
      xml->last[0] = '\0';
      xml->last[1] = '\0';
      if (xml->bang_len == 2U && memcmp(xml->bang, "--", 2U) == 0) {
        xml->state = X_COMMENT;
      } else if (xml->bang_len == 7U && memcmp(xml->bang, "[CDATA[", 7U) == 0) {
        xml->state = X_CDATA;
      } else if (c == '>') {
        xml->state = X_TEXT;
      } else if (xml->bang_len == sizeof(xml->bang)) {
        xml->state = X_SKIP;
      }
      break;
    case X_COMMENT:
    case X_CDATA:
      if (c == '>' &&
          ((xml->state == X_COMMENT && xml->last[0] == '-' &&
            xml->last[1] == '-') ||
           (xml->state == X_CDATA && xml->last[0] == ']' &&
            xml->last[1] == ']'))) {
        xml->state = X_TEXT;
      }
      xml->last[0] = xml->last[1];
      xml->last[1] = c;
      break;
    case X_SKIP:
      if (c == '>') {
        xml->state = X_TEXT;
      }
      break;
    case X_NAME:
      if (is_name_char(c)) {
        name_push(xml, c);
      } else if (c == '>') {
        xml->state = X_TEXT;
        (void)emit_start(xml);
      } else if (c == '/') {
        xml->state = X_SELF;
      } else {
        xml->state = X_ATTRS;
      }
      break;
    case X_ATTRS:
      if (is_space(c)) {
        break;
      }
      if (c == '>') {
        xml->state = X_TEXT;
        (void)emit_start(xml);
      } else if (c == '/') {
        xml->state = X_SELF;
      } else if (is_name_char(c)) {
        stored = attr_begin(xml);
        if (stored) {
          (void)text_push(xml, c);
        }
        xml->state = X_ANAME;
      }
      break;
    case X_ANAME:
      if (is_name_char(c)) {
        if (stored && !text_push(xml, c)) {
          stored = 0;
          xml->truncated = 1;
        }
      } else {
        attr_name_end(xml, stored);
        if (c == '=') {
          xml->state = X_AEQ;
        } else if (is_space(c)) {
          xml->state = X_AEQ_WAIT;
        } else {
          /* A name without '=': keep scanning the tag. */
          if (stored) {
            xml->count++;
          }
          xml->state = X_ATTRS;
          i--;
        }
      }
      break;
    case X_AEQ_WAIT:
      if (c == '=') {
        xml->state = X_AEQ;
      } else if (!is_space(c)) {
        if (stored) {
          xml->count++;
        }
        xml->state = X_ATTRS;
        i--;
      }
      break;
    case X_AEQ:
      if (c == '"' || c == '\'') {
        xml->quote = c;
        if (stored && xml->used >= sizeof(xml->text)) {
          /* The name filled the buffer: no room for the value's
           * terminator, so the attribute is dropped. */
          stored = 0;
          xml->truncated = 1;
        }
        if (stored) {
          xml->value_start = xml->used;
          xml->value_off[xml->count] = xml->used;
        }
        xml->state = X_AVAL;
      } else if (!is_space(c)) {
        if (stored) {
          xml->count++;
        }
        xml->state = X_ATTRS;
        i--;
      }
      break;
    case X_AVAL:
      if (c == xml->quote) {
        if (stored) {
          xml->text[xml->used++] = '\0';
          decode_entities(xml->text + xml->value_start);
          xml->used = xml->value_start +
                      (uint32_t)strlen(xml->text + xml->value_start) + 1U;
          xml->count++;
        }
        xml->state = X_ATTRS;
      } else if (stored) {
        if (xml->used - xml->value_start >= ZZPLAY_XML_VALUE_MAX ||
            !text_push(xml, c)) {
          xml->truncated = 1;
        }
      }
      break;
    case X_SELF:
      if (c == '>') {
        xml->state = X_TEXT;
        if (!emit_start(xml)) {
          (void)emit_end(xml);
        }
      } else {
        xml->state = X_ATTRS;
        i--;
      }
      break;
    case X_END_NAME:
      if (is_name_char(c)) {
        name_push(xml, c);
      } else if (c == '>') {
        xml->state = X_TEXT;
        (void)emit_end(xml);
      } else {
        xml->state = X_END_REST;
      }
      break;
    case X_END_REST:
      if (c == '>') {
        xml->state = X_TEXT;
        (void)emit_end(xml);
      }
      break;
    default:
      xml->state = X_TEXT;
      break;
    }
  }
  xml->attr_stored = stored;
  return xml->stopped;
}

const char *zzplay_xml_attr(const ZZPlayXmlAttr *attrs, uint32_t count,
                            const char *name)
{
  uint32_t i;

  for (i = 0U; i < count; i++) {
    if (strcmp(attrs[i].name, name) == 0) {
      return attrs[i].value;
    }
  }
  return NULL;
}
