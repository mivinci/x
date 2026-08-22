/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client.c - Asynchronous HTTP client: libcurl multi-socket + xEventLoop
 */

#include "client_private.h"

#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ──────────────────────────────────────────────── */

static void check_multi_info(struct xHttpClient_ *c);
static void destroy_req(struct xHttpClient_ *c, CURL *easy, struct xHttpReq_ *req, int notify);
static void on_read_timeout(void *arg);
static int  socket_callback(CURL *easy, curl_socket_t fd, int what, void *userp, void *socketp);
static int  timer_callback(CURLM *multi, long timeout_ms, void *userp);
static void fd_ready_callback(int fd, xEventMask mask, void *arg);
static void on_timeout(void *arg);

/* ── HTTP version helper ───────────────────────────────────────────────── */

static void apply_http_version(CURL *easy, xHttpVersion ver) {
  switch (ver) {
  case xHttpVersion_H1:
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    break;
  case xHttpVersion_H2:
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);
    break;
  case xHttpVersion_H2TLS:
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    break;
  case xHttpVersion_H2C:
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    break;
  default:
    break; /* xHttpVersion_Default — use libcurl default */
  }
}

/* ── Vtable for oneshot HTTP requests ──────────────────────────────────── */

static void oneshot_on_done(struct xHttpReq_ *req, CURLcode result);
static void oneshot_on_cleanup(struct xHttpReq_ *req);

static const struct xHttpReqVtable oneshot_vtable = {
  .on_done    = oneshot_on_done,
  .on_cleanup = oneshot_on_cleanup,
};

/* ── curl data callbacks ───────────────────────────────────────────────── */

/* Build the xHttpCtx for callback delivery. Idempotent — safe to call
 * multiple times (e.g. once for on_response, once for on_done). */
static void req_build_ctx(struct xHttpReq_ *req, CURLcode result) {
  xHttpCtx *ctx = &req->ctx;

  /* Ensure headers are NUL-terminated */
  xBufferAppend(&req->header_buf, "\0", 1);
  const char *header_data = (const char *)xBufferData(req->header_buf);
  size_t      header_len  = xBufferLen(req->header_buf);
  /* Strip trailing NUL(s) added by repeated calls */
  while (header_len > 0 && header_data[header_len - 1] == '\0')
    header_len--;

  ctx->method = NULL;
  /* Effective URL after redirects (client side): curl reports the URL the
   * request ultimately used. For non-redirected requests this equals the
   * request URL. NULL before the URL is known. */
  const char *effective = NULL;
  curl_easy_getinfo(req->easy, CURLINFO_EFFECTIVE_URL, &effective);
  ctx->url         = effective;
  ctx->headers     = header_data ? header_data : "";
  ctx->headers_len = header_len;
  ctx->internal_   = NULL;
  ctx->curl_code   = (int)result;

  /* Always try to get the response code — the server may have sent a
   * valid response (e.g. 413) before closing the connection while the
   * client was still uploading.  In that case curl reports an error
   * (CURLE_SEND_ERROR / CURLE_RECV_ERROR) but the response code is
   * still available. */
  long code = 0;
  curl_easy_getinfo(req->easy, CURLINFO_RESPONSE_CODE, &code);
  ctx->status_code = code;

  if (result == CURLE_OK) {
    ctx->curl_error = NULL;
  } else {
    ctx->status_code = 0;
    ctx->curl_error  = req->errbuf[0] ? req->errbuf : curl_easy_strerror(result);
  }
}

/* Call on_response once, on first body chunk or at completion.
 * Returns 0 on success, non-zero on abort. */
static int req_maybe_call_on_response(struct xHttpReq_ *req) {
  if (req->headers_done) return 0;
  req->headers_done = 1;
  if (req->on_response) {
    req_build_ctx(req, CURLE_OK);
    return req->on_response(&req->ctx, req->arg);
  }
  return 0;
}

