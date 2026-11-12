/* HTTP and HTTPS client over bsdsocket.library and AmiSSL for ZZPlay's
 * network sources.
 *
 * Connection handling follows the ZZTop updater's HTTPS client
 * (zz9000-drivers ZZTop/Core/https.c), reworked so several processes can
 * each hold connections: all state lives in the connection, including the
 * bsdsocket.library and AmiSSL bases.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-net.h"

#include "zzplay-sys.h"

#include <dos/dosextens.h>
#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/exec.h>

/* The per-process network context (defined after the library headers, so
 * it can hold an AmiSSL session): a bsdsocket.library base belongs to the
 * process that opened it, socket descriptors belong to that base, and
 * AmiSSL must be given the base that created the sockets it reads. Each
 * process that uses the network therefore gets one context holding its
 * bsdsocket and AmiSSL bases, shared by its connections and counted by
 * them. The inline library stubs look the bases up for the calling
 * process. */
static struct Library *net_socket_base(void);
static struct Library *net_amissl_master_base(void);
static struct Library *net_amissl_base(void);
static struct Library *net_amissl_ext_base(void);

/* AmiSSL's call macros come first: its OpenSSL headers then skip the
 * plain prototypes. */
#define AMISSLMASTER_BASE_NAME (net_amissl_master_base())
#define AMISSL_BASE_NAME (net_amissl_base())
#define AMISSLEXT_BASE_NAME (net_amissl_ext_base())
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#define BSDSOCKET_BASE_NAME (net_socket_base())
#include <proto/socket.h>
#include <libraries/bsdsocket.h>
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
#define NET_SESSIONS 4U    /* remembered TLS sessions per process */
#define NET_TASKS 8U       /* processes with a network context */
#define NET_USER_AGENT "ZZPlay (AmigaOS; ZZ9000)"

typedef struct NetTask {
  struct Task *task;           /* owner, or NULL for a free slot */
  ULONG users;                 /* open connections */
  struct Library *socket;
  struct Library *master;      /* AmiSSL, from the first https:// hop */
  struct Library *amissl;
  struct Library *amissl_ext;
  int amissl_errno;
  /* The last TLS session per host:port, offered on a later connection
   * to the same host so it can resume instead of a full handshake.
   * Sessions belong to the AmiSSL bases above and die with them. */
  struct {
    char host[ZZPLAY_HTTP_HOST_MAX];
    uint16_t port;
    SSL_SESSION *session;
  } sessions[NET_SESSIONS];
} NetTask;

static NetTask net_tasks[NET_TASKS];

/* Only the owner writes a claimed slot; others only compare `task`. */
static NetTask *net_task(void)
{
  struct Task *me = FindTask(NULL);
  int i;

  for (i = 0; i < (int)NET_TASKS; i++) {
    if (net_tasks[i].task == me) {
      return &net_tasks[i];
    }
  }
  return NULL;
}

static struct Library *net_socket_base(void)
{
  return net_task()->socket;
}

static struct Library *net_amissl_master_base(void)
{
  return net_task()->master;
}

static struct Library *net_amissl_base(void)
{
  return net_task()->amissl;
}

static struct Library *net_amissl_ext_base(void)
{
  return net_task()->amissl_ext;
}

