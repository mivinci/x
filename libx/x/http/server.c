/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server.c - Asynchronous HTTP server implementation
 */

#include "proto_h1.h"
#include "proto_h2.h"
#include "server_private.h"
#include "ws_private.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <x/base/log.h>
#include <x/net/transport_private.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Forward declarations
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void on_listen_event(xSocket sock, xEventMask mask, void *arg);
static void on_conn_event(xSocket sock, xEventMask mask, void *arg);

/* Internal helpers */
static void conn_init_parser(struct xHttpConn_ *conn);
static void conn_reset_request_state(struct xHttpConn_ *conn);
static void conn_dispatch_request(struct xHttpConn_ *conn);
static int  conn_write_ready(struct xHttpConn_ *conn);
static void conn_after_response(struct xHttpConn_ *conn);
static void conn_try_flush(struct xHttpConn_ *conn);
/* xHttpConnFlushWriteInternal is declared in server_private.h. */

/* ═══════════════════════════════════════════════════════════════════════════
 *  HTTP status reason phrases
 * ═══════════════════════════════════════════════════════════════════════════
 */

const char *xHttpStatusReason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 408:
    return "Request Timeout";
  case 413:
    return "Content Too Large";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  default:
    return "Unknown";
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Route parsing helpers (shared with mux)
 * ═══════════════════════════════════════════════════════════════════════════
 */

int xHttpRouteParseSegments_(const char *path, struct xHttpRouteSegment_ **out) {
  int         count = 0;
  const char *p     = path;
  while (*p) {
    if (*p == '/') {
      p++;
      continue;
    }
    count++;
    while (*p && *p != '/')
      p++;
  }

  if (count == 0) {
    *out = NULL;
    return 0;
  }

  struct xHttpRouteSegment_ *segs =
    (struct xHttpRouteSegment_ *)calloc((size_t)count, sizeof(struct xHttpRouteSegment_));
  if (!segs) return -1;

  int i = 0;
  p     = path;
  while (*p) {
    if (*p == '/') {
      p++;
      continue;
    }
    const char *start = p;
    while (*p && *p != '/')
      p++;
    size_t len = (size_t)(p - start);

    if (start[0] == ':' && len > 1) {
      segs[i].text  = NULL;
      segs[i].param = strndup(start + 1, len - 1);
      if (!segs[i].param) goto fail;
    } else {
      segs[i].text  = strndup(start, len);
      segs[i].param = NULL;
      if (!segs[i].text) goto fail;
    }
    i++;
  }

  *out = segs;
  return count;

fail:
  for (int j = 0; j < i; j++) {
    free((void *)segs[j].text);
    free((void *)segs[j].param);
  }
  free(segs);
  return -1;
}

void xHttpRouteFreeSegments_(struct xHttpRouteSegment_ *segs, int count) {
  if (!segs) return;
  for (int i = 0; i < count; i++) {
    free((void *)segs[i].text);
    free((void *)segs[i].param);
  }
  free(segs);
}

