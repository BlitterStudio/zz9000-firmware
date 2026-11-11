/* HTTP client over bsdsocket.library for ZZPlay's network sources.
 *
 * Connection handling follows the ZZTop updater's HTTPS client
 * (zz9000-drivers ZZTop/Core/https.c), reworked so several processes can
 * each hold connections: all state lives in the connection, including the
 * bsdsocket.library base.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-net.h"

#include "zzplay-sys.h"

#include <exec/types.h>
#include <proto/exec.h>

/* The inline bsdsocket stubs read the library base through this name, so
 * every function below that calls bsdsocket.library has a `net` in scope
 * and uses that connection's own base. */
#define BSDSOCKET_BASE_NAME (net->socket_base)
#include <proto/socket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define NET_MAX_REDIRECTS 5
#define NET_HEAD_MAX 8192U
#define NET_IO_BYTES 8192U
#define NET_POLL_MICROS 100000UL
#define NET_IDLE_POLLS 300 /* 30 s without progress */
#define NET_USER_AGENT "ZZPlay (AmigaOS; ZZ9000)"

struct ZZPlayNet {
  struct Library *socket_base;
  LONG fd;
  ZZPlayNetKeepRunning keep_running;
  void *user;
  int cancelled;
  char err[ZZPLAY_NET_ERROR_MAX];
  char url[ZZPLAY_HTTP_URL_MAX];
  /* Redirect bookkeeping: kept here rather than on the caller's stack. */
  ZZPlayUrl origin;
  ZZPlayUrl at;
  ZZPlayUrl target;

  /* Body framing of the final response. */
  int chunked;
  int has_length;
  uint32_t remaining;
  int body_done;
  ZZPlayChunked chunk;

  /* Received bytes not yet consumed, and de-chunked bytes not yet
   * delivered. */
  uint32_t raw_off;
  uint32_t raw_len;
  uint32_t out_off;
  uint32_t out_len;
  uint8_t raw[NET_IO_BYTES];
  uint8_t out[NET_IO_BYTES];
  char head[NET_HEAD_MAX];
};

static void set_err(ZZPlayNet *net, const char *text)
{
  (void)snprintf(net->err, sizeof(net->err), "%s", text);
}

static void set_err_host(ZZPlayNet *net, const char *fmt, const char *host)
{
  (void)snprintf(net->err, sizeof(net->err), fmt, host);
}

static int keep_running(ZZPlayNet *net)
{
  if (!net->cancelled && net->keep_running != NULL &&
      !net->keep_running(net->user)) {
    net->cancelled = 1;
    set_err(net, "Cancelled");
  }
  return !net->cancelled;
}

/* Wait until the socket can be read or written, polling the caller in
 * between. Returns 1 when ready, 0 on cancel, error or timeout. */
