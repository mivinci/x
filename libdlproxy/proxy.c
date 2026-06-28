/*
 * proxy.c - Local HTTP proxy server with cache miss deferral
 */
#include "proxy.h"
#include "bus.h"
#include "cache.h"
#include "dlproxy_internal.h"
#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <x/base/event.h>
#include <x/base/map.h>
#include <x/http/server.h>

#define DL_BLOCK_SIZE (256 * 1024)

/* ── Per-request context for cache-miss deferral ── */

struct proxy_req {
  xHttpCtx *http_ctx;
  dlp_ctx_t dlp;
  char      rid[64];
  uint64_t  offset;
  size_t    length;
  uint8_t  *buf;  /* for async cache read completion */
};

/* ── Cache-read completion callback ── */

struct read_done_ctx {
  xHttpCtx *http_ctx;
  uint8_t  *buf;
  size_t    length;
  uint64_t  offset;
};

static void on_cache_read_done(xErrno err, void *arg) {
  struct read_done_ctx *p = (struct read_done_ctx *)arg;
  if (err == xErrno_Ok && p->length > 0) {
    xHttpCtxSetStatus(p->http_ctx, 206);
    xHttpCtxSetHeader(p->http_ctx, "Content-Type", "video/mp4");
    xHttpCtxSetHeader(p->http_ctx, "Access-Control-Allow-Origin", "*");
    char cr[64];
    snprintf(cr, sizeof(cr), "bytes %llu-%llu/*",
             (unsigned long long)p->offset,
             (unsigned long long)(p->offset + p->length - 1));
    xHttpCtxSetHeader(p->http_ctx, "Content-Range", cr);
    xHttpCtxSend(p->http_ctx, (const char *)p->buf, p->length);
  } else {
    xHttpCtxSetStatus(p->http_ctx, 503);
    xHttpCtxSend(p->http_ctx, "cache read error\n", 17);
  }
  free(p->buf);
  free(p);
}

/* ── Bus callback: chunk ready, re-serve ── */

static void on_chunk_ready(void *arg) {
  struct proxy_req *pr = (struct proxy_req *)arg;
  struct dlp_ctx   *c  = (struct dlp_ctx *)pr->dlp;

  size_t   len = pr->length;
  uint8_t *buf = (uint8_t *)malloc(len);
  if (!buf) {
    xHttpCtxSetStatus(pr->http_ctx, 503);
    xHttpCtxSend(pr->http_ctx, "out of memory\n", 14);
    xHttpCtxResume(pr->http_ctx);
    free(pr);
    return;
  }

  struct read_done_ctx *rd = (struct read_done_ctx *)malloc(sizeof(*rd));
  if (!rd) {
    free(buf);
    xHttpCtxSetStatus(pr->http_ctx, 503);
    xHttpCtxSend(pr->http_ctx, "out of memory\n", 14);
    xHttpCtxResume(pr->http_ctx);
    free(pr);
    return;
  }
  rd->http_ctx = pr->http_ctx;
  rd->buf      = buf;
  rd->length   = len;
  rd->offset   = pr->offset;

  dlp_cache_read(c->cache, pr->rid, "0", pr->offset, buf, len,
                  on_cache_read_done, rd);

  xHttpCtxResume(pr->http_ctx);
  free(pr);
}

/* ── Main route handler ── */

