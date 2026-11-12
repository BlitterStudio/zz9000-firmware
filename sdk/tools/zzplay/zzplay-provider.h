/* The interface between ZZPlay and its service modules (Plex, internet
 * radio, ...).
 *
 * A module is an AmigaOS hunk file in ZZPlay's Providers drawer whose
 * first code is an entry function returning its ZZPlayProvider. ZZPlay
 * checks the descriptor before calling anything, then hands the module a
 * ZZPlayHost: the only services a module uses. A module never opens a
 * library, socket or file itself, so it needs no startup code and no
 * library bases.
 *
 * This is a public interface that outlives any one ZZPlay release:
 * structures only grow at the end, each carries its size, and a new
 * capability bumps ZZPLAY_PROVIDER_ABI only when an existing field
 * changes meaning.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef ZZPLAY_PROVIDER_H
#define ZZPLAY_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#define ZZPLAY_PROVIDER_ID 0x5A5A5056UL /* "ZZPV" */
#define ZZPLAY_PROVIDER_ABI 1UL

#define ZZPLAY_PROVIDER_URL_MAX 1168U
#define ZZPLAY_PROVIDER_TEXT_MAX 96U
#define ZZPLAY_PROVIDER_ERROR_MAX 128U

/* A non-playable row with this id asks ZZPlay to open the service's
 * Settings window instead of trying to browse or play it. */
#define ZZPLAY_PROVIDER_SETTINGS_ID "\037settings"

/* Device-link sign-in results. link_poll may block while it polls because
 * ZZPlay always calls it in the Browse worker process. */
#define ZZPLAY_LINK_DONE 1U
#define ZZPLAY_LINK_CHOOSE 2U
#define ZZPLAY_LINK_CHOICES_MAX 8U

typedef struct ZZPlayProviderLinkStart {
  char url[ZZPLAY_PROVIDER_TEXT_MAX];
  char code[ZZPLAY_PROVIDER_TEXT_MAX];
  char link_id[ZZPLAY_PROVIDER_TEXT_MAX];
} ZZPlayProviderLinkStart;

typedef struct ZZPlayProviderLinkChoice {
  char id[ZZPLAY_PROVIDER_TEXT_MAX];
  char label[ZZPLAY_PROVIDER_TEXT_MAX];
} ZZPlayProviderLinkChoice;

typedef struct ZZPlayProviderLinkResult {
  uint32_t state; /* ZZPLAY_LINK_* */
  uint32_t choice_count;
  ZZPlayProviderLinkChoice choices[ZZPLAY_LINK_CHOICES_MAX];
  /* Set on DONE to the human-readable account/server selected. */
  char signed_in_name[ZZPLAY_PROVIDER_TEXT_MAX];
} ZZPlayProviderLinkResult;


/* What a browse row is. */
#define ZZPLAY_ITEM_FOLDER 0U  /* opens another list */
#define ZZPLAY_ITEM_TRACK 1U   /* a finite item */
#define ZZPLAY_ITEM_STATION 2U /* a live stream */
/* Opens a provider-owned form. Form rows are never playable. */
#define ZZPLAY_ITEM_FORM 3U

typedef struct ZZPlayProviderItem {
  /* Module-defined, at most ZZPLAY_PROVIDER_TEXT_MAX - 1 bytes. A folder's
   * id is passed back to list(); a playable item's id is stored in the
   * playlist as "<scheme>://<id>" and passed back to resolve(). */
  const char *id;
  const char *title;
  const char *subtitle;      /* artist and album, codec and bitrate, or "" */
  uint32_t duration_ms;      /* 0 when unknown */
  uint32_t kind;             /* ZZPLAY_ITEM_* */
  uint32_t playable;         /* 0 for an item shown but not playable. On a
                               * FOLDER, 1 marks a collection ZZPlay may
                               * play whole by listing it: the folder's
                               * rows, then the rows of each playable
                               * folder inside it, give its playable
                               * tracks and stations. */
  uint32_t favourite;        /* 1 when it is in the service's favourites */
  uint32_t editable;         /* 1 when the Browse Edit menu may open a form */
} ZZPlayProviderItem;

/* Receives list and search results. ZZPlay copies every string before
 * item() returns. Return nonzero from item() to stop the listing. */
typedef struct ZZPlayItemSink {
  int (*item)(void *user, const ZZPlayProviderItem *item);
  /* The number of items the whole list holds, when the service says. */
  void (*total)(void *user, uint32_t total);
  void *user;
} ZZPlayItemSink;