/* Write callback — handles both streaming (on_data) and discard modes.
 * Triggers on_response on the first body chunk. */
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  struct xHttpReq_ *req   = (struct xHttpReq_ *)userdata;
  size_t            total = size * nmemb;
  XDEBUGL1("curl: write_cb len=%zu req=%p", total, (void *)req);
  /* Trigger on_response on first body chunk (if not yet called) */
  if (req_maybe_call_on_response(req) != 0) return 0; /* abort */

  /* Read timeout: each body chunk proves the peer is alive — reset the
   * idle timer. First chunk arms it (headers arrived, body pending). */
  if (req->read_timeout_ms > 0) {
    if (req->read_timer) {
      xTimerStop(req->read_timer);
      req->read_timer = NULL;
    }
    req->read_timer = xTimerStart(on_read_timeout, req, NULL, (uint64_t)req->read_timeout_ms, 0);
  }
  /* Deliver body via on_data, or discard if NULL */
  if (req->on_data) {
    int r = req->on_data(ptr, total, req->arg);
    if (r < 0) return 0; /* abort */
    if (r > 0) {
      /* Pause (backpressure): buffer this chunk and stop delivering.
       * curl treats a write_cb return < total as abort, so we must return
       * total after taking ownership of the data. The chunk is re-delivered
       * to on_data by xHttpClientResume(). */
      if (xBufferAppend(&req->paused_buf, ptr, total) != xErrno_Ok) return 0;
      if (curl_easy_pause(req->easy, CURLPAUSE_RECV) != CURLE_OK) return 0;
      req->paused = 1;
    }
  }
  return total;
}

/* Header callback — always buffers into header_buf. */
static size_t header_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  struct xHttpReq_ *req   = (struct xHttpReq_ *)userdata;
  size_t            total = size * nmemb;
  if (xBufferAppend(&req->header_buf, ptr, total) != xErrno_Ok) return 0;
  return total;
}

/* Read callback — delegates to user's on_read for streaming uploads */
static size_t read_callback(char *buf, size_t size, size_t nmemb, void *userdata) {
  struct xHttpReq_ *req     = (struct xHttpReq_ *)userdata;
  size_t            bufsize = size * nmemb;
  if (!req->on_read) return 0; /* no provider — EOF */
  return req->on_read(buf, bufsize, req->arg);
}

/* ── Transfer completion check ─────────────────────────────────────────── */

static void check_multi_info(struct xHttpClient_ *c) {
  CURLMsg *msg;
  int      msgs_in_queue;

  while ((msg = curl_multi_info_read(c->multi, &msgs_in_queue)) != NULL) {
    if (msg->msg != CURLMSG_DONE) continue;

    CURL    *easy   = msg->easy_handle;
    CURLcode result = msg->data.result;

    struct xHttpReq_ *req = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
    XDEBUGL1("curl: CURLMSG_DONE req=%p result=%d", (void *)req, result);
    if (!req) continue;

    if (req->cleaned) continue; /* already destroyed via destroy_req */

    /* Dispatch via vtable — on_done calls user callback only.
     * After it returns, clean up curl handles + req resources. */
    if (req->vt && req->vt->on_done) req->vt->on_done(req, result);

    if (!req->cleaned) {
      req->cleaned = 1;
      /* Remove from client's request list */
      struct xHttpReq_ **pp = &c->reqs;
      while (*pp && *pp != req)
        pp = &(*pp)->next;
      if (*pp) *pp = req->next;
      curl_multi_remove_handle(c->multi, easy);
      curl_easy_cleanup(easy);
      if (req->vt && req->vt->on_cleanup) req->vt->on_cleanup(req);
      free(req);
    }
  }
}

/* ── Oneshot HTTP request handlers ─────────────────────────────────────── */

static void oneshot_on_done(struct xHttpReq_ *req, CURLcode result) {
  /* If on_response hasn't been called yet (no body or error), call it now.
   * Ignore the return value since the transfer is already complete. */
  if (!req->headers_done) {
    req->headers_done = 1;
    if (req->on_response) {
      req_build_ctx(req, result);
      req->on_response(&req->ctx, req->arg);
    }
  }

  /* Build/refresh ctx for on_done */
  req_build_ctx(req, result);

  /* Invoke user callback — do NOT clean up here, let check_multi_info handle it */
  XDEBUGL1("curl: on_done req=%p user_cb=%p", (void *)req, (void *)req->on_done);
  if (req->on_done) req->on_done(&req->ctx, req->arg);
}

