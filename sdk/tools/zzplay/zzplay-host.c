/* ZZPlay's side of the service-module host table implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zzplay-host.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zzplay-sys.h"
#include "zzplay-xml.h"

#if defined(__amigaos__) || defined(AMIGA)
#include <dos/dos.h>
#include <proto/dos.h>
#endif
#include "zzplay-net.h"

#define ZZPLAY_HOST_PREF_FILE_MAX 65536U

/* Compile-time verification that host and internal XML attribute structs match. */
typedef char assert_host_xml_attr_size[
    sizeof(ZZPlayHostXmlAttr) == sizeof(ZZPlayXmlAttr) ? 1 : -1];
typedef char assert_host_xml_attr_name[
    offsetof(ZZPlayHostXmlAttr, name) == offsetof(ZZPlayXmlAttr, name) ? 1 : -1];
typedef char assert_host_xml_attr_value[
    offsetof(ZZPlayHostXmlAttr, value) == offsetof(ZZPlayXmlAttr, value) ? 1 : -1];

uint32_t zzplay_host_url_escape(const char *in, char *out, uint32_t size)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t needed = 0U;
  const unsigned char *p;
  char *d;

  if (!in || !out || size == 0U) {
    return 0U;
  }

  for (p = (const unsigned char *)in; *p != '\0'; p++) {
    unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      needed++;
    } else {
      needed += 3U;
    }
  }

  if (needed >= (size_t)size) {
    return 0U;
  }

  d = out;
  for (p = (const unsigned char *)in; *p != '\0'; p++) {
    unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      *d++ = (char)c;
    } else {
      *d++ = '%';
      *d++ = hex[(c >> 4) & 0x0FU];
      *d++ = hex[c & 0x0FU];
    }
  }
  *d = '\0';

  return (uint32_t)needed;
}