static int serve_range(xHttpCtx *http_ctx, void *arg) {
  dlp_ctx_t dlp = (dlp_ctx_t)arg;
  struct dlp_ctx *c = (struct dlp_ctx *)dlp;

  if (strcmp(http_ctx->method, "GET") != 0) {
    xHttpCtxSetStatus(http_ctx, 405);
    xHttpCtxSend(http_ctx, "only GET\n", 9);
    return 0;
  }

  /* Parse path as /:rid */
  size_t  rid_len = 0;
  const char *rid = xHttpCtxParam(http_ctx, "rid", &rid_len);
  if (!rid || rid_len == 0) {
    xHttpCtxSetStatus(http_ctx, 404);
    xHttpCtxSend(http_ctx, "missing rid\n", 12);
    return 0;
  }

  char rid_buf[64];
  snprintf(rid_buf, sizeof(rid_buf), "%.*s", (int)rid_len, rid);

  /* Parse Range header from raw headers */
  uint64_t range_start = 0;
  uint64_t range_end   = (uint64_t)(-1);
  if (http_ctx->headers) {
    const char *p = strstr(http_ctx->headers, "Range:");
    if (p) {
      sscanf(p, "Range: bytes=%llu-%llu",
             (unsigned long long *)&range_start,
             (unsigned long long *)&range_end);
    }
  }

  /* Look up task — sync playback position from Range request */
  struct dlp_task *task = (struct dlp_task *)xMapGet(c->task_map, rid_buf);
  if (task) {
    task->read_offset = range_start;
    /* Estimate remain_time from cache readiness ahead of play head */
    int cached_blocks = 0;
    uint64_t pos = range_start;
    while (dlp_cache_is_ready(c->cache, rid_buf, "0", pos, DL_BLOCK_SIZE)) {
      cached_blocks++;
      pos += DL_BLOCK_SIZE;
    }
    task->remain_time_ms = cached_blocks > 0
      ? (int)(cached_blocks * DL_BLOCK_SIZE / (task->bitrate > 0 ? task->bitrate : 1) * 1000)
      : 0;
  }

  size_t max_read = 2 * 1024 * 1024;
  if (range_end == (uint64_t)(-1) || range_end - range_start + 1 > max_read)
    range_end = range_start + max_read - 1;
  size_t length = (size_t)(range_end - range_start + 1);

  /* Check cache */
  if (dlp_cache_is_ready(c->cache, rid_buf, "0", range_start, length)) {
    uint8_t *buf = (uint8_t *)malloc(length);
    if (!buf) {
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return 0;
    }
    struct read_done_ctx *rd = (struct read_done_ctx *)malloc(sizeof(*rd));
    if (!rd) {
      free(buf);
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return 0;
    }
    rd->http_ctx = http_ctx;
    rd->buf      = buf;
    rd->length   = length;
    rd->offset   = range_start;
    dlp_cache_read(c->cache, rid_buf, "0", range_start, buf, length,
                    on_cache_read_done, rd);
    return 0;
  }

  /* Cache miss — defer response */
  xHttpCtxYield(http_ctx);

  struct proxy_req *pr = (struct proxy_req *)calloc(1, sizeof(*pr));
  if (!pr) { xHttpCtxResume(http_ctx); return 0; }
  pr->http_ctx = http_ctx;
  pr->dlp      = dlp;
  pr->offset   = range_start;
  pr->length   = length;
  snprintf(pr->rid, sizeof(pr->rid), "%s", rid_buf);

  dlp_bus_subscribe(c->bus, rid_buf, on_chunk_ready, pr);

  /* Trigger immediate download on cache miss */
  if (task) {
    uint64_t boff = (range_start / DL_BLOCK_SIZE) * DL_BLOCK_SIZE;
    dlp_scheduler_fetch(c->scheduler, task->rid, "0", task->url,
                        boff, DL_BLOCK_SIZE);
  }

  return 0;
}

/* ── Lifecycle ── */

struct dlp_proxy {
  xHttpServer server;
  xHttpMux    mux;
  uint16_t    port;
};

dlp_proxy_t dlp_proxy_init(dlp_ctx_t ctx, xEventLoop loop, uint16_t port) {
  struct dlp_proxy *p = (struct dlp_proxy *)calloc(1, sizeof(*p));
  if (!p) return NULL;

  p->port = port;
  xEventLoopEnter(loop);

  p->mux = xHttpMuxCreate();
  if (!p->mux) goto fail;

  xHttpRouteConf rc = {0};
  rc.pattern    = "GET /:rid";
  rc.on_request = serve_range;
  rc.arg        = ctx;
  xHttpMuxHandle(p->mux, &rc);

  xHttpServerConf sc = {0};
  sc.resolve = xHttpMuxResolve;
  sc.router  = p->mux;
  p->server  = xHttpServerCreate(&sc);
  if (!p->server) goto fail;

  if (xHttpServerListen(p->server, "127.0.0.1", port) != xErrno_Ok)
    goto fail;

  fprintf(stderr, "dlproxy listening on 127.0.0.1:%d\n", port);
  xEventLoopLeave();
  return p;

fail:
  if (p->server) xHttpServerDestroy(p->server);
  if (p->mux) xHttpMuxDestroy(p->mux);
  xEventLoopLeave();
  free(p);
  return NULL;
}

void dlp_proxy_deinit(dlp_proxy_t p) {
  if (!p) return;
  if (p->server) xHttpServerDestroy(p->server);
  if (p->mux) xHttpMuxDestroy(p->mux);
  free(p);
}