static int wait_socket(ZZPlayNet *net, int for_write)
{
  int polls;

  for (polls = 0; polls < NET_IDLE_POLLS; polls++) {
    fd_set fds;
    struct __timeval tv;
    LONG n;

    if (!keep_running(net)) {
      return 0;
    }
    FD_ZERO(&fds);
    FD_SET(net->fd, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = NET_POLL_MICROS;
    n = WaitSelect(net->fd + 1, for_write ? NULL : &fds,
                   for_write ? &fds : NULL, NULL, &tv, NULL);
    if (n > 0) {
      return 1;
    }
    if (n < 0) {
      set_err(net, "Network wait failed");
      return 0;
    }
  }
  set_err(net, "The server stopped responding");
  return 0;
}

static void close_socket(ZZPlayNet *net)
{
  if (net->fd >= 0) {
    CloseSocket(net->fd);
    net->fd = -1;
  }
}

static int connect_to(ZZPlayNet *net, const ZZPlayUrl *url)
{
  struct sockaddr_in sa;
  ULONG addr;
  LONG one = 1;

  if (!keep_running(net)) {
    return 0;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sin_len = sizeof(sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(url->port);

  /* A dotted address needs no lookup. Otherwise the resolver blocks until
   * it answers; bsdsocket offers no other kind. */
  addr = inet_addr((STRPTR)url->host);
  if (addr != (ULONG)INADDR_NONE) {
    sa.sin_addr.s_addr = addr;
  } else {
    struct hostent *he = gethostbyname((STRPTR)url->host);

    if (he == NULL || he->h_addrtype != AF_INET ||
        he->h_addr_list[0] == NULL) {
      set_err_host(net, "Cannot find the server %s", url->host);
      return 0;
    }
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
  }
  if (!keep_running(net)) {
    return 0;
  }

  net->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (net->fd < 0) {
    set_err(net, "Cannot create a network socket");
    return 0;
  }
  IoctlSocket(net->fd, FIONBIO, (APTR)&one);
  if (connect(net->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    LONG soerr = 0;
    socklen_t len = sizeof(soerr);

    if (Errno() != EINPROGRESS) {
      set_err_host(net, "Cannot connect to %s", url->host);
      return 0;
    }
    if (!wait_socket(net, 1)) {
      return 0;
    }
    if (getsockopt(net->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 ||
        soerr != 0) {
      set_err_host(net, "Cannot connect to %s", url->host);
      return 0;
    }
  }
  return 1;
}

static int send_all(ZZPlayNet *net, const char *data, size_t len)
{
  while (len != 0U) {
    LONG n;

    if (!keep_running(net)) {
      return 0;
    }
    n = send(net->fd, (APTR)data, (LONG)len, 0);
    if (n > 0) {
      data += n;
      len -= (size_t)n;
    } else if (n < 0 && Errno() == EWOULDBLOCK) {
      if (!wait_socket(net, 1)) {
        return 0;
      }
    } else {
      set_err(net, "Sending the request failed");
      return 0;
    }
  }
  return 1;
}

/* Receive some bytes: the count, 0 when the server closed the connection,
 * -1 on failure or cancel. The hook runs before every receive, not only
 * while waiting: a fast stream can keep recv() succeeding indefinitely. */
static long recv_some(ZZPlayNet *net, uint8_t *buf, size_t size)
{
  for (;;) {
    LONG n;

    if (!keep_running(net)) {
      return -1;
    }
    n = recv(net->fd, (APTR)buf, (LONG)size, 0);
    if (n >= 0) {
      return (long)n;
    }
    if (Errno() != EWOULDBLOCK) {
      set_err(net, "Receiving data failed");
      return -1;
    }
    if (!wait_socket(net, 0)) {
      return -1;
    }
  }
}

/* Read the response head. On success the bytes after it are left in
 * net->raw. */
static int read_head(ZZPlayNet *net, ZZPlayHttpHead *head)
{
  uint32_t have = 0U;
  char *end = NULL;
  uint32_t sep = 4U;
  uint32_t body;

  while (end == NULL) {
    long n;

    if (have >= sizeof(net->head) - 1U) {
      set_err(net, "The server sent oversized headers");
      return 0;
    }
    n = recv_some(net, (uint8_t *)net->head + have,
                  sizeof(net->head) - 1U - have);
    if (n < 0) {
      return 0;
    }
    if (n == 0) {
      set_err(net, "The server closed the connection without answering");
      return 0;
    }
    have += (uint32_t)n;
    net->head[have] = '\0';
    end = strstr(net->head, "\r\n\r\n");
    sep = 4U;
    if (end == NULL) {
      /* Some Shoutcast servers end the head with bare newlines. */
      end = strstr(net->head, "\n\n");
      sep = 2U;
    }
  }
  body = (uint32_t)(end - net->head) + sep;
  if (!zzplay_http_parse_head(net->head, (size_t)(end - net->head), head)) {
    set_err(net, "The server sent a malformed response");
    return 0;
  }
  net->raw_len = have - body;
  net->raw_off = 0U;
  memcpy(net->raw, net->head + body, net->raw_len);
  return 1;
}

static int is_redirect(int status)
{
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

ZZPlayNet *zzplay_net_get(const ZZPlayNetRequest *req, ZZPlayHttpHead *head,
                          char *err, size_t err_size, int *cancelled)
{
  static const char no_stack[] = "No TCP/IP stack is running";
  ZZPlayNet *net;
  int redirects;

  if (cancelled != NULL) {
    *cancelled = 0;
  }
  if (err != NULL && err_size != 0U) {
    err[0] = '\0';
  }
  memset(head, 0, sizeof(*head));
  net = (ZZPlayNet *)zzplay_alloc(sizeof(*net));
  if (net == NULL) {
    if (err != NULL) {
      (void)snprintf(err, err_size, "%s", "Out of memory");
    }
    return NULL;
  }
  memset(net, 0, sizeof(*net));
  net->fd = -1;
  if (req == NULL || req->url == NULL ||
      !zzplay_http_parse_url(req->url, &net->origin)) {
    set_err(net, "Not a valid http:// address");
    goto fail;
  }
  net->keep_running = req->keep_running;
  net->user = req->user;
  net->socket_base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
  if (net->socket_base == NULL) {
    set_err(net, no_stack);
    goto fail;
  }

  net->at = net->origin;
  (void)snprintf(net->url, sizeof(net->url), "%s", req->url);
  for (redirects = 0;; redirects++) {
    size_t len;
    int same_origin = zzplay_http_same_origin(&net->origin, &net->at);

    if (net->at.tls) {
      set_err(net, "Secure (https://) addresses are not supported yet");
      goto fail;
    }
    /* The head buffer holds the request until the response replaces it. */
    len = zzplay_http_format_request(
        net->head, sizeof(net->head), &net->at, NET_USER_AGENT,
        req->has_range, req->range_from, req->icy,
        same_origin ? req->extra_headers : NULL);
    if (len == 0U) {
      set_err(net, "The address is too long");
      goto fail;
    }
    if (!connect_to(net, &net->at) || !send_all(net, net->head, len) ||
        !read_head(net, head)) {
      goto fail;
    }
    if (is_redirect(head->status) && head->location[0] != '\0') {
      if (redirects == NET_MAX_REDIRECTS) {
        set_err(net, "Too many redirects");
        goto fail;
      }
      if (!zzplay_http_resolve(&net->at, head->location, net->url,
                               sizeof(net->url)) ||
          !zzplay_http_parse_url(net->url, &net->target)) {
        set_err(net, "The server redirected to an unusable address");
        goto fail;
      }
      if (!zzplay_http_redirect_allowed(&net->at, &net->target)) {
        set_err(net, "Refused a redirect from https:// to http://");
        goto fail;
      }
      close_socket(net);
      net->at = net->target;
      continue;
    }
    break;
  }

  if (head->status < 200 || head->status > 299) {
    (void)snprintf(net->err, sizeof(net->err), "The server answered HTTP %d",
                   head->status);
    goto fail;
  }
  net->chunked = head->chunked;
  net->has_length = head->has_length && !head->chunked;
  net->remaining = head->content_length;
  zzplay_chunked_init(&net->chunk);
  if (net->has_length && net->remaining == 0U) {
    net->body_done = 1;
  }
  return net;

fail:
  if (err != NULL) {
    (void)snprintf(err, err_size, "%s", net->err);
  }
  if (cancelled != NULL) {
    *cancelled = net->cancelled;
  }
  zzplay_net_close(net);
  return NULL;
}

/* Make sure net->raw holds unconsumed bytes. Returns the count available,
 * 0 when the server closed the connection, -1 on failure. */
static long fill_raw(ZZPlayNet *net)
{
  long n;

  if (net->raw_off < net->raw_len) {
    return (long)(net->raw_len - net->raw_off);
  }
  n = recv_some(net, net->raw, sizeof(net->raw));
  if (n <= 0) {
    return n;
  }
  net->raw_off = 0U;
  net->raw_len = (uint32_t)n;
  return n;
}

static int out_sink(void *ctx, const uint8_t *data, size_t len)
{
  ZZPlayNet *net = (ZZPlayNet *)ctx;

  /* The decoder is fed at most the free space, and decoded data is never
   * longer than its input. */
  if (len > sizeof(net->out) - net->out_len) {
    return 1;
  }
  memcpy(net->out + net->out_len, data, len);
  net->out_len += (uint32_t)len;
  return 0;
}

static long read_chunked(ZZPlayNet *net, uint8_t *buf, size_t size)
{
  while (net->out_off == net->out_len) {
    long avail;
    size_t feed;
    size_t used = 0U;
    int rc;

    if (net->body_done) {
      return 0;
    }
    avail = fill_raw(net);
    if (avail < 0) {
      return -1;
    }
    if (avail == 0) {
      set_err(net, "The connection closed before the end of the data");
      return -1;
    }
    net->out_off = 0U;
    net->out_len = 0U;
    feed = (size_t)avail;
    if (feed > sizeof(net->out)) {
      feed = sizeof(net->out);
    }
    rc = zzplay_chunked_feed(&net->chunk, net->raw + net->raw_off, feed,
                             out_sink, net, &used);
    net->raw_off += (uint32_t)used;
    if (rc == ZZPLAY_CHUNKED_ERROR) {
      set_err(net, "The server sent malformed chunked data");
      return -1;
    }
    if (rc == ZZPLAY_CHUNKED_DONE) {
      net->body_done = 1;
    }
  }
  if (size > net->out_len - net->out_off) {
    size = net->out_len - net->out_off;
  }
  memcpy(buf, net->out + net->out_off, size);
  net->out_off += (uint32_t)size;
  return (long)size;
}

long zzplay_net_read(ZZPlayNet *net, uint8_t *buf, size_t size)
{
  long avail;

  if (net == NULL || buf == NULL) {
    return -1;
  }
  if (size == 0U) {
    return 0;
  }
  if (net->chunked) {
    return read_chunked(net, buf, size);
  }
  if (net->body_done) {
    return 0;
  }
  avail = fill_raw(net);
  if (avail < 0) {
    return -1;
  }
  if (avail == 0) {
    if (net->has_length) {
      set_err(net, "The connection closed before the end of the data");
      return -1;
    }
    net->body_done = 1;
    return 0;
  }
  if (size > (size_t)avail) {
    size = (size_t)avail;
  }
  if (net->has_length && size > net->remaining) {
    size = net->remaining;
  }
  memcpy(buf, net->raw + net->raw_off, size);
  net->raw_off += (uint32_t)size;
  if (net->has_length) {
    net->remaining -= (uint32_t)size;
    if (net->remaining == 0U) {
      net->body_done = 1;
    }
  }
  return (long)size;
}

const char *zzplay_net_error(const ZZPlayNet *net)
{
  return net != NULL ? net->err : "";
}

int zzplay_net_cancelled(const ZZPlayNet *net)
{
  return net != NULL && net->cancelled;
}

const char *zzplay_net_url(const ZZPlayNet *net)
{
  return net != NULL ? net->url : "";
}

void zzplay_net_close(ZZPlayNet *net)
{
  if (net == NULL) {
    return;
  }
  close_socket(net);
  if (net->socket_base != NULL) {
    CloseLibrary(net->socket_base);
  }
  zzplay_free(net);
}