static int is_valid_key_char(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static int is_valid_key(const char *key)
{
  if (!key || !*key) {
    return 0;
  }
  while (*key) {
    if (!is_valid_key_char(*key)) {
      return 0;
    }
    key++;
  }
  return 1;
}

int zzplay_host_pref_text_get(const char *text, const char *key, char *out,
                              uint32_t size)
{
  const char *p;
  size_t klen;

  if (out && size > 0U) {
    out[0] = '\0';
  }
  if (!text || !key || !is_valid_key(key) || !out || size == 0U) {
    return 0;
  }

  klen = strlen(key);
  p = text;

  while (*p) {
    const char *line_start = p;
    const char *line_end;
    const char *eq;
    const char *k_start;
    const char *k_end;
    size_t cur_klen;

    while (*p && *p != '\n') {
      p++;
    }
    line_end = p;
    if (*p == '\n') {
      p++;
    }

    k_start = line_start;
    while (k_start < line_end && (*k_start == ' ' || *k_start == '\t')) {
      k_start++;
    }

    if (k_start >= line_end || *k_start == '#' || *k_start == ';') {
      continue;
    }

    eq = memchr(k_start, '=', (size_t)(line_end - k_start));
    if (!eq) {
      continue;
    }

    k_end = eq;
    while (k_end > k_start && (*(k_end - 1) == ' ' || *(k_end - 1) == '\t')) {
      k_end--;
    }

    cur_klen = (size_t)(k_end - k_start);
    if (cur_klen == klen && memcmp(k_start, key, klen) == 0) {
      const char *val_start = eq + 1;
      const char *val_end = line_end;
      size_t vlen;

      if (val_end > val_start && *(val_end - 1) == '\r') {
        val_end--;
      }

      vlen = (size_t)(val_end - val_start);
      if (vlen >= (size_t)size) {
        out[0] = '\0';
        return 0;
      }

      memcpy(out, val_start, vlen);
      out[vlen] = '\0';
      return 1;
    }
  }

  return 0;
}

int zzplay_host_pref_text_set(const char *in_text, const char *key,
                              const char *value, char *out, uint32_t out_size)
{
  const char *p;
  size_t klen;
  size_t vlen;
  size_t out_used = 0U;
  int key_set = 0;

  if (!key || !is_valid_key(key) || !value || !out || out_size == 0U) {
    return 0;
  }

  klen = strlen(key);
  vlen = strlen(value);
  p = in_text ? in_text : "";

  while (*p) {
    const char *line_start = p;
    const char *line_end;
    const char *next_line;
    const char *k_start;
    const char *eq;
    int is_match = 0;

    while (*p && *p != '\n') {
      p++;
    }
    line_end = p;
    if (*p == '\n') {
      p++;
    }
    next_line = p;

    k_start = line_start;
    while (k_start < line_end && (*k_start == ' ' || *k_start == '\t')) {
      k_start++;
    }

    if (k_start < line_end && *k_start != '#' && *k_start != ';') {
      eq = memchr(k_start, '=', (size_t)(line_end - k_start));
      if (eq) {
        const char *k_end = eq;
        while (k_end > k_start && (*(k_end - 1) == ' ' || *(k_end - 1) == '\t')) {
          k_end--;
        }
        if ((size_t)(k_end - k_start) == klen &&
            memcmp(k_start, key, klen) == 0) {
          is_match = 1;
        }
      }
    }

    if (is_match) {
      size_t line_len = klen + 1U + vlen + 1U;
      if (out_size - out_used < line_len + 1U) {
        return 0;
      }
      memcpy(out + out_used, key, klen);
      out_used += klen;
      out[out_used++] = '=';
      memcpy(out + out_used, value, vlen);
      out_used += vlen;
      out[out_used++] = '\n';
      key_set = 1;
    } else {
      size_t line_len = (size_t)(next_line - line_start);
      if (out_size - out_used < line_len + 1U) {
        return 0;
      }
      memcpy(out + out_used, line_start, line_len);
      out_used += line_len;
    }
  }

  if (!key_set) {
    if (out_used > 0U && out[out_used - 1U] != '\n') {
      if (out_size - out_used < 2U) {
        return 0;
      }
      out[out_used++] = '\n';
    }
    {
      size_t line_len = klen + 1U + vlen + 1U;
      if (out_size - out_used < line_len + 1U) {
        return 0;
      }
      memcpy(out + out_used, key, klen);
      out_used += klen;
      out[out_used++] = '=';
      memcpy(out + out_used, value, vlen);
      out_used += vlen;
      out[out_used++] = '\n';
    }
  }

  out[out_used] = '\0';
  return 1;
}

#if defined(__amigaos__) || defined(AMIGA)
static void host_ensure_drawer(const char *drawer)
{
  BPTR lock = Lock((CONST_STRPTR)drawer, ACCESS_READ);
  if (lock) {
    UnLock(lock);
  } else {
    lock = CreateDir((CONST_STRPTR)drawer);
    if (lock) {
      UnLock(lock);
    }
  }
}

#include <exec/semaphores.h>
#include <proto/exec.h>

/* The Browse worker (sign-in, favourites) and the GUI (Settings, Recently
 * played) both read, change and write these files: one at a time, or a
 * save could drop the keys another just wrote. */
static struct SignalSemaphore host_files_lock;
static int host_files_lock_ready;

static void host_files_obtain(void)
{
  Forbid();
  if (!host_files_lock_ready) {
    InitSemaphore(&host_files_lock);
    host_files_lock_ready = 1;
  }
  Permit();
  ObtainSemaphore(&host_files_lock);
}

static void host_files_release(void)
{
  ReleaseSemaphore(&host_files_lock);
}
#else
static void host_files_obtain(void)
{
}

static void host_files_release(void)
{
}
#endif

static int host_pref_get_locked(const char *scheme, const char *key,
                                char *out, uint32_t size)
{
  char env_path[64];
  char envarc_path[64];
  void *buf = NULL;
  size_t len = 0U;
  int found = 0;

  if (out && size > 0U) {
    out[0] = '\0';
  }
  if (!scheme || !*scheme || !key || !out || size == 0U) {
    return 0;
  }

  (void)snprintf(env_path, sizeof(env_path), "ENV:ZZPlay/%s.prefs", scheme);
  (void)snprintf(envarc_path, sizeof(envarc_path), "ENVARC:ZZPlay/%s.prefs", scheme);

  if (zzplay_file_read_all(env_path, ZZPLAY_HOST_PREF_FILE_MAX, &buf, &len)) {
    char *text = (char *)buf;
    text[len] = '\0';
    found = zzplay_host_pref_text_get(text, key, out, size);
    zzplay_free(buf);
    return found;
  }

  if (zzplay_file_read_all(envarc_path, ZZPLAY_HOST_PREF_FILE_MAX, &buf, &len)) {
    char *text = (char *)buf;
    text[len] = '\0';
    found = zzplay_host_pref_text_get(text, key, out, size);
    zzplay_free(buf);
    return found;
  }

  return 0;
}

int zzplay_host_pref_get(const char *scheme, const char *key, char *out,
                         uint32_t size)
{
  int found;

  host_files_obtain();
  found = host_pref_get_locked(scheme, key, out, size);
  host_files_release();
  return found;
}

static int host_pref_set_locked(const char *scheme, const char *key,
                                const char *value)
{
  char env_path[64];
  char envarc_path[64];
  void *old_buf = NULL;
  size_t old_len = 0U;
  char *new_buf = NULL;
  size_t new_cap;
  int ok;

  if (!scheme || !*scheme || !key || !value) {
    return 0;
  }

  (void)snprintf(env_path, sizeof(env_path), "ENV:ZZPlay/%s.prefs", scheme);
  (void)snprintf(envarc_path, sizeof(envarc_path), "ENVARC:ZZPlay/%s.prefs", scheme);

  if (!zzplay_file_read_all(env_path, ZZPLAY_HOST_PREF_FILE_MAX, &old_buf, &old_len)) {
    (void)zzplay_file_read_all(envarc_path, ZZPLAY_HOST_PREF_FILE_MAX, &old_buf, &old_len);
  }
  if (old_buf) {
    ((char *)old_buf)[old_len] = '\0';
  }

  new_cap = old_len + strlen(key) + strlen(value) + 64U;
  if (new_cap < 1024U) {
    new_cap = 1024U;
  }
  new_buf = (char *)zzplay_alloc(new_cap);
  if (!new_buf) {
    if (old_buf) {
      zzplay_free(old_buf);
    }
    return 0;
  }

  ok = zzplay_host_pref_text_set(old_buf ? (const char *)old_buf : "",
                                 key, value, new_buf, (uint32_t)new_cap);
  if (old_buf) {
    zzplay_free(old_buf);
  }

  if (!ok) {
    zzplay_free(new_buf);
    return 0;
  }

#if defined(__amigaos__) || defined(AMIGA)
  host_ensure_drawer("ENV:ZZPlay");
  host_ensure_drawer("ENVARC:ZZPlay");
#endif

  {
    size_t new_len = strlen(new_buf);
    int w1 = zzplay_file_write_all(env_path, new_buf, new_len);
    int w2 = zzplay_file_write_all(envarc_path, new_buf, new_len);
    zzplay_free(new_buf);
#if defined(__amigaos__) || defined(AMIGA)
    return (w1 && w2) ? 1 : 0;
#else
    return (w1 || w2 || ok) ? 1 : 0;
#endif
  }
}

int zzplay_host_pref_set(const char *scheme, const char *key,
                         const char *value)
{
  int ok;

  host_files_obtain();
  ok = host_pref_set_locked(scheme, key, value);
  host_files_release();
  return ok;
}

/* "ENV:ZZPlay/<scheme><suffix>" and its ENVARC: twin; 0 when either
 * does not fit. */
static int host_text_paths(const char *scheme, const char *suffix,
                           char *env_path, char *envarc_path, size_t size)
{
  int n1;
  int n2;

  if (!scheme || !*scheme || !suffix) {
    return 0;
  }
  n1 = snprintf(env_path, size, "ENV:ZZPlay/%s%s", scheme, suffix);
  n2 = snprintf(envarc_path, size, "ENVARC:ZZPlay/%s%s", scheme, suffix);
  return n1 > 0 && n2 > 0 && (size_t)n1 < size && (size_t)n2 < size;
}

int zzplay_host_text_load(const char *scheme, const char *suffix,
                          uint32_t max_len, char **text, uint32_t *length)
{
  char env_path[64];
  char envarc_path[64];
  void *buf = NULL;
  size_t len = 0U;

  if (!text || !length) {
    return 0;
  }
  *text = NULL;
  *length = 0U;
  if (!host_text_paths(scheme, suffix, env_path, envarc_path,
                       sizeof(env_path))) {
    return 0;
  }
  host_files_obtain();
  if (!zzplay_file_read_all(env_path, (size_t)max_len, &buf, &len) &&
      !zzplay_file_read_all(envarc_path, (size_t)max_len, &buf, &len)) {
    host_files_release();
    return 0;
  }
  host_files_release();
  *text = (char *)buf;
  *length = (uint32_t)len;
  return 1;
}

int zzplay_host_text_save(const char *scheme, const char *suffix,
                          const char *text, uint32_t length)
{
  char env_path[64];
  char envarc_path[64];
  int w1;
  int w2;

  if (!text || !host_text_paths(scheme, suffix, env_path, envarc_path,
                                sizeof(env_path))) {
    return 0;
  }
  host_files_obtain();
#if defined(__amigaos__) || defined(AMIGA)
  host_ensure_drawer("ENV:ZZPlay");
  host_ensure_drawer("ENVARC:ZZPlay");
#endif
  w1 = zzplay_file_write_all(env_path, text, (size_t)length);
  w2 = zzplay_file_write_all(envarc_path, text, (size_t)length);
  host_files_release();
  return (w1 && w2) ? 1 : 0;
}

static void *host_alloc(const ZZPlayHost *host, uint32_t size)
{
  (void)host;
  return zzplay_alloc((size_t)size);
}

static void host_free(const ZZPlayHost *host, void *ptr)
{
  (void)host;
  zzplay_free(ptr);
}

static int host_format(char *out, uint32_t size, const char *fmt, ...)
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

typedef struct HostXmlWrapper {
  ZZPlayXml xml;
  ZZPlayHostXmlHandler handler;
} HostXmlWrapper;

static int host_xml_start_cb(void *user, const char *name,
                             const ZZPlayXmlAttr *attrs, uint32_t count,
                             int truncated)
{
  HostXmlWrapper *w = (HostXmlWrapper *)user;
  if (!w->handler.start) {
    return 0;
  }
  return w->handler.start(w->handler.user, name,
                          (const ZZPlayHostXmlAttr *)attrs, count, truncated);
}

static int host_xml_end_cb(void *user, const char *name)
{
  HostXmlWrapper *w = (HostXmlWrapper *)user;
  if (!w->handler.end) {
    return 0;
  }
  return w->handler.end(w->handler.user, name);
}

static void *host_xml_open(const ZZPlayHost *host,
                           const ZZPlayHostXmlHandler *handler)
{
  HostXmlWrapper *w;
  ZZPlayXmlHandler internal_handler;

  (void)host;
  if (!handler) {
    return NULL;
  }

  w = (HostXmlWrapper *)zzplay_alloc(sizeof(HostXmlWrapper));
  if (!w) {
    return NULL;
  }
  w->handler = *handler;
  internal_handler.start = host_xml_start_cb;
  internal_handler.end = host_xml_end_cb;
  internal_handler.user = w;
  zzplay_xml_init(&w->xml, &internal_handler);
  return w;
}

static int host_xml_feed(const ZZPlayHost *host, void *xml, const char *data,
                         uint32_t len)
{
  HostXmlWrapper *w = (HostXmlWrapper *)xml;
  (void)host;
  if (!w || !data || len == 0U) {
    return 0;
  }
  return zzplay_xml_feed(&w->xml, data, (size_t)len);
}

static int host_xml_finish(const ZZPlayHost *host, void *xml)
{
  HostXmlWrapper *w = (HostXmlWrapper *)xml;
  (void)host;
  return w != NULL && zzplay_xml_finish(&w->xml);
}

static void host_xml_close(const ZZPlayHost *host, void *xml)
{
  (void)host;
  if (xml) {
    zzplay_free(xml);
  }
}

static const char *host_xml_attr(const ZZPlayHostXmlAttr *attrs, uint32_t count,
                                 const char *name)
{
  return zzplay_xml_attr((const ZZPlayXmlAttr *)attrs, count, name);
}

static void host_stage_cb(void *user, ZZPlayNetStage stage, const char *host)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)user;
  if (b && b->stage) {
    b->stage(b->user, (int)stage, host);
  }
}