struct ZZPlayNet {
  NetTask *task;
  SSL_CTX *ctx;
  SSL *ssl;
  LONG fd;
  ZZPlayNetKeepRunning keep_running;
  ZZPlayNetStageHook stage;
  void *user;
  int cancelled;
  /* The server closed the connection before any response bytes. */
  int closed_early;
  /* Retrying after a TLS 1.3 connection that the server closed unanswered. */
  int tls12_only;
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

static void report_stage(ZZPlayNet *net, ZZPlayNetStage stage,
                         const char *host)
{
  if (net->stage != NULL) {
    net->stage(net->user, stage, host);
  }
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

static void ssl_err(ZZPlayNet *net, const char *what)
{
  unsigned long e = ERR_get_error();
  long verify = net->ssl != NULL ? SSL_get_verify_result(net->ssl)
                                 : X509_V_OK;

  if (verify != X509_V_OK) {
    (void)snprintf(net->err, sizeof(net->err), "%s: certificate %s", what,
                   X509_verify_cert_error_string(verify));
  } else if (e != 0UL && ERR_reason_error_string(e) != NULL) {
    (void)snprintf(net->err, sizeof(net->err), "%s: %s", what,
                   ERR_reason_error_string(e));
  } else {
    set_err(net, what);
  }
}

/* Drive a non-blocking TLS call that returned `rc`. Returns 1 to retry
 * after waiting, 0 on a real failure or cancel. */
static int ssl_retry(ZZPlayNet *net, int rc, const char *what)
{
  int e = SSL_get_error(net->ssl, rc);

  if (e == SSL_ERROR_WANT_READ) {
    return wait_socket(net, 0);
  }
  if (e == SSL_ERROR_WANT_WRITE) {
    return wait_socket(net, 1);
  }
  if (!net->cancelled) {
    ssl_err(net, what);
  }
  return 0;
}

static void close_socket(ZZPlayNet *net)
{
  if (net->ssl != NULL) {
    SSL_free(net->ssl);
    net->ssl = NULL;
  }
  if (net->ctx != NULL) {
    SSL_CTX_free(net->ctx);
    net->ctx = NULL;
  }
  if (net->fd >= 0) {
    CloseSocket(net->fd);
    net->fd = -1;
  }
}

/* Claim this process's context and make sure bsdsocket.library is open.
 * Returns NULL with the reason in `net` when it cannot. */
static NetTask *net_task_acquire(ZZPlayNet *net)
{
  struct Task *me = FindTask(NULL);
  NetTask *t;
  int i;

  Forbid();
  t = net_task();
  for (i = 0; t == NULL && i < (int)NET_TASKS; i++) {
    if (net_tasks[i].task == NULL) {
      t = &net_tasks[i];
      t->task = me;
    }
  }
  if (t != NULL) {
    t->users++;
  }
  Permit();
  if (t == NULL) {
    set_err(net, "Too many processes are using the network");
    return NULL;
  }
  net->task = t;
  if (t->socket == NULL) {
    struct TagItem errno_tags[2];

    t->socket = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (t->socket == NULL) {
      set_err(net, "No TCP/IP stack is running");
      return NULL;
    }
    /* AmiSSL reads socket errors through the errno variable it is given
     * (AmiSSL_ErrNoPtr), and bsdsocket.library writes that variable only
     * when told to. */
    errno_tags[0].ti_Tag = SBTM_SETVAL(SBTC_ERRNOLONGPTR);
    errno_tags[0].ti_Data = (ULONG)&t->amissl_errno;
    errno_tags[1].ti_Tag = TAG_DONE;
    errno_tags[1].ti_Data = 0;
    (void)SocketBaseTagList(errno_tags);
  }
  return t;
}

/* The last connection of a process closes its libraries: AmiSSL before
 * the socket base it was given. */
static void net_task_release(NetTask *t)
{
  if (t == NULL || --t->users != 0U) {
    return;
  }
  {
    int i;

    for (i = 0; i < (int)NET_SESSIONS; i++) {
      if (t->sessions[i].session != NULL) {
        SSL_SESSION_free(t->sessions[i].session);
        t->sessions[i].session = NULL;
        t->sessions[i].host[0] = '\0';
      }
    }
  }
  if (t->amissl != NULL) {
    CloseAmiSSL();
    t->amissl = NULL;
    t->amissl_ext = NULL;
  }
  if (t->master != NULL) {
    CloseLibrary(t->master);
    t->master = NULL;
  }
  if (t->socket != NULL) {
    CloseLibrary(t->socket);
    t->socket = NULL;
  }
  Forbid();
  t->task = NULL;
  Permit();
}

/* Open AmiSSL for this process. AmiSSL opens usergroup.library and falls
 * back to AmiTCP:Libs, so AmigaDOS volume requesters are off meanwhile: a
 * stack without that assign would otherwise stop the player on "insert
 * volume AmiTCP". */
static int tls_open(ZZPlayNet *net)
{
  NetTask *t = net->task;
  struct Process *me = (struct Process *)FindTask(NULL);
  APTR window = me->pr_WindowPtr;
  struct TagItem tags[6];
  int ok = 0;

  if (t->amissl != NULL) {
    return 1;
  }
  me->pr_WindowPtr = (APTR)-1;
  if (t->master == NULL) {
    t->master = OpenLibrary((CONST_STRPTR)"amisslmaster.library",
                            AMISSLMASTER_MIN_VERSION);
  }
  if (t->master == NULL) {
    set_err(net, "https:// needs AmiSSL 5, which is not installed");
  } else {
    tags[0].ti_Tag = AmiSSL_UsesOpenSSLStructs;
    tags[0].ti_Data = FALSE;
    tags[1].ti_Tag = AmiSSL_GetAmiSSLBase;
    tags[1].ti_Data = (ULONG)&t->amissl;
    tags[2].ti_Tag = AmiSSL_GetAmiSSLExtBase;
    tags[2].ti_Data = (ULONG)&t->amissl_ext;
    tags[3].ti_Tag = AmiSSL_SocketBase;
    tags[3].ti_Data = (ULONG)t->socket;
    tags[4].ti_Tag = AmiSSL_ErrNoPtr;
    tags[4].ti_Data = (ULONG)&t->amissl_errno;
    tags[5].ti_Tag = TAG_DONE;
    tags[5].ti_Data = 0;
    /* OpenSSL 3 API: any AmiSSL 5 release provides these calls. */
    if (OpenAmiSSLTagList(AMISSL_V3xx, tags) != 0) {
      t->amissl = NULL;
      t->amissl_ext = NULL;
      set_err(net, "AmiSSL could not be opened");
    } else {
      ok = 1;
    }
  }
  me->pr_WindowPtr = window;
  return ok;
}

/* The TLS session cache: the last session per host:port of this process,
 * offered on a later connection so the handshake can resume instead of
 * starting over. Sessions live as long as the process's AmiSSL bases. */
static int host_equal(const char *a, const char *b)
{
  size_t i;

  for (i = 0U; a[i] != '\0' && b[i] != '\0'; i++) {
    char ca = a[i];
    char cb = b[i];

    if (ca >= 'A' && ca <= 'Z') {
      ca = (char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return 0;
    }
  }
  return a[i] == '\0' && b[i] == '\0';
}

static SSL_SESSION *session_cache_get(NetTask *t, const char *host,
                                      uint16_t port)
{
  int i;

  for (i = 0; i < (int)NET_SESSIONS; i++) {
    if (t->sessions[i].session != NULL &&
        t->sessions[i].port == port &&
        host_equal(t->sessions[i].host, host)) {
      return t->sessions[i].session;
    }
  }
  return NULL;
}

static void session_cache_put(NetTask *t, const char *host, uint16_t port,
                              SSL_SESSION *session)
{
  int i;
  int slot = -1;

  for (i = 0; i < (int)NET_SESSIONS; i++) {
    if (t->sessions[i].session != NULL) {
      if (t->sessions[i].port == port &&
          host_equal(t->sessions[i].host, host)) {
        slot = i; /* replace the entry for this host:port */
        break;
      }
    } else if (slot < 0) {
      slot = i; /* first free slot */
    }
  }
  if (slot < 0) {
    /* Full: recycle the oldest entry (index 0). */
    slot = 0;
  }
  if (t->sessions[slot].session != NULL) {
    SSL_SESSION_free(t->sessions[slot].session);
  }
  memset(t->sessions[slot].host, 0, sizeof(t->sessions[slot].host));
  strncpy(t->sessions[slot].host, host, sizeof(t->sessions[slot].host) - 1U);
  t->sessions[slot].port = port;
  t->sessions[slot].session = session;
}

static void session_cache_drop(NetTask *t, const char *host, uint16_t port)
{
  int i;

  for (i = 0; i < (int)NET_SESSIONS; i++) {
    if (t->sessions[i].session != NULL &&
        t->sessions[i].port == port &&
        host_equal(t->sessions[i].host, host)) {
      SSL_SESSION_free(t->sessions[i].session);
      t->sessions[i].session = NULL;
      t->sessions[i].host[0] = '\0';
      return;
    }
  }
}

/* TLS 1.2 or later over the connected socket, with the certificate and
 * host name verified against AmiSSL's certificate store. A remembered
 * session for this host:port is offered first; the server falls back to
 * a full handshake when it no longer accepts it. */
static int tls_connect(ZZPlayNet *net, const ZZPlayUrl *url)
{
  int rc;
  SSL_SESSION *resume;

  net->ctx = SSL_CTX_new(TLS_client_method());
  if (net->ctx == NULL) {
    ssl_err(net, "TLS setup failed");
    return 0;
  }
  SSL_CTX_set_min_proto_version(net->ctx, TLS1_2_VERSION);
  if (net->tls12_only) {
    SSL_CTX_set_max_proto_version(net->ctx, TLS1_2_VERSION);
  }
  /* Many stream servers close the connection without a TLS close_notify.
   * AmiSSL 5 reports that as an error; treat it as the end of the data,
   * as over plain HTTP, and let the HTTP framing decide whether the body
   * was complete. */
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
  SSL_CTX_set_options(net->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
  SSL_CTX_set_verify(net->ctx, SSL_VERIFY_PEER, NULL);
  if (SSL_CTX_set_default_verify_paths(net->ctx) != 1) {
    ssl_err(net, "Cannot load the AmiSSL certificates");
    return 0;
  }
  net->ssl = SSL_new(net->ctx);
  if (net->ssl == NULL || SSL_set_fd(net->ssl, (int)net->fd) != 1 ||
      SSL_set_tlsext_host_name(net->ssl, url->host) != 1 ||
      SSL_set1_host(net->ssl, url->host) != 1) {
    ssl_err(net, "TLS setup failed");
    return 0;
  }
  resume = session_cache_get(net->task, url->host, (uint16_t)url->port);
  if (resume != NULL && SSL_set_session(net->ssl, resume) != 1) {
    ssl_err(net, "TLS setup failed");
    return 0;
  }
  while ((rc = SSL_connect(net->ssl)) != 1) {
    if (!ssl_retry(net, rc, "Secure connection failed")) {
      if (resume != NULL) {
        /* The remembered session did not work: a later connection to
         * this host must do the full handshake again. */
        session_cache_drop(net->task, url->host, (uint16_t)url->port);
      }
      return 0;
    }
  }
  {
    SSL_SESSION *done = SSL_get1_session(net->ssl);

    if (done != NULL) {
      session_cache_put(net->task, url->host, (uint16_t)url->port, done);
    }
  }
  return 1;
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
    struct hostent *he;

    report_stage(net, ZZPLAY_NET_STAGE_LOOKUP, url->host);
    he = gethostbyname((STRPTR)url->host);
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

  report_stage(net, ZZPLAY_NET_STAGE_CONNECT, url->host);
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
    if (net->ssl != NULL) {
      int rc = SSL_write(net->ssl, data, (int)len);

      if (rc > 0) {
        data += rc;
        len -= (size_t)rc;
      } else if (!ssl_retry(net, rc, "Sending the request failed")) {
        return 0;
      }
      continue;
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
    if (net->ssl != NULL) {
      int rc = SSL_read(net->ssl, buf, (int)size);
      int e;

      if (rc > 0) {
        return (long)rc;
      }
      e = SSL_get_error(net->ssl, rc);
      if (e == SSL_ERROR_ZERO_RETURN) {
        return 0;
      }
      /* A server that closes without close_notify after a delimited body:
       * the framing decides whether that was the end. */
      if (e == SSL_ERROR_SYSCALL && ERR_peek_error() == 0UL) {
        return 0;
      }
      if (!ssl_retry(net, rc, "Receiving data failed")) {
        return -1;
      }
      continue;
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
      net->closed_early = 1;
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
    set_err(net, "Not a valid http:// or https:// address");
    goto fail;
  }
  net->keep_running = req->keep_running;
  net->stage = req->stage;
  net->user = req->user;
  if (net_task_acquire(net) == NULL) {
    goto fail;
  }

  net->at = net->origin;
  (void)snprintf(net->url, sizeof(net->url), "%s", req->url);
  for (redirects = 0;; redirects++) {
    size_t len;
    int same_origin = zzplay_http_same_origin(&net->origin, &net->at);

    if (net->at.tls && !tls_open(net)) {
      goto fail;
    }
    /* The head buffer holds the request until the response replaces it. */
    len = zzplay_http_format_request(
        net->head, sizeof(net->head), req->method, &net->at, NET_USER_AGENT,
        req->has_range, req->range_from, req->icy, req->body_len,
        same_origin ? req->extra_headers : NULL);
    if (len == 0U) {
      set_err(net, "The address is too long");
      goto fail;
    }
    net->closed_early = 0;
    if (!connect_to(net, &net->at)) {
      goto fail;
    }
    if (net->at.tls) {
      report_stage(net, ZZPLAY_NET_STAGE_SECURE, net->at.host);
      if (!tls_connect(net, &net->at)) {
        goto fail;
      }
    }
    if (!send_all(net, net->head, len)) {
      goto fail;
    }
    if (req->body != NULL && req->body_len > 0U) {
      if (!send_all(net, (const char *)req->body, req->body_len)) {
        goto fail;
      }
    }
    report_stage(net, ZZPLAY_NET_STAGE_WAIT, net->at.host);
    if (!read_head(net, head)) {
      /* laut.fm's stream servers close a TLS 1.3 connection from AmiSSL
       * without answering, while TLS 1.2 to the same server works and
       * stays at least as strict as the minimum. Try once more that way. */
      if (net->closed_early && net->ssl != NULL && !net->tls12_only &&
          SSL_version(net->ssl) == TLS1_3_VERSION) {
        net->tls12_only = 1;
        close_socket(net);
        redirects--;
        continue;
      }
      goto fail;
    }
    report_stage(net, ZZPLAY_NET_STAGE_BODY, net->at.host);
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
  net_task_release(net->task);
  zzplay_free(net);
}