int xHttpRouteMatch_(const struct xHttpRouteSegment_ *segments, int segment_count, const char *url,
                     struct xHttpParam_ *params, int *param_count) {
  *param_count = 0;

  const char *p       = url;
  int         seg_idx = 0;

  while (*p == '/')
    p++;

  for (seg_idx = 0; seg_idx < segment_count; seg_idx++) {
    if (*p == '\0') return 0;

    const char *seg_start = p;
    while (*p && *p != '/' && *p != '?')
      p++;
    size_t seg_len = (size_t)(p - seg_start);

    const struct xHttpRouteSegment_ *rs = &segments[seg_idx];
    if (rs->param) {
      if (seg_len == 0) return 0;
      if (*param_count >= XHTTP_MAX_PARAMS) return 0;
      params[*param_count].name      = rs->param;
      params[*param_count].value     = seg_start;
      params[*param_count].value_len = seg_len;
      (*param_count)++;
    } else {
      if (seg_len != strlen(rs->text) || memcmp(seg_start, rs->text, seg_len) != 0) {
        return 0;
      }
    }

    while (*p == '/')
      p++;
  }

  if (*p != '\0' && *p != '?') return 0;

  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  xHttpMux (built-in router)
 * ═══════════════════════════════════════════════════════════════════════════
 */

xHttpMux xHttpMuxCreate(void) {
  struct xHttpMux_ *m = (struct xHttpMux_ *)calloc(1, sizeof(*m));
  return (xHttpMux)m;
}

void xHttpMuxDestroy(xHttpMux mux) {
  if (!mux) return;
  struct xHttpMux_      *m = (struct xHttpMux_ *)mux;
  struct xHttpMuxRoute_ *r = m->routes;
  while (r) {
    struct xHttpMuxRoute_ *next = r->next;
    free((void *)r->method);
    free((void *)r->path);
    xHttpRouteFreeSegments_(r->segments, r->segment_count);
    free(r);
    r = next;
  }
  free(m);
}

xErrno xHttpMuxHandle(xHttpMux mux, const xHttpRouteConf *conf) {
  if (!mux || !conf || !conf->pattern) return xErrno_InvalidArg;
  struct xHttpMux_ *m = (struct xHttpMux_ *)mux;

  /* Parse pattern: "METHOD /path" or "/path" */
  const char *method_str = NULL;
  const char *path       = conf->pattern;

  if (conf->pattern[0] != '/') {
    const char *space = strchr(conf->pattern, ' ');
    if (!space || space[1] != '/') return xErrno_InvalidArg;
    size_t method_len = (size_t)(space - conf->pattern);
    char  *mt         = (char *)malloc(method_len + 1);
    if (!mt) return xErrno_NoMemory;
    memcpy(mt, conf->pattern, method_len);
    mt[method_len] = '\0';
    method_str     = mt;
    path           = space + 1;
  }

  struct xHttpMuxRoute_ *route = (struct xHttpMuxRoute_ *)calloc(1, sizeof(*route));
  if (!route) {
    free((void *)method_str);
    return xErrno_NoMemory;
  }

  route->method          = method_str;
  route->path            = strdup(path);
  route->info.on_request = conf->on_request;
  route->info.on_data    = conf->on_data;
  route->info.on_done    = conf->on_done;
  route->info.arg        = conf->arg;

  if (!route->path) {
    free((void *)route->method);
    free(route);
    return xErrno_NoMemory;
  }

  int seg_count = xHttpRouteParseSegments_(path, &route->segments);
  if (seg_count < 0) {
    free((void *)route->method);
    free((void *)route->path);
    free(route);
    return xErrno_NoMemory;
  }
  route->segment_count = seg_count;

  if (m->routes_tail) {
    m->routes_tail->next = route;
  } else {
    m->routes = route;
  }
  m->routes_tail = route;

  return xErrno_Ok;
}

const xHttpRouteInfo *xHttpMuxResolve(void *router, xHttpCtx *ctx) {
  struct xHttpMux_ *m = (struct xHttpMux_ *)router;
  if (!m || !ctx || !ctx->internal_) return NULL;

  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;

  int                    path_matched = 0;
  struct xHttpMuxRoute_ *r            = m->routes;
  struct xHttpParam_     params[XHTTP_MAX_PARAMS + 1];
  int                    param_count = 0;

  while (r) {
    if (xHttpRouteMatch_(r->segments, r->segment_count, ctx->url, params, &param_count)) {
      path_matched = 1;
      if (!r->method || strcasecmp(r->method, ctx->method) == 0) {
        /* Copy matched params to stream */
        for (int i = 0; i < param_count; i++) {
          stream->params[i] = params[i];
        }
        stream->params[param_count].name      = NULL;
        stream->params[param_count].value     = NULL;
        stream->params[param_count].value_len = 0;
        stream->param_count                   = param_count;
        return &r->info;
      }
    }
    r = r->next;
  }

  /* Path matched but no route's method matched → 405 Method Not Allowed.
   * Signal via the stream's pending_error so the server dispatches a 405
   * instead of the default 404. */
  if (path_matched) {
    stream->pending_error        = 405;
    stream->pending_error_reason = "Method Not Allowed";
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Server lifecycle
 * ═══════════════════════════════════════════════════════════════════════════
 */

xHttpServer xHttpServerCreate(const xHttpServerConf *conf) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop) return NULL;

  signal(SIGPIPE, SIG_IGN);

  struct xHttpServer_ *s = (struct xHttpServer_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->loop            = loop;
  s->listen_sock     = NULL;
  s->listen_fd       = -1;
  s->tls_listen_sock = NULL;
  s->tls_listen_fd   = -1;
  s->tls_ctx         = NULL;
  s->conns           = NULL;
  s->ws_conns        = NULL;

  /* Apply configuration */
  if (conf) {
    s->resolve = conf->resolve;
    s->router  = conf->router;
    s->idle_timeout_ms =
      conf->idle_timeout_ms > 0 ? conf->idle_timeout_ms : XHTTP_DEFAULT_IDLE_TIMEOUT_MS;
    s->max_header_size =
      conf->max_header_size > 0 ? conf->max_header_size : XHTTP_DEFAULT_MAX_HEADER_SIZE;
  } else {
    s->resolve         = NULL;
    s->router          = NULL;
    s->idle_timeout_ms = XHTTP_DEFAULT_IDLE_TIMEOUT_MS;
    s->max_header_size = XHTTP_DEFAULT_MAX_HEADER_SIZE;
  }

  return (xHttpServer)s;
}

xErrno xHttpServerListen(xHttpServer server, const char *host, uint16_t port) {
  if (!server) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return xErrno_SysError;

  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  if (host) {
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
      close(fd);
      return xErrno_InvalidArg;
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return xErrno_SysError;
  }

  if (listen(fd, SOMAXCONN) < 0) {
    close(fd);
    return xErrno_SysError;
  }

  xSocket sock = xSocketCreateFromFd(fd, xEvent_Read, on_listen_event, s);
  if (!sock) {
    close(fd);
    return xErrno_SysError;
  }

  s->listen_sock = sock;
  s->listen_fd   = fd;

  return xErrno_Ok;
}

void xHttpServerDestroy(xHttpServer server) {
  if (!server) return;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  /* Close all active WebSocket connections. xWsConnClose/Destroy are
   * declared in ws_private.h (included via server_private.h). */
  while (s->ws_conns) {
    xWsConnClose(s->ws_conns, 1001, NULL, 0);
    xWsConnDestroy(s->ws_conns);
  }

  /* Close all active connections */
  while (s->conns) {
    xHttpConnClose(s->conns);
  }

  if (s->listen_sock) {
    xSocketDestroy(s->listen_sock);
    s->listen_sock = NULL;
    s->listen_fd   = -1;
  }

  if (s->tls_listen_sock) {
    xSocketDestroy(s->tls_listen_sock);
    s->tls_listen_sock = NULL;
    s->tls_listen_fd   = -1;
  }

  if (s->tls_ctx) {
    xTlsCtxDestroy(s->tls_ctx);
    s->tls_ctx = NULL;
  }

  /* Free auxiliary data */
  if (s->aux_free) {
    s->aux_free(s->aux_data);
  }

  free(s);
}

/* ── Configuration ─────────────────────────────────────────────────────── */

xErrno xHttpServerSetMaxHeaderSize(xHttpServer server, size_t max_size) {
  if (!server) return xErrno_InvalidArg;
  if (max_size == 0) return xErrno_InvalidArg;
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->max_header_size     = max_size;
  return xErrno_Ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection accept & management
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void on_listen_event(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;
  struct xHttpServer_ *s = (struct xHttpServer_ *)arg;

  if (!(mask & xEvent_Read)) return;

  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t          addr_len  = sizeof(client_addr);
    int                client_fd = accept(s->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        xLog(false, "xhttp: accept() failed: %s (fd exhaustion)", strerror(errno));
        break;
      }
      break;
    }

    struct xHttpConn_ *conn = (struct xHttpConn_ *)calloc(1, sizeof(struct xHttpConn_));
    if (!conn) {
      close(client_fd);
      continue;
    }

    conn->server = s;
    xIOBufferInit(&conn->read_buf);
    xIOBufferInit(&conn->write_buf);
    conn->keep_alive     = 1;
    conn->writing        = 0;
    conn->handshake_done = 1;

    xTransportPlainInit(&conn->transport, client_fd);
    conn_init_parser(conn);

    xSocket client_sock = xSocketCreateFromFd(client_fd, xEvent_Read, on_conn_event, conn);
    if (!client_sock) {
      xIOBufferDeinit(&conn->read_buf);
      xIOBufferDeinit(&conn->write_buf);
      free(conn);
      close(client_fd);
      continue;
    }
    conn->sock = client_sock;

    if (s->idle_timeout_ms > 0) {
      xSocketSetTimeout(conn->sock, s->idle_timeout_ms, 0);
    }

    conn->prev = NULL;
    conn->next = s->conns;
    if (s->conns) s->conns->prev = conn;
    s->conns = conn;
  }
}

struct xHttpStream_ *xHttpStreamCreate(struct xHttpConn_ *conn, int32_t stream_id) {
  struct xHttpStream_ *stream = (struct xHttpStream_ *)calloc(1, sizeof(struct xHttpStream_));
  if (!stream) return NULL;

  stream->conn      = conn;
  stream->stream_id = stream_id;

  stream->writer.status_code  = 200;
  stream->writer.headers      = NULL;
  stream->writer.headers_tail = NULL;
  stream->writer.sent         = 0;
  stream->writer.streaming    = 0;
  stream->writer.stream       = stream;

  return stream;
}

void xHttpStreamDestroy(struct xHttpStream_ *stream) {
  if (!stream) return;

  xBufferDestroy(stream->url);
  xBufferDestroy(stream->header_field);
  xBufferDestroy(stream->headers_raw);

  struct xHttpHeader_ *h = stream->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }

  free(stream);
}