static int host_keep_running_cb(void *user)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)user;
  return (b && b->keep_running) ? b->keep_running(b->user) : 1;
}

static int host_http_request(const ZZPlayHost *host, const char *method,
                             const char *url, const char *headers,
                             const uint8_t *body, uint32_t body_len,
                             int (*data)(void *user, const uint8_t *bytes,
                                         uint32_t len),
                             void *user, int *status, char *err,
                             uint32_t err_size)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)host;
  ZZPlayNetRequest req;
  ZZPlayHttpHead head;
  ZZPlayNet *net;
  int cancelled = 0;
  uint8_t *buf;
  long n;
  int stop = 0;

  if (status) {
    *status = 0;
  }
  if (err && err_size > 0U) {
    err[0] = '\0';
  }
  if (!url) {
    if (err && err_size > 0U) {
      (void)snprintf(err, err_size, "Missing URL");
    }
    return 0;
  }

  memset(&req, 0, sizeof(req));
  req.url = url;
  req.method = method;
  req.body = body;
  req.body_len = (size_t)body_len;
  req.extra_headers = headers;
  req.keep_running = (b && b->keep_running) ? host_keep_running_cb : NULL;
  req.stage = (b && b->stage) ? host_stage_cb : NULL;
  req.user = (void *)b;
  net = zzplay_net_get(&req, &head, err, err_size, &cancelled);
  if (head.status > 0 && status) {
    *status = head.status;
  }
  if (!net) {
    if (cancelled && err && err_size > 0U && err[0] == '\0') {
      (void)snprintf(err, err_size, "Cancelled");
    }
    return 0;
  }

  buf = (uint8_t *)zzplay_alloc(4096U);
  if (!buf) {
    zzplay_net_close(net);
    if (err && err_size > 0U) {
      (void)snprintf(err, err_size, "Out of memory");
    }
    return 0;
  }

  while ((n = zzplay_net_read(net, buf, 4096U)) > 0) {
    if (data) {
      if (data(user, buf, (uint32_t)n) != 0) {
        stop = 1;
        break;
      }
    }
  }

  if (n < 0 && !stop) {
    if (err && err_size > 0U && err[0] == '\0') {
      const char *net_err = zzplay_net_error(net);
      if (net_err && *net_err) {
        (void)snprintf(err, err_size, "%s", net_err);
      } else if (zzplay_net_cancelled(net)) {
        (void)snprintf(err, err_size, "Cancelled");
      } else {
        (void)snprintf(err, err_size, "Network read error");
      }
    }
    zzplay_free(buf);
    zzplay_net_close(net);
    return 0;
  }

  zzplay_free(buf);
  zzplay_net_close(net);
  return 1;
}