/* A playable item turned into something ZZPlay can open. */
typedef struct ZZPlayResolved {
  char url[ZZPLAY_PROVIDER_URL_MAX];   /* http:// or https:// */
  char title[ZZPLAY_PROVIDER_TEXT_MAX];
  uint32_t duration_ms;
} ZZPlayResolved;

/* A setting the module needs from the user, shown in the Browse window's
 * settings form and stored in ENVARC:ZZPlay/<scheme>.prefs. */
#define ZZPLAY_FIELD_TEXT 0U
#define ZZPLAY_FIELD_SECRET 1U /* shown as dots */
#define ZZPLAY_FIELD_NUMBER 2U
#define ZZPLAY_FIELD_CHOICE 3U /* pick one of choice_labels */

typedef struct ZZPlayProviderField {
  const char *key;      /* prefs key: letters, digits and '_' */
  const char *label;
  uint32_t kind;        /* ZZPLAY_FIELD_* */
  uint32_t max_len;     /* longest value, excluding the terminator */
  /* ZZPLAY_FIELD_CHOICE only: parallel NULL-terminated arrays; the first
   * choice is the default when the pref is empty or unknown. NULL for
   * the other kinds. */
  const char *const *choice_values; /* stored in prefs */
  const char *const *choice_labels; /* shown */
} ZZPlayProviderField;

#define ZZPLAY_PROVIDER_FORM_FIELDS_MAX 8U

/* A row form. fields and values belong to the provider and remain valid until
 * the call returns. values has field_count entries, one per field. */
typedef struct ZZPlayProviderForm {
  const char *title;
  const ZZPlayProviderField *fields;
  const char *const *values;
  uint32_t field_count;
  uint32_t deletable;
} ZZPlayProviderForm;

/* XML element callback; see zzplay-xml.h for the scanner's behaviour. */
typedef struct ZZPlayHostXmlAttr {
  const char *name;
  const char *value;
} ZZPlayHostXmlAttr;

typedef struct ZZPlayHostXmlHandler {
  int (*start)(void *user, const char *name, const ZZPlayHostXmlAttr *attrs,
               uint32_t count, int truncated);
  int (*end)(void *user, const char *name);
  void *user;
} ZZPlayHostXmlHandler;

/* Services ZZPlay offers a module. Every call takes the host table it
 * came from (ZZPlay binds a table to one module and one process) and runs
 * in the process that called the module. */
typedef struct ZZPlayHost ZZPlayHost;
struct ZZPlayHost {
  uint32_t abi;               /* ZZPLAY_PROVIDER_ABI */
  uint32_t size;              /* sizeof(ZZPlayHost) of the running ZZPlay */

  void *(*alloc)(const ZZPlayHost *host, uint32_t size); /* cleared, or NULL */
  void (*free)(const ZZPlayHost *host, void *ptr);

  /* GET `url`, sending `headers` (CRLF-terminated lines, or NULL) only to
   * that URL's origin. Body bytes go to data(), which returns nonzero to
   * stop early. Returns 1 when the body was delivered or data() stopped;
   * 0 with a reason in `err` otherwise. `*status` is the final HTTP
   * status, 0 when none arrived. The user's Stop cancels the transfer. */
  int (*http_get)(const ZZPlayHost *host, const char *url,
                  const char *headers,
                  int (*data)(void *user, const uint8_t *bytes,
                              uint32_t len),
                  void *user, int *status, char *err, uint32_t err_size);

  /* A streaming XML scanner over the body bytes. */
  void *(*xml_open)(const ZZPlayHost *host,
                    const ZZPlayHostXmlHandler *handler);
  /* Returns 1 once a handler asked to stop. */
  int (*xml_feed)(const ZZPlayHost *host, void *xml, const char *data,
                  uint32_t len);
  void (*xml_close)(const ZZPlayHost *host, void *xml);
  const char *(*xml_attr)(const ZZPlayHostXmlAttr *attrs, uint32_t count,
                          const char *name);

  /* Percent-encode `in` for a URL query; returns the length written, or
   * 0 when it does not fit. */
  uint32_t (*url_escape)(const char *in, char *out, uint32_t size);
  /* snprintf. */
  int (*format)(char *out, uint32_t size, const char *fmt, ...);

  /* This module's settings: get returns 0 and "" for a missing key; set
   * stores for this session and saves to ENVARC:. */
  int (*pref_get)(const ZZPlayHost *host, const char *key, char *out,
                  uint32_t size);
  int (*pref_set)(const ZZPlayHost *host, const char *key,
                  const char *value);