void xHttpStreamReset(struct xHttpStream_ *stream) {
  if (!stream) return;

  xBufferReset(stream->url);
  xBufferReset(stream->header_field);
  xBufferReset(stream->headers_raw);

  stream->header_bytes         = 0;
  stream->request_complete     = 0;
  stream->pending_error        = 0;
  stream->pending_error_reason = NULL;
  stream->route_info           = NULL;
  stream->on_request_done      = 0;
  stream->request_aborted      = 0;
  stream->param_count          = 0;
  stream->params[0].name       = NULL;

  struct xHttpHeader_ *h = stream->writer.headers;
  while (h) {
    struct xHttpHeader_ *next = h->next;
    free(h->key);
    free(h->value);
    free(h);
    h = next;
  }
  stream->writer.status_code  = 200;
  stream->writer.headers      = NULL;
  stream->writer.headers_tail = NULL;
  stream->writer.sent         = 0;
  stream->writer.streaming    = 0;
}

static void conn_init_parser(struct xHttpConn_ *conn) {
  conn->proto_detected = 0;
  memset(&conn->proto, 0, sizeof(conn->proto));
}

static void conn_reset_request_state(struct xHttpConn_ *conn) {
  if (conn->stream) {
    xHttpStreamReset(conn->stream);
  }
  if (conn->proto.reset) {
    conn->proto.reset(conn);
  }
}