static int host_http_get(const ZZPlayHost *host, const char *url,
                         const char *headers,
                         int (*data)(void *user, const uint8_t *bytes,
                                     uint32_t len),
                         void *user, int *status, char *err, uint32_t err_size)
{
  return host_http_request(host, "GET", url, headers, NULL, 0U, data, user,
                           status, err, err_size);
}

static int host_http_post(const ZZPlayHost *host, const char *url,
                          const char *headers, const uint8_t *body,
                          uint32_t body_len,
                          int (*data)(void *user, const uint8_t *bytes,
                                      uint32_t len),
                          void *user, int *status, char *err, uint32_t err_size)
{
  return host_http_request(host, "POST", url, headers, body, body_len, data,
                           user, status, err, err_size);
}

#if !defined(__amigaos__) && !defined(AMIGA)
__attribute__((weak)) ZZPlayNet *zzplay_net_get(const ZZPlayNetRequest *req,
                                                ZZPlayHttpHead *head,
                                                char *err, size_t err_size,
                                                int *cancelled)
{
  (void)req;
  (void)head;
  (void)cancelled;
  if (err && err_size > 0U) {
    (void)snprintf(err, err_size, "HTTP requests not supported on host");
  }
  return NULL;
}

__attribute__((weak)) long zzplay_net_read(ZZPlayNet *net, uint8_t *buf, size_t size)
{
  (void)net;
  (void)buf;
  (void)size;
  return -1;
}