  /* POST `url` with an optional `body`. Same semantics as http_get. */
  int (*http_post)(const ZZPlayHost *host, const char *url,
                   const char *headers, const uint8_t *body,
                   uint32_t body_len,
                   int (*data)(void *user, const uint8_t *bytes,
                               uint32_t len),
                   void *user, int *status, char *err, uint32_t err_size);

  /* Sleep for up to `ms` milliseconds, polling keep_running in steps of at
   * most 100 ms. Returns 0 if cancelled, 1 if completed. */
  int (*sleep)(const ZZPlayHost *host, uint32_t ms);

  /* After the body was fed: 1 when the XML document was complete (every
   * element closed, nothing cut inside markup) or a handler stopped the
   * scan, 0 for a reply cut short. Added after sleep: check `size` before
   * calling. */
  int (*xml_finish)(const ZZPlayHost *host, void *xml);
};

typedef struct ZZPlayProvider {
  uint32_t id;                /* ZZPLAY_PROVIDER_ID */
  uint32_t abi;               /* ZZPLAY_PROVIDER_ABI the module was built for */
  uint32_t size;              /* sizeof(ZZPlayProvider) the module was built with */
  const char *scheme;         /* URI scheme and prefs name: "radio", "plex" */
  const char *name;           /* shown in the service list */
  const ZZPlayProviderField *fields;
  uint32_t field_count;

  /* Start a session with the current settings. Returns the module's
   * context, or NULL with the reason in `err`. */
  void *(*open)(const ZZPlayHost *host, char *err, uint32_t err_size);
  void (*close)(void *ctx);
  /* List folder `node` ("" for the service's top level): at most `count`
   * items starting at `start`. Returns 1, or 0 with the reason. */
  int (*list)(void *ctx, const char *node, uint32_t start, uint32_t count,
              const ZZPlayItemSink *sink, char *err, uint32_t err_size);
  /* Search the service. Returns 1, or 0 with the reason. */
  int (*search)(void *ctx, const char *query, const ZZPlayItemSink *sink,
                char *err, uint32_t err_size);
  /* Turn a playable item's id into an address. Returns 1, or 0 with the
   * reason (an unsupported codec, say). */
  int (*resolve)(void *ctx, const char *item, ZZPlayResolved *out,
                 char *err, uint32_t err_size);
  /* Add (`add` 1) or remove (`add` 0) a playable item, as the module
   * listed it, from the service's favourites. NULL when the service keeps
   * none. Returns 1, or 0 with the reason. */
  int (*favourite)(void *ctx, const ZZPlayProviderItem *item, int add,
                   char *err, uint32_t err_size);
  /* Optional device-link sign-in. These fields were appended after the
   * original ABI; callers must check size before reading them. link_name
   * is the short account name used in the window title ("Plex");
   * link_host is the site shown in prompts ("plex.tv"). link_start obtains
   * the URL, code and opaque id. link_poll waits for approval and either
   * saves the only choice (DONE) or returns choices (CHOOSE). link_choose
   * saves one returned choice. */
  const char *link_name;
  const char *link_host;
  int (*link_start)(void *ctx, ZZPlayProviderLinkStart *out,
                    char *err, uint32_t err_size);
  int (*link_poll)(void *ctx, const char *link_id,
                   ZZPlayProviderLinkResult *out,
                   char *err, uint32_t err_size);
  int (*link_choose)(void *ctx, const char *choice_id,
                     ZZPlayProviderLinkResult *out,
                     char *err, uint32_t err_size);
  /* Optional row forms. These fields are appended, so hosts check size
   * before reading them. item_id identifies either a form row or an
   * editable row. submit receives field_count values in field order. */
  int (*form_get)(void *ctx, const char *item_id, ZZPlayProviderForm *out,
                  char *err, uint32_t err_size);
  int (*form_submit)(void *ctx, const char *item_id,
                     const char *const *values, uint32_t value_count,
                     char *err, uint32_t err_size);
  int (*form_delete)(void *ctx, const char *item_id,
                     char *err, uint32_t err_size);
  /* Optional Browse menu label for editable rows. Appended after forms, so
   * hosts must check size before reading it. */
  const char *edit_label;
} ZZPlayProvider;


/* The first code of a module's hunk file. */
typedef const ZZPlayProvider *(*ZZPlayProviderEntry)(void);

/* Each module defines this; providers/zzprov_entry.S, the first code of
 * the hunk file, calls it. */
const ZZPlayProvider *zzplay_provider_entry(void);

#endif /* ZZPLAY_PROVIDER_H */