void xHttpConnResetParser(struct xHttpConn_ *conn) {
  conn_reset_request_state(conn);
}

void xHttpConnClose(struct xHttpConn_ *conn) {
  if (!conn) return;
  if (conn->hijacked) return;
  struct xHttpServer_ *s = conn->server;

  if (conn->prev)
    conn->prev->next = conn->next;
  else
    s->conns = conn->next;
  if (conn->next) conn->next->prev = conn->prev;

  if (conn->transport.destroy) {
    conn->transport.destroy(conn->transport.ctx);
    conn->transport.ctx = NULL;
  }

  if (conn->sock) {
    xSocketDestroy(conn->sock);
    conn->sock = NULL;
  }

  xIOBufferDeinit(&conn->read_buf);
  xIOBufferDeinit(&conn->write_buf);

  if (conn->proto_detected && conn->proto.destroy) {
    conn->proto.destroy(conn);
  }

  if (conn->stream) {
    xHttpStreamDestroy(conn->stream);
    conn->stream = NULL;
  }

  free(conn);
}

void xHttpConnHijack(struct xHttpConn_ *conn) {
  if (!conn) return;
  struct xHttpServer_ *s = conn->server;

  conn->hijacked = 1;

  if (conn->prev)
    conn->prev->next = conn->next;
  else
    s->conns = conn->next;
  if (conn->next) conn->next->prev = conn->prev;
  conn->prev = NULL;
  conn->next = NULL;

  if (conn->proto_detected && conn->proto.destroy) {
    conn->proto.destroy(conn);
    conn->proto.state = NULL;
  }

  if (conn->stream) {
    xHttpStreamDestroy(conn->stream);
    conn->stream = NULL;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Connection I/O event handler
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void on_conn_event(xSocket sock, xEventMask mask, void *arg) {
  struct xHttpConn_ *conn = (struct xHttpConn_ *)arg;
  (void)sock;

  if (conn->hijacked) return;

  if (mask & xEvent_Timeout) {
    xHttpConnClose(conn);
    return;
  }

  if (mask & xEvent_Write) {
    /* conn_write_ready may close the connection (e.g. when keep_alive=0
     * and the write buffer finishes draining). If it did, conn is freed
     * and we must not touch it — bail out before the Read/handshake paths
     * below turn this into a use-after-free. */
    if (conn_write_ready(conn)) return;
  }

  if (!conn->handshake_done && conn->transport.handshake) {
    int hs = conn->transport.handshake(conn->transport.ctx);
    switch (hs) {
    case xTransportResult_Done:
      conn->handshake_done = 1;
      break;
    case xTransportResult_WantRead:
      xSocketSetMask(conn->sock, xEvent_Read);
      return;
    case xTransportResult_WantWrite:
      xSocketSetMask(conn->sock, xEvent_Read | xEvent_Write);
      return;
    case xTransportResult_Error:
    default:
      xLog(false, "xhttp: TLS handshake failed");
      xHttpConnClose(conn);
      return;
    }
  }

  if (mask & xEvent_Read) {
    for (;;) {
      ssize_t n = xIOBufferReadWith(&conn->read_buf, conn->transport.read, conn->transport.ctx);
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        xHttpConnClose(conn);
        return;
      }
      if (n == 0) {
        xHttpConnClose(conn);
        return;
      }

      size_t buf_len = xIOBufferLen(&conn->read_buf);
      if (buf_len == 0) {
        return;
      }

      /* Protocol auto-detection */
      if (!conn->proto_detected) {
        if (conn->transport.alpn) {
          const char *alpn = conn->transport.alpn(conn->transport.ctx);
          if (alpn && strcmp(alpn, "h2") == 0) {
            xHttpProtoH2Init(conn);
          } else {
            xHttpProtoH1Init(conn);
          }
          conn->proto_detected = 1;
        } else {
          static const char   h2_magic[]   = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
          static const size_t h2_magic_len = 24;

          char *peek = (char *)malloc(buf_len);
          if (!peek) {
            xHttpConnClose(conn);
            return;
          }
          xIOBufferCopyTo(&conn->read_buf, peek);

          if (buf_len >= h2_magic_len) {
            if (memcmp(peek, h2_magic, h2_magic_len) == 0) {
              xHttpProtoH2Init(conn);
            } else {
              xHttpProtoH1Init(conn);
            }
            conn->proto_detected = 1;
          } else {
            if (memcmp(peek, h2_magic, buf_len) == 0) {
              free(peek);
              return;
            } else {
              xHttpProtoH1Init(conn);
              conn->proto_detected = 1;
            }
          }
          free(peek);
        }
      }

      buf_len      = xIOBufferLen(&conn->read_buf);
      char *linear = (char *)malloc(buf_len);
      if (!linear) {
        xHttpConnSendError(conn, 500, "Internal Server Error");
        conn_after_response(conn);
        return;
      }
      xIOBufferCopyTo(&conn->read_buf, linear);
      xIOBufferConsume(&conn->read_buf, buf_len);

      int rc = conn->proto.on_data(conn, linear, buf_len);
      free(linear);

      if (rc < 0) {
        if (conn->stream && conn->stream->pending_error) {
          int         code                   = conn->stream->pending_error;
          const char *reason                 = conn->stream->pending_error_reason;
          conn->stream->pending_error        = 0;
          conn->stream->pending_error_reason = NULL;
          xHttpConnSendError(conn, code, reason);
        } else {
          xHttpConnSendError(conn, 400, "Bad Request");
        }
        conn_after_response(conn);
        return;
      }

      if (rc > 0) {
        conn->stream->request_complete = 0;
        conn_dispatch_request(conn);
        return;
      }

      if (n < 0) {
        return;
      }
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Route resolution
 * ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * Resolve the route for a stream after headers are complete.
 * Builds the xHttpCtx from stream state, calls the server's resolver,
 * and stores the route_info on the stream. If no route matches, sets
 * pending_error to 404.
 */
void xHttpStreamResolve(struct xHttpStream_ *stream) {
  struct xHttpConn_   *conn = stream->conn;
  struct xHttpServer_ *s    = conn->server;

  /* NUL-terminate buffers for C string usage */
  if (stream->url) xBufferAppend(&stream->url, "", 1);
  if (stream->headers_raw) xBufferAppend(&stream->headers_raw, "", 1);

  /* Build the per-request xHttpCtx */
  memset(&stream->ctx, 0, sizeof(stream->ctx));
  stream->ctx.method  = conn->proto.method(stream);
  stream->ctx.url     = stream->url ? (const char *)xBufferData(stream->url) : "/";
  stream->ctx.headers = stream->headers_raw ? (const char *)xBufferData(stream->headers_raw) : "";
  stream->ctx.headers_len = stream->headers_raw ? xBufferLen(stream->headers_raw) - 1 : 0;
  stream->ctx.internal_   = stream;

  /* Call the resolver */
  if (s->resolve) {
    stream->route_info = s->resolve(s->router, &stream->ctx);
  } else {
    stream->route_info = NULL;
  }

  if (!stream->route_info) {
    /* Resolver may have already set a more specific error (e.g. 405). */
    if (!stream->pending_error) {
      stream->pending_error        = 404;
      stream->pending_error_reason = "Not Found";
    }
    return;
  }

  /* Call on_request if present */
  if (stream->route_info->on_request) {
    stream->on_request_done = 1;
    int rc                  = stream->route_info->on_request(&stream->ctx, stream->route_info->arg);
    if (rc != 0) {
      stream->request_aborted = 1;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Request dispatch
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void conn_dispatch_request(struct xHttpConn_ *conn) {
  struct xHttpStream_ *stream = conn->stream;

  /* If there was a pending error (404, 413, etc.), send it */
  if (stream->pending_error) {
    int         code             = stream->pending_error;
    const char *reason           = stream->pending_error_reason;
    stream->pending_error        = 0;
    stream->pending_error_reason = NULL;
    xHttpConnSendError(conn, code, reason);
    conn_after_response(conn);
    return;
  }

  /* If on_request aborted, send 500 if nothing was sent */
  if (stream->request_aborted) {
    if (!stream->writer.sent && !stream->writer.streaming) {
      xHttpConnSendError(conn, 500, "Internal Server Error");
    }
    conn_after_response(conn);
    return;
  }

  /* Call on_done if present.
   *
   * on_done may call xHttpCtxSend / xHttpCtxEndStream, which internally
   * call conn_after_response.  If the connection is not keep-alive,
   * conn_after_response will close and free conn.  Save the hijacked
   * flag BEFORE calling on_done so we can safely clean up a hijacked
   * connection without touching freed memory. */
  if (stream->route_info && stream->route_info->on_done) {
    int hijacked = conn->hijacked;
    stream->route_info->on_done(&stream->ctx, stream->route_info->arg);

    /* If the handler hijacked the connection (e.g. WebSocket upgrade
     * in on_request), clean up and return.  conn is still valid here
     * because hijacked connections don't call conn_after_response. */
    if (hijacked) {
      xIOBufferDeinit(&conn->read_buf);
      xIOBufferDeinit(&conn->write_buf);
      free(conn);
    }
    /* If not hijacked, conn may have been freed by conn_after_response
     * (called from xHttpCtxSend/xHttpCtxEndStream).  Don't touch it. */
    return;
  }

  /* No on_done handler — check hijacked directly. */
  if (conn->hijacked) {
    xIOBufferDeinit(&conn->read_buf);
    xIOBufferDeinit(&conn->write_buf);
    free(conn);
    return;
  }

  /* If handler already sent (xHttpCtxSend finalizes internally) or
   * is streaming (waits for xHttpCtxEndStream), nothing to do.
   * If nothing was sent, the connection stays open — the handler
   * may send a response later from a callback. */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  xHttpCtx write functions
 * ═══════════════════════════════════════════════════════════════════════════
 */

void xHttpCtxSetStatus(xHttpCtx *ctx, int code) {
  if (!ctx) return;
  ctx->status_code            = code;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;
  if (stream) stream->writer.status_code = code;
}

xErrno xHttpCtxSetHeader(xHttpCtx *ctx, const char *key, const char *value) {
  if (!ctx || !key || !value) return xErrno_InvalidArg;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;
  if (!stream) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = &stream->writer;

  struct xHttpHeader_ *h = (struct xHttpHeader_ *)calloc(1, sizeof(struct xHttpHeader_));
  if (!h) return xErrno_NoMemory;

  h->key   = strdup(key);
  h->value = strdup(value);
  h->next  = NULL;

  if (!h->key || !h->value) {
    free(h->key);
    free(h->value);
    free(h);
    return xErrno_NoMemory;
  }

  if (w->headers_tail) {
    w->headers_tail->next = h;
  } else {
    w->headers = h;
  }
  w->headers_tail = h;

  return xErrno_Ok;
}

xErrno xHttpCtxSend(xHttpCtx *ctx, const char *body, size_t body_len) {
  if (!ctx) return xErrno_InvalidArg;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;
  if (!stream) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = &stream->writer;

  if (w->sent || w->streaming) return xErrno_InvalidState;
  w->sent = 1;

  struct xHttpConn_ *conn = stream->conn;
  conn->proto.send_response(stream, w->status_code, w->headers, body, body_len);
  conn_try_flush(conn);
  conn_after_response(conn);

  return xErrno_Ok;
}

xErrno xHttpCtxWrite(xHttpCtx *ctx, const char *data, size_t len) {
  if (!ctx) return xErrno_InvalidArg;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;
  if (!stream) return xErrno_InvalidArg;
  struct xHttpResponseWriter_ *w = &stream->writer;

  if (w->sent) return xErrno_InvalidState;

  struct xHttpConn_ *conn = stream->conn;
  conn->proto.write_data(stream, data, len);
  conn_try_flush(conn);

  return xErrno_Ok;
}

xErrno xHttpCtxEndStream(xHttpCtx *ctx) {
  if (!ctx || !ctx->internal_) return xErrno_InvalidArg;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;
  struct xHttpConn_   *conn   = stream->conn;

  if (stream->writer.streaming && !stream->writer.sent) {
    conn->proto.end_stream(stream);
    conn_try_flush(conn);
  }
  conn_after_response(conn);
  return xErrno_Ok;
}

const char *xHttpCtxParam(xHttpCtx *ctx, const char *name, size_t *len) {
  if (!ctx || !name || !ctx->internal_) return NULL;
  struct xHttpStream_ *stream = (struct xHttpStream_ *)ctx->internal_;

  for (int i = 0; i < stream->param_count; i++) {
    if (stream->params[i].name && strcmp(stream->params[i].name, name) == 0) {
      if (len) *len = stream->params[i].value_len;
      return stream->params[i].value;
    }
  }
  return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Response flushing & lifecycle
 * ═══════════════════════════════════════════════════════════════════════════
 */

static void conn_try_flush(struct xHttpConn_ *conn) {
  /* Loop until EAGAIN or buffer empty — ensures we drain as much as
   * possible in one call.  Without the loop, a partial writev (common
   * for large responses) leaves data in the buffer, and edge-triggered
   * epoll may not fire again if the socket is still technically writable. */
  while (!xIOBufferEmpty(&conn->write_buf)) {
    struct iovec iov[XHTTP_MAX_IOV];
    int          cnt = xIOBufferReadIov(&conn->write_buf, iov, XHTTP_MAX_IOV);
    if (cnt == 0) break;

    ssize_t n = conn->transport.writev(conn->transport.ctx, iov, cnt);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (!conn->writing) {
          conn->writing = 1;
          xSocketSetMask(conn->sock, xEvent_Read | xEvent_Write);
        }
        return;
      }
      conn->keep_alive = 0;
      return;
    }
    if (n > 0) xIOBufferConsume(&conn->write_buf, (size_t)n);
    if (n == 0) break;
  }

  /* Buffer fully drained */
  if (xIOBufferEmpty(&conn->write_buf)) {
    if (conn->writing) {
      conn->writing = 0;
      xSocketSetMask(conn->sock, xEvent_Read);
    }
  } else if (!conn->writing) {
    conn->writing = 1;
    xSocketSetMask(conn->sock, xEvent_Read | xEvent_Write);
  }
}

void xHttpConnSendError(struct xHttpConn_ *conn, int status_code, const char *reason) {
  struct xHttpStream_ *stream = conn->stream;

  /* Callers can reach us on parse-error / OOM paths before a stream has
   * been created (e.g. proto init failed, or the very first request chunk
   * failed to allocate). Without a stream there is no writer state to
   * build a response with, so just mark the connection for close and let
   * the caller's lifecycle path tear it down. */
  if (!stream) {
    conn->keep_alive = 0;
    return;
  }

  if (stream->writer.sent) {
    conn->keep_alive = 0;
    return;
  }

  char body[256];
  int  body_len = snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1></body></html>\r\n",
                           status_code, reason);

  stream->writer.status_code = status_code;

  /* H1: close after error; H2: only the stream closes */
  if (!stream->stream_id) {
    conn->keep_alive = 0;
  }

  /* Add Content-Type header */
  struct xHttpHeader_ *h = (struct xHttpHeader_ *)calloc(1, sizeof(struct xHttpHeader_));
  if (h) {
    h->key   = strdup("Content-Type");
    h->value = strdup("text/html");
    h->next  = NULL;
    if (stream->writer.headers_tail) {
      stream->writer.headers_tail->next = h;
    } else {
      stream->writer.headers = h;
    }
    stream->writer.headers_tail = h;
  }

  stream->writer.sent = 1;
  conn->proto.send_response(stream, stream->writer.status_code, stream->writer.headers, body,
                            (size_t)body_len);
  conn_try_flush(conn);
}

int xHttpConnFlushWriteInternal(struct xHttpConn_ *conn) {
  conn_try_flush(conn);

  if (xIOBufferEmpty(&conn->write_buf)) {
    if (!conn->keep_alive) {
      xHttpConnClose(conn);
      return 1;
    } else {
      conn_reset_request_state(conn);
      if (conn->server->idle_timeout_ms > 0) {
        xSocketSetTimeout(conn->sock, conn->server->idle_timeout_ms, 0);
      }
    }
  }
  return 0;
}

void xHttpConnFlushWrite(struct xHttpConn_ *conn) {
  xHttpConnFlushWriteInternal(conn);
}

static int conn_write_ready(struct xHttpConn_ *conn) {
  /* Returns 1 if the connection was closed (caller must stop touching conn),
   * 0 otherwise. */
  return xHttpConnFlushWriteInternal(conn);
}

static void conn_after_response(struct xHttpConn_ *conn) {
  /* H2 connections: stream lifecycle is managed by nghttp2 callbacks */
  if (conn->proto_detected && conn->proto.reset != NULL) {
    if (conn->proto.should_keep_alive && conn->proto.should_keep_alive(conn) && conn->keep_alive) {
      if (conn->stream && conn->stream->stream_id > 0) {
        if (conn->stream->closed_by_peer) {
          xHttpStreamDestroy(conn->stream);
          conn->stream = NULL;
        }
        return;
      }
    }
  }

  if (xIOBufferEmpty(&conn->write_buf)) {
    if (!conn->keep_alive) {
      xHttpConnClose(conn);
    } else {
      conn_reset_request_state(conn);
      if (conn->server->idle_timeout_ms > 0) {
        xSocketSetTimeout(conn->sock, conn->server->idle_timeout_ms, 0);
      }
    }
  } else if (!conn->writing) {
    /* Flush may close the connection (keep_alive=0 + buffer drained).
     * If it did, return immediately rather than relying on the caller
     * doing nothing afterwards — future code added below this branch
     * would otherwise risk a use-after-free. */
    if (xHttpConnFlushWriteInternal(conn)) return;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public wrappers
 * ═══════════════════════════════════════════════════════════════════════════
 */

void xHttpConnDispatchRequest(struct xHttpConn_ *conn) {
  conn_dispatch_request(conn);
}

void xHttpConnTryFlush(struct xHttpConn_ *conn) {
  conn_try_flush(conn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  TLS support
 * ═══════════════════════════════════════════════════════════════════════════
 */

#if defined(X_HAS_OPENSSL) || defined(X_HAS_MBEDTLS)
static void on_tls_listen_event(xSocket sock, xEventMask mask, void *arg);
#endif

xErrno xHttpServerListenTls(xHttpServer server, const char *host, uint16_t port,
                            const xTlsConf *config) {
  if (!server) return xErrno_InvalidArg;
  if (!config) return xErrno_InvalidArg;
  if (!config->cert || !config->key) return xErrno_InvalidArg;

#if !defined(X_HAS_OPENSSL) && !defined(X_HAS_MBEDTLS)
  (void)host;
  (void)port;
  return xErrno_NotSupported;
#else
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;

  static const char *http_alpn[] = {"h2", "http/1.1", NULL};
  xTlsConf           tls_conf    = *config;
  if (!tls_conf.alpn) tls_conf.alpn = http_alpn;
  xTlsCtx tls_ctx = xTlsCtxCreate(&tls_conf);
  if (!tls_ctx) return xErrno_SysError;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    xTlsCtxDestroy(tls_ctx);
    return xErrno_SysError;
  }

  int optval = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);

  if (host) {
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
      close(fd);
      xTlsCtxDestroy(tls_ctx);
      return xErrno_InvalidArg;
    }
  } else {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    xTlsCtxDestroy(tls_ctx);
    return xErrno_SysError;
  }

  if (listen(fd, SOMAXCONN) < 0) {
    close(fd);
    xTlsCtxDestroy(tls_ctx);
    return xErrno_SysError;
  }

  xSocket sock = xSocketCreateFromFd(fd, xEvent_Read, on_tls_listen_event, s);
  if (!sock) {
    close(fd);
    xTlsCtxDestroy(tls_ctx);
    return xErrno_SysError;
  }

  s->tls_listen_sock = sock;
  s->tls_listen_fd   = fd;
  s->tls_ctx         = tls_ctx;

  return xErrno_Ok;
#endif /* X_HAS_OPENSSL || X_HAS_MBEDTLS */
}

#if defined(X_HAS_OPENSSL) || defined(X_HAS_MBEDTLS)
static void on_tls_listen_event(xSocket sock, xEventMask mask, void *arg) {
  (void)sock;

  if (!(mask & xEvent_Read)) return;

  struct xHttpServer_ *s = (struct xHttpServer_ *)arg;
  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t          addr_len = sizeof(client_addr);
    int client_fd = accept(s->tls_listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EMFILE || errno == ENFILE) {
        xLog(false, "xhttp: TLS accept() failed: %s (fd exhaustion)", strerror(errno));
        break;
      }
      break;
    }

    struct xHttpConn_ *conn = (struct xHttpConn_ *)calloc(1, sizeof(struct xHttpConn_));
    if (!conn) {
      close(client_fd);
      continue;
    }

    conn->server = s;
    xIOBufferInit(&conn->read_buf);
    xIOBufferInit(&conn->write_buf);
    conn->keep_alive     = 1;
    conn->writing        = 0;
    conn->handshake_done = 0;

    xTransportTlsServerInit(&conn->transport, s->tls_ctx, client_fd);
    conn_init_parser(conn);

    xSocket client_sock = xSocketCreateFromFd(client_fd, xEvent_Read, on_conn_event, conn);
    if (!client_sock) {
      if (conn->transport.destroy) {
        conn->transport.destroy(conn->transport.ctx);
      }
      xIOBufferDeinit(&conn->read_buf);
      xIOBufferDeinit(&conn->write_buf);
      free(conn);
      close(client_fd);
      continue;
    }
    conn->sock = client_sock;

    if (s->idle_timeout_ms > 0) {
      xSocketSetTimeout(conn->sock, s->idle_timeout_ms, 0);
    }

    conn->prev = NULL;
    conn->next = s->conns;
    if (s->conns) s->conns->prev = conn;
    s->conns = conn;
  }
}
#endif /* X_HAS_OPENSSL || X_HAS_MBEDTLS */