__attribute__((weak)) const char *zzplay_net_error(const ZZPlayNet *net)
{
  (void)net;
  return "";
}

__attribute__((weak)) int zzplay_net_cancelled(const ZZPlayNet *net)
{
  (void)net;
  return 0;
}

__attribute__((weak)) void zzplay_net_close(ZZPlayNet *net)
{
  (void)net;
}
#endif

static int host_sleep(const ZZPlayHost *host, uint32_t ms)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)host;
  while (ms > 0U) {
    uint32_t step = ms > 100U ? 100U : ms;
    if (b && b->keep_running && !b->keep_running(b->user)) {
      return 0;
    }
#if defined(__amigaos__) || defined(AMIGA)
    {
      uint32_t ticks = (step * 50U + 999U) / 1000U;
      if (ticks == 0U) {
        ticks = 1U;
      }
      Delay(ticks);
    }
#endif
    ms -= step;
  }
  if (b && b->keep_running && !b->keep_running(b->user)) {
    return 0;
  }
  return 1;
}

static int host_table_pref_get(const ZZPlayHost *host, const char *key,
                               char *out, uint32_t size)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)host;
  return zzplay_host_pref_get(b ? b->scheme : "", key, out, size);
}

static int host_table_pref_set(const ZZPlayHost *host, const char *key,
                               const char *value)
{
  const ZZPlayHostBinding *b = (const ZZPlayHostBinding *)host;
  return zzplay_host_pref_set(b ? b->scheme : "", key, value);
}

