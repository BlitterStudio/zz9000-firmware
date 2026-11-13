/* A push scanner for the XML that ZZPlay's service modules read (Plex,
 * radio-browser.info): elements and their attributes, fed in chunks of any
 * size, with bounded memory. Text content, comments, CDATA, processing
 * instructions and declarations are skipped; those services carry their
 * data in attributes.
 *
 * Host-tested. SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_XML_H
#define ZZPLAY_XML_H

#include <stddef.h>
#include <stdint.h>

#define ZZPLAY_XML_NAME_MAX 48U
#define ZZPLAY_XML_ATTRS_MAX 48U
/* Attribute names and values of one element. */
#define ZZPLAY_XML_TEXT_MAX 4096U
/* Longest single attribute value kept; the rest of a longer one (a Plex
 * album summary, say) is dropped so the attributes after it still fit. */
#define ZZPLAY_XML_VALUE_MAX 1024U

typedef struct ZZPlayXmlAttr {
  const char *name;
  const char *value;
} ZZPlayXmlAttr;

typedef struct ZZPlayXmlHandler {
  /* An element opened. `truncated` is set when a value was shortened or
   * attributes were dropped to stay within the limits above. Return
   * nonzero to stop scanning. */
  int (*start)(void *user, const char *name, const ZZPlayXmlAttr *attrs,
               uint32_t count, int truncated);
  /* An element closed; self-closing elements get start then end. May be
   * NULL. */
  int (*end)(void *user, const char *name);
  void *user;
} ZZPlayXmlHandler;

typedef struct ZZPlayXml {
  ZZPlayXmlHandler handler;
  int state;
  int stopped;
  char quote;
  char last[2];                       /* comment/CDATA terminator check */
  char bang[8];                       /* text after "<!" */
  uint32_t bang_len;
  int attr_stored;                    /* the attribute being read is kept */
  char name[ZZPLAY_XML_NAME_MAX];
  uint32_t name_len;
  int truncated;
  uint32_t count;
  uint32_t used;                      /* bytes of text in use */
  uint32_t value_start;
  uint32_t name_off[ZZPLAY_XML_ATTRS_MAX];
  uint32_t value_off[ZZPLAY_XML_ATTRS_MAX];
  uint32_t depth;                     /* elements open */
  int seen_element;
  char text[ZZPLAY_XML_TEXT_MAX];
} ZZPlayXml;

void zzplay_xml_init(ZZPlayXml *xml, const ZZPlayXmlHandler *handler);

/* Scan the next chunk. Returns 1 once a handler asked to stop (further
 * chunks are ignored), else 0. */
int zzplay_xml_feed(ZZPlayXml *xml, const char *data, size_t len);

/* After the last chunk: 1 when the document was complete (at least one
 * element, every element closed, and the text did not end inside markup)
 * or a handler stopped the scan; 0 for a document that was cut short. */
int zzplay_xml_finish(const ZZPlayXml *xml);

/* The value of attribute `name` in a start callback's list, or NULL. */
const char *zzplay_xml_attr(const ZZPlayXmlAttr *attrs, uint32_t count,
                            const char *name);

#endif /* ZZPLAY_XML_H */