static void oneshot_on_cleanup(struct xHttpReq_ *req) {
  /* Only clean up request-specific resources here.
   * curl_multi_remove + curl_easy_cleanup + free(req) are handled
   * by destroy_req() which calls this. */
  if (req->read_timer) {
    xTimerStop(req->read_timer);
    req->read_timer = NULL;
  }
  xBufferDestroy(req->header_buf);
  xBufferDestroy(req->paused_buf);
  if (req->post_data) free(req->post_data);
  if (req->req_headers) curl_slist_free_all(req->req_headers);
}

/* ── Socket callback (CURLMOPT_SOCKETFUNCTION) ─────────────────────────── */

static int socket_callback(CURL *easy, curl_socket_t fd, int what, void *userp, void *socketp) {
  (void)easy;
  struct xHttpClient_    *c   = (struct xHttpClient_ *)userp;
  struct xHttpSocketCtx_ *ctx = (struct xHttpSocketCtx_ *)socketp;

  if (what == CURL_POLL_REMOVE) {
    if (ctx) {
      xEventDel(ctx->src);
      free(ctx);
      curl_multi_assign(c->multi, fd, NULL);
    }
    return 0;
  }

  /* Map curl poll flags to xEventMask.
   * Use level-triggered for libcurl sockets: libcurl's multi-socket API
   * expects to be called whenever a fd is ready, not just on state
   * transitions.  Edge-triggered misses repeated write-ready notifications
   * (socket stays writable after small writes), causing upload stalls. */
  xEventMask mask = xEvent_LevelTriggered;
  if (what == CURL_POLL_IN) mask |= xEvent_Read;
  if (what == CURL_POLL_OUT) mask |= xEvent_Write;
  if (what == CURL_POLL_INOUT) mask |= xEvent_Read | xEvent_Write;

  if (!ctx) {
    /* First time seeing this fd — register with event loop */
    ctx = (struct xHttpSocketCtx_ *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->fd     = (int)fd;
    ctx->client = c;
    ctx->src    = xEventAdd((int)fd, mask, fd_ready_callback, ctx);
    if (!ctx->src) {
      free(ctx);
      return -1;
    }
    curl_multi_assign(c->multi, fd, ctx);
  } else {
    /* Already tracked — just update the event mask */
    xEventMod(ctx->src, mask);
  }

  return 0;
}

/* ── fd ready callback (xEventFunc) ────────────────────────────────────── */

static void fd_ready_callback(int fd, xEventMask mask, void *arg) {
  struct xHttpSocketCtx_ *ctx = (struct xHttpSocketCtx_ *)arg;
  struct xHttpClient_    *c   = (struct xHttpClient_ *)ctx->client;

  int ev_bitmask = 0;
  if (mask & xEvent_Read) ev_bitmask |= CURL_CSELECT_IN;
  if (mask & xEvent_Write) ev_bitmask |= CURL_CSELECT_OUT;

  int running = 0; /* output param — number of running transfers, unused */
  curl_multi_socket_action(c->multi, (curl_socket_t)fd, ev_bitmask, &running);
  check_multi_info(c);
}

/* ── Timer callback (CURLMOPT_TIMERFUNCTION) ───────────────────────────── */

static int timer_callback(CURLM *multi, long timeout_ms, void *userp) {
  (void)multi;
  struct xHttpClient_ *c = (struct xHttpClient_ *)userp;

  /* Cancel any existing timer */
  if (c->timer) {
    xTimerStop(c->timer);
    c->timer = NULL;
  }

  if (timeout_ms == -1) {
    /* curl says no timeout needed */
    return 0;
  }

  /*
   * For timeout_ms == 0, curl wants immediate action. However, this callback
   * may be invoked from within curl_multi_add_handle(), so calling
   * curl_multi_socket_action() here would be reentrant. Schedule a 1ms
   * timer instead to defer the action to the next event loop iteration.
   */
  if (timeout_ms == 0) timeout_ms = 1;

  /* Cap the timer at 200 ms.  When curl uses the threaded DNS resolver,
   * it may report a long timeout (e.g. the request-level CURLOPT_TIMEOUT_MS)
   * while the resolver runs on a background thread.  The resolver's
   * completion is only observed when curl_multi_socket_action is called,
   * so capping the timer bounds the stall.  The 200 ms cap is harmless
   * for normal I/O — curl_multi_socket_action(TIMEOUT) is cheap when no
   * timers have actually expired. */
  if (timeout_ms > 200) timeout_ms = 200;

  /* Schedule a new timer */
  c->timer = xTimerStart(on_timeout, c, NULL, (uint64_t)timeout_ms, 0);
  return 0;
}

/* ── Timer expiry callback (xTimerFunc) ───────────────────────────── */

static void on_timeout(void *arg) {
  struct xHttpClient_ *c = (struct xHttpClient_ *)arg;
  c->timer               = NULL;

  int running = 0;
  curl_multi_socket_action(c->multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_multi_info(c);

  /* Fallback timer: if curl has running transfers but didn't set a
   * new timer (e.g. during threaded DNS resolution, curl may report
   * timeout_ms=-1), set a short polling timer to ensure progress. */
  if (running > 0 && !c->timer) {
    c->timer = xTimerStart(on_timeout, c, NULL, 100, 0);
  }
}

/* ── Read timeout (idle body detection) ───────────────────────────────── */

static void on_read_timeout(void *arg) {
  struct xHttpReq_ *req = (struct xHttpReq_ *)arg;
  req->read_timer       = NULL; /* handle consumed by the fire */
  if (req->cleaned || req->read_timed_out) return;
  req->read_timed_out = 1;
  destroy_req(req->client, req->easy, req, 1);
}

/* ── Lifecycle: Create / Destroy ───────────────────────────────────────── */

/* ── Helper: duplicate a string or return NULL ─────────────────────────── */

static char *xstrdup_(const char *s) {
  if (!s) return NULL;
  size_t len = strlen(s);
  char  *dup = (char *)malloc(len + 1);
  if (dup) memcpy(dup, s, len + 1);
  return dup;
}

static void tls_conf_free(struct xHttpClient_ *c) {
  free(c->tls_ca);
  free(c->tls_cert);
  free(c->tls_key);
  free(c->tls_key_password);
  c->tls_ca           = NULL;
  c->tls_cert         = NULL;
  c->tls_key          = NULL;
  c->tls_key_password = NULL;
  c->tls_skip_verify  = 0;
}

static void apply_tls_conf(struct xHttpClient_ *c, const xTlsConf *conf) {
  /* Free any previous TLS config */
  tls_conf_free(c);

  if (!conf) return; /* reset to defaults */

  c->tls_ca           = xstrdup_(conf->ca);
  c->tls_cert         = xstrdup_(conf->cert);
  c->tls_key          = xstrdup_(conf->key);
  c->tls_key_password = xstrdup_(conf->key_password);
  c->tls_skip_verify  = conf->skip_verify;
}

/* Free owned string fields (user_agent / proxy / no_proxy).  Separate from
 * tls_conf_free because the two configs are independent. */
static void client_strs_free(struct xHttpClient_ *c) {
  free(c->user_agent);
  free(c->proxy);
  free(c->no_proxy);
  c->user_agent = NULL;
  c->proxy      = NULL;
  c->no_proxy   = NULL;
}

/* Apply xHttpClientConf defaults to the client struct.  Strings are copied;
 * the caller's strings do not need to outlive this call. */
static void apply_client_conf(struct xHttpClient_ *c, const xHttpClientConf *conf) {
  client_strs_free(c);

  if (!conf) {
    /* Defaults: follow up to 10 redirects, no timeouts, no proxy. */
    c->follow_location    = 1;
    c->max_redirects      = 10;
    c->timeout_ms         = 0;
    c->connect_timeout_ms = 0;

    return;
  }

  /* follow_location: zero-init struct means "default = follow".  Callers
   * who want to disable redirects must set follow_location = 0 explicitly.
   * We treat any non-zero value as "follow" to match curl semantics. */
  c->follow_location = (conf->follow_location != 0) ? 1 : 0;
  if (c->follow_location) {
    /* max_redirects == 0 means "follow infinitely" in curl.  If the caller
     * zero-initialized the struct and enabled follow (the default), cap at
     * 10.  If they explicitly set 0 with follow on, we honor "infinite". */
    c->max_redirects = conf->max_redirects;
    if (c->max_redirects == 0) {
      /* Only apply the default cap when follow_location was the implicit
       * default — i.e. the caller didn't set max_redirects.  We can't tell
       * that apart from an explicit "infinite", so we treat both as 10.
       * Callers wanting infinite redirects can set a very large value. */
      c->max_redirects = 10;
    }
  } else {
    c->max_redirects = 0;
  }

  c->timeout_ms         = conf->timeout_ms;
  c->connect_timeout_ms = conf->connect_timeout_ms;
  c->read_timeout_ms    = conf->read_timeout_ms;

  c->user_agent = xstrdup_(conf->user_agent);
  c->proxy      = xstrdup_(conf->proxy);
  c->no_proxy   = xstrdup_(conf->no_proxy);
}

xHttpClient xHttpClientCreate(const xHttpClientConf *conf) {
  xEventLoop loop = xEventLoopCurrent();
  if (!loop) return NULL;

  struct xHttpClient_ *c = (struct xHttpClient_ *)calloc(1, sizeof(struct xHttpClient_));
  if (!c) return NULL;

  c->multi = curl_multi_init();
  if (!c->multi) {
    free(c);
    return NULL;
  }

  c->loop     = loop;
  c->timer    = NULL;
  c->http_ver = xHttpVersion_Default;

  /* Apply client-level configuration (redirects, timeouts, proxy, UA,
   * TLS).  Safe to call with conf == NULL — uses defaults. */
  apply_client_conf(c, conf);
  if (conf && conf->tls) apply_tls_conf(c, conf->tls);
  if (conf && conf->http_version != xHttpVersion_Default) c->http_ver = conf->http_version;

  curl_multi_setopt(c->multi, CURLMOPT_SOCKETFUNCTION, socket_callback);
  curl_multi_setopt(c->multi, CURLMOPT_SOCKETDATA, c);
  curl_multi_setopt(c->multi, CURLMOPT_TIMERFUNCTION, timer_callback);
  curl_multi_setopt(c->multi, CURLMOPT_TIMERDATA, c);

  return (xHttpClient)c;
}

/**
 * @brief Helper: clean up a single request context and its easy handle.
 *
 * Two cleanup paths exist:
 * 1. Oneshot completion: check_multi_info -> vt->on_done -> vt->on_cleanup ->
 * free(req)
 * 2. Early destroy: destroy_req -> curl cleanup -> vt->on_cleanup -> free(req)
 *
 * The cleaned flag ensures they don't interfere with each other.
 */
static void destroy_req(struct xHttpClient_ *c, CURL *easy, struct xHttpReq_ *req, int notify) {
  if (req && notify && req->on_done) {
    if (req->read_timed_out) {
      req_build_ctx(req, CURLE_OPERATION_TIMEDOUT);
      req->ctx.curl_error = "Read timeout: no body data";
    } else {
      req_build_ctx(req, CURLE_ABORTED_BY_CALLBACK);
      req->ctx.curl_error = "Request aborted: client destroyed";
    }
    req->on_done(&req->ctx, req->arg);
  }

  if (req && !req->cleaned) {
    req->cleaned = 1;
    /* Remove from client's request list */
    struct xHttpReq_ **pp = &c->reqs;
    while (*pp && *pp != req)
      pp = &(*pp)->next;
    if (*pp) *pp = req->next;
    curl_multi_remove_handle(c->multi, easy);
    curl_easy_cleanup(easy);
    if (req->vt && req->vt->on_cleanup) req->vt->on_cleanup(req);
    free(req);
  }
}

void xHttpClientDestroy(xHttpClient client) {
  if (!client) return;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  /* Cancel the curl timeout timer if active */
  if (c->timer) {
    xTimerStop(c->timer);
    c->timer = NULL;
  }

  /*
   * Drain any already-completed transfers first, then forcibly remove
   * all remaining in-flight handles.
   */
  CURLMsg *msg;
  int      msgs_in_queue;

  while ((msg = curl_multi_info_read(c->multi, &msgs_in_queue)) != NULL) {
    if (msg->msg != CURLMSG_DONE) continue;
    CURL             *easy = msg->easy_handle;
    struct xHttpReq_ *req  = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &req);
    destroy_req(c, easy, req, 1);
  }

  /*
   * Forcibly remove any still-running easy handles by walking the
   * client's own request list.  This avoids curl_multi_get_handles()
   * which requires libcurl >= 7.84.0.
   */
  while (c->reqs) {
    struct xHttpReq_ *req = c->reqs;
    destroy_req(c, req->easy, req, 1);
  }

  curl_multi_cleanup(c->multi);
  tls_conf_free(c);
  client_strs_free(c);
  free(c);
}

/* ── Internal: configure and submit an easy handle ─────────────────────── */

static xErrno http_submit(struct xHttpClient_ *c, struct xHttpReq_ *req) {
  /* Write callback: single handler covers streaming (on_data) and discard */
  curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, req);

  /* Header callback: always buffers into header_buf */
  curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, req);

  /* Read callback: for streaming uploads (on_read).
   * Don't set CURLOPT_UPLOAD — it forces PUT.  Instead, rely on the
   * method setting (POST/PUT/etc.) + CURLOPT_READFUNCTION. */
  if (req->on_read) {
    curl_easy_setopt(req->easy, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(req->easy, CURLOPT_READDATA, req);
  }

  curl_easy_setopt(req->easy, CURLOPT_PRIVATE, req);
  curl_easy_setopt(req->easy, CURLOPT_ERRORBUFFER, req->errbuf);
  curl_easy_setopt(req->easy, CURLOPT_NOSIGNAL, 1L);

  /* Apply HTTP version: per-request override or client default */
  if (req->client) apply_http_version(req->easy, req->client->http_ver);

  /* Apply client-level defaults: redirects, connect_timeout, proxy, UA.
   * Per-request total timeout is handled in xHttpClientDo (it knows both
   * the request's timeout_ms and the client default, and applies the
   * per-request value if set, otherwise the client default). */
  if (req->client) {
    struct xHttpClient_ *cl = req->client;

    /* Redirects */
    if (cl->follow_location) {
      curl_easy_setopt(req->easy, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(req->easy, CURLOPT_MAXREDIRS, cl->max_redirects);
    }

    /* Connect-phase-only timeout (no per-request override — client only) */
    if (cl->connect_timeout_ms > 0) {
      curl_easy_setopt(req->easy, CURLOPT_CONNECTTIMEOUT_MS, cl->connect_timeout_ms);
    }

    /* Identity / proxy */
    if (cl->user_agent) curl_easy_setopt(req->easy, CURLOPT_USERAGENT, cl->user_agent);
    if (cl->proxy) curl_easy_setopt(req->easy, CURLOPT_PROXY, cl->proxy);
    if (cl->no_proxy) curl_easy_setopt(req->easy, CURLOPT_NOPROXY, cl->no_proxy);

    /* Apply TLS configuration */
    if (cl->tls_skip_verify) {
      curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(req->easy, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (cl->tls_ca) curl_easy_setopt(req->easy, CURLOPT_CAINFO, cl->tls_ca);
    if (cl->tls_cert) curl_easy_setopt(req->easy, CURLOPT_SSLCERT, cl->tls_cert);
    if (cl->tls_key) curl_easy_setopt(req->easy, CURLOPT_SSLKEY, cl->tls_key);
    if (cl->tls_key_password) curl_easy_setopt(req->easy, CURLOPT_KEYPASSWD, cl->tls_key_password);
  }

  CURLMcode mc = curl_multi_add_handle(c->multi, req->easy);
  if (mc != CURLM_OK) {
    curl_easy_cleanup(req->easy);
    xBufferDestroy(req->header_buf);
    if (req->post_data) free(req->post_data);
    if (req->req_headers) curl_slist_free_all(req->req_headers);
    free(req);
    return xErrno_Unknown;
  }

  /* Track in client's request list */
  req->next = c->reqs;
  c->reqs   = req;

  return xErrno_Ok;
}

static struct xHttpReq_ *http_req_new(struct xHttpClient_ *c, const xHttpRequestConf *conf,
                                      void *arg) {
  struct xHttpReq_ *req = (struct xHttpReq_ *)calloc(1, sizeof(struct xHttpReq_));
  if (!req) return NULL;

  req->easy = curl_easy_init();
  if (!req->easy) {
    free(req);
    return NULL;
  }

  req->vt             = &oneshot_vtable;
  req->client         = c;
  req->arg            = arg;
  req->on_done        = conf->on_done;
  req->on_response    = conf->on_response;
  req->on_data        = conf->on_data;
  req->on_read        = conf->on_read;
  req->headers_done   = 0;
  req->paused         = 0;
  req->read_timer     = NULL;
  req->read_timed_out = 0;
  memset(req->errbuf, 0, sizeof(req->errbuf));
  req->post_data   = NULL;
  req->req_headers = NULL;
  memset(&req->ctx, 0, sizeof(req->ctx));

  /* Header buffer: always needed for on_response / on_done */
  req->header_buf = xBufferCreate(512);
  if (!req->header_buf) {
    curl_easy_cleanup(req->easy);
    free(req);
    return NULL;
  }

  /* Pause buffer: lazily filled when on_data returns > 0 (backpressure). */
  req->paused_buf = xBufferCreate(0);
  if (!req->paused_buf) {
    xBufferDestroy(req->header_buf);
    curl_easy_cleanup(req->easy);
    free(req);
    return NULL;
  }

  curl_easy_setopt(req->easy, CURLOPT_URL, conf->url);

  return req;
}

/* ── Public API: GET ───────────────────────────────────────────────────── */

xErrno xHttpClientGet(xHttpClient client, const xHttpRequestConf *conf, void *arg) {
  if (!conf) return xErrno_InvalidArg;
  xHttpRequestConf c = *conf;
  c.method           = xHttpMethod_GET;
  return xHttpClientDo(client, &c, arg);
}

/* ── Public API: POST ──────────────────────────────────────────────────── */

xErrno xHttpClientPost(xHttpClient client, const xHttpRequestConf *conf, void *arg) {
  if (!conf) return xErrno_InvalidArg;
  xHttpRequestConf c = *conf;
  c.method           = xHttpMethod_POST;
  return xHttpClientDo(client, &c, arg);
}

/* ── Public API: Do (generic request) ──────────────────────────────────── */

xErrno xHttpClientDo(xHttpClient client, const xHttpRequestConf *conf, void *arg) {
  if (!client || !conf || !conf->url) return xErrno_Unknown;
  struct xHttpClient_ *c = (struct xHttpClient_ *)client;

  struct xHttpReq_ *req = http_req_new(c, conf, arg);
  if (!req) return xErrno_Unknown;

  /* Method */
  switch (conf->method) {
  case xHttpMethod_GET:
    curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
    break;
  case xHttpMethod_POST:
    curl_easy_setopt(req->easy, CURLOPT_POST, 1L);
    break;
  case xHttpMethod_PUT:
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "PUT");
    break;
  case xHttpMethod_DELETE:
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    break;
  case xHttpMethod_PATCH:
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "PATCH");
    break;
  case xHttpMethod_HEAD:
    curl_easy_setopt(req->easy, CURLOPT_NOBODY, 1L);
    break;
  case xHttpMethod_OPTIONS:
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "OPTIONS");
    break;
  case xHttpMethod_TRACE:
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "TRACE");
    break;
  default:
    curl_easy_setopt(req->easy, CURLOPT_HTTPGET, 1L);
    break;
  }

  /* Method + Body.
   *
   * When on_read is set, we use CURLOPT_UPLOAD to enable the read
   * callback (CURLOPT_UPLOAD is the reliable cross-platform way).
   * UPLOAD forces PUT, so for other methods we override with
   * CURLOPT_CUSTOMREQUEST.  When on_read is NULL, the method switch
   * above already configured the method correctly. */
  if (conf->on_read) {
    static const char *method_str[] = {
      "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE",
    };
    int mi = (conf->method >= 0 && conf->method <= 7) ? conf->method : 0;
    curl_easy_setopt(req->easy, CURLOPT_UPLOAD, 1L);
    if (mi != 2) { /* PUT is the UPLOAD default — no override needed */
      curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, method_str[mi]);
    }
    if (conf->content_length > 0) {
      curl_easy_setopt(req->easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)conf->content_length);
    }
    /* content_length == 0 → chunked transfer-encoding (automatic) */

    /* Disable Expect: 100-continue — libcurl adds it for uploads >1KB
     * on Linux, and our server doesn't send 100 Continue, causing
     * a 1s stall followed by CURLE_GOT_NOTHING. */
    req->req_headers = curl_slist_append(req->req_headers, "Expect:");
  }
  /* If on_read is NULL, no request body (GET/DELETE/etc.) */

  /* Custom headers */
  if (conf->headers) {
    for (const char **h = conf->headers; *h; h++) {
      req->req_headers = curl_slist_append(req->req_headers, *h);
    }
  }

  /* Always set CURLOPT_HTTPHEADER if we have any headers (including
   * the Expect: added above for on_read uploads).  Previously this
   * was only set when conf->headers was non-NULL, which meant the
   * Expect: header was silently dropped for uploads without custom
   * headers — causing CURLE_GOT_NOTHING on Linux. */
  if (req->req_headers) {
    curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->req_headers);
  }

  /* Per-request total timeout.
   * Priority: per-request timeout_ms > client default timeout_ms.
   * 0 means "no limit" at either level. */
  if (conf->timeout_ms > 0) {
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, conf->timeout_ms);
  } else if (c->timeout_ms > 0) {
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT_MS, c->timeout_ms);
  }

  /* Read timeout (idle body detection): client-level option. */
  req->read_timeout_ms = c->read_timeout_ms;

  /* Per-request HTTP version override */
  if (conf->http_version != xHttpVersion_Default) {
    apply_http_version(req->easy, conf->http_version);
  }

  return http_submit(c, req);
}

/* ── Public API: Resume (backpressure) ────────────────────────────────── */

xErrno xHttpClientResume(xHttpClient client_, void *arg) {
  struct xHttpClient_ *c = (struct xHttpClient_ *)client_;
  if (!c) return xErrno_InvalidArg;

  struct xHttpReq_ *req;
  for (req = c->reqs; req; req = req->next) {
    if (req->arg == arg) break;
  }
  if (!req || !req->paused) return xErrno_Unknown;

  /* Re-deliver the buffered chunk(s) to on_data. If the callback pauses
   * again (or aborts), the data stays buffered and the transfer stays
   * paused — the caller resumes again later. */
  if (req->on_data && xBufferLen(req->paused_buf) > 0) {
    int r = req->on_data((const char *)xBufferData(req->paused_buf), xBufferLen(req->paused_buf),
                         req->arg);
    if (r != 0) return xErrno_Ok; /* still paused */
  }
  xBufferReset(req->paused_buf);
  req->paused = 0;
  if (curl_easy_pause(req->easy, CURLPAUSE_RECV_CONT) != CURLE_OK) {
    return xErrno_Unknown;
  }
  return xErrno_Ok;
}