void zzplay_host_bind(ZZPlayHostBinding *binding, const char *scheme,
                      int (*keep_running)(void *user), void *user)
{
  if (!binding) {
    return;
  }
  memset(binding, 0, sizeof(*binding));

  binding->table.abi = ZZPLAY_PROVIDER_ABI;
  binding->table.size = (uint32_t)sizeof(ZZPlayHost);
  binding->table.alloc = host_alloc;
  binding->table.free = host_free;
  binding->table.http_get = host_http_get;
  binding->table.xml_open = host_xml_open;
  binding->table.xml_feed = host_xml_feed;
  binding->table.xml_close = host_xml_close;
  binding->table.xml_attr = host_xml_attr;
  binding->table.url_escape = zzplay_host_url_escape;
  binding->table.format = host_format;
  binding->table.pref_get = host_table_pref_get;
  binding->table.pref_set = host_table_pref_set;
  binding->table.http_post = host_http_post;
  binding->table.sleep = host_sleep;
  binding->table.xml_finish = host_xml_finish;
  if (scheme) {
    strncpy(binding->scheme, scheme, sizeof(binding->scheme) - 1U);
    binding->scheme[sizeof(binding->scheme) - 1U] = '\0';
  }
  binding->keep_running = keep_running;
  binding->user = user;
}
