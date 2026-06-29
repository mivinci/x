/*
 * proxy.c - Local HTTP proxy server with cache miss deferral
 *
 * Range handling modeled after tvkproxy:
 *   - Non-Range request → 200 OK, first block, Accept-Ranges: bytes
 *     (so the browser can parse initial MP4 data and discover it
 *      needs more — e.g. moov at the tail)
 *   - Range request    → 206 Partial Content, Content-Range: bytes X-Y/TOTAL
 *   - Responses are clamped to one-block (256 KB) boundaries
 *
 * Flow:
 *   1. Browser navigates to http://127.0.0.1:<port>/:rid
 *   2. If no Range header → serve first block as 200
 *   3. Browser creates <video> element, sends Range: bytes=0-1 probe
 *   4. Cache hit  → read + respond synchronously
 *      Cache miss → defer (xHttpCtxYield) + subscribe to bus +
 *                   post scheduler tick (xEventLoopPost)
 *   5. Scheduler downloads the needed block, writes to cache,
 *      publishes to bus → on_chunk_ready → read cache → send response
 *
 * Resume / seek support:
 *   Seeking sends a Range request at the new playback position.
 *   If the block is cached → instant 206 response.
 *   If not → defer + scheduler downloads it on-demand.
 *   This works without any special "resume" logic — the browser
 *   drives byte-range requests naturally.
 */
#include "proxy.h"
#include "bus.h"
#include "cache.h"
#include "dlproxy_internal.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <x/base/event.h>
#include <x/base/log.h>
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
  bool      served;
  int       has_range;
};

/* ── Cache-read completion callback ── */

struct read_done_ctx {
  xHttpCtx *http_ctx;
  dlp_ctx_t dlp;
  char      rid[64];
  uint8_t  *buf;
  size_t    length;
  uint64_t  offset;
  int       has_range;
};

/*
 * Cache-read completion — invoked when xFsReq async read finishes.
 * Sets status/headers based on has_range, then sends and resumes.
 */
static void on_cache_read_done(xErrno err, void *arg) {
  struct read_done_ctx *p = (struct read_done_ctx *)arg;
  if (err == xErrno_Ok && p->length > 0) {
    xHttpCtxSetStatus(p->http_ctx, p->has_range ? 206 : 200);
    xHttpCtxSetHeader(p->http_ctx, "Content-Type", "video/mp4");
    xHttpCtxSetHeader(p->http_ctx, "Accept-Ranges", "bytes");
    xHttpCtxSetHeader(p->http_ctx, "Access-Control-Allow-Origin", "*");
    if (p->has_range) {
      char cr[96];
      struct dlp_ctx *c = (struct dlp_ctx *)p->dlp;
      struct dlp_task *task = (struct dlp_task *)xMapGet(c->task_map, p->rid);
      uint64_t total = (task && task->file_size) ? task->file_size : 0;
      if (total > 0)
        snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu",
                 (unsigned long long)p->offset,
                 (unsigned long long)(p->offset + p->length - 1),
                 (unsigned long long)total);
      else
        snprintf(cr, sizeof(cr), "bytes %llu-%llu/*",
                 (unsigned long long)p->offset,
                 (unsigned long long)(p->offset + p->length - 1));
      xHttpCtxSetHeader(p->http_ctx, "Content-Range", cr);
    }
    xHttpCtxSend(p->http_ctx, (const char *)p->buf, p->length);
  } else {
    xHttpCtxSetStatus(p->http_ctx, 503);
    xHttpCtxSend(p->http_ctx, "cache read error\n", 17);
  }
  xHttpCtxResume(p->http_ctx);
  free(p);
}

/* ── Bus callback: chunk ready, re-serve ── */

/*
 * Bus callback — fired when a chunk download completes.
 * Re-reads from cache and sends the deferred response.
 */
static void on_chunk_ready(void *arg) {
  struct proxy_req *pr = (struct proxy_req *)arg;
  if (pr->served) return;
  struct dlp_ctx *c = (struct dlp_ctx *)pr->dlp;

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
  rd->http_ctx  = pr->http_ctx;
  rd->dlp       = pr->dlp;
  snprintf(rd->rid, sizeof(rd->rid), "%s", pr->rid);
  rd->buf       = buf;
  rd->length    = len;
  rd->offset    = pr->offset;
  rd->has_range = pr->has_range;

  dlp_cache_read(c->cache, pr->rid, "0", pr->offset, buf, len,
                  on_cache_read_done, rd);

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

  /* Look up task */
  struct dlp_task *task = (struct dlp_task *)xMapGet(c->task_map, rid_buf);
  if (!task) {
    xHttpCtxSetStatus(http_ctx, 404);
    xHttpCtxSend(http_ctx, "unknown rid\n", 12);
    return 0;
  }

  /* Parse Range header */
  int      has_range   = 0;
  uint64_t range_start = 0;
  uint64_t range_end   = (uint64_t)(-1);
  if (http_ctx->headers) {
    const char *p = strstr(http_ctx->headers, "Range:");
    if (p) {
      has_range = 1;
      sscanf(p, "Range: bytes=%llu-%llu",
             (unsigned long long *)&range_start,
             (unsigned long long *)&range_end);
    }
  }

  /* Without Range → serve first block as 200 (like tvkproxy).
   * The browser parses initial MP4 data from this and will send
   * Range probes and tail requests as needed. */
  if (!has_range) {
    range_start = 0;
    range_end   = DL_BLOCK_SIZE - 1;
  }

  /* Clamp to file bounds */
  uint64_t file_size = task->file_size;
  if (range_end == (uint64_t)(-1)) range_end = range_start + DL_BLOCK_SIZE - 1;
  if (file_size > 0 && range_end >= file_size) range_end = file_size - 1;
  if (range_start > range_end) range_start = range_end;
  size_t length = (size_t)(range_end - range_start + 1);

  /* Clamp to chunk boundary — serve one block at a time.
   * When the requested range spans multiple blocks, only the
   * first block is served. The browser will request the rest
   * via subsequent Range headers. */
  uint64_t boff_block = (range_start / DL_BLOCK_SIZE) * DL_BLOCK_SIZE;
  uint64_t chunk_end  = boff_block + DL_BLOCK_SIZE - 1;
  if (file_size > 0 && chunk_end >= file_size) chunk_end = file_size - 1;
  if (range_start + length - 1 > chunk_end)
    length = (size_t)(chunk_end - range_start + 1);

  task->read_offset = range_start;

  XDEBUGL0("REQ: rid=%s has_range=%d start=%llu len=%zu",
          rid_buf, has_range, (unsigned long long)range_start, length);

  /* Check cache */
  if (dlp_cache_is_ready(c->cache, rid_buf, "0", range_start, length)) {
    XDEBUGL0("cache HIT offset=%llu len=%zu", (unsigned long long)range_start, length);
    xHttpCtxYield(http_ctx);
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
    rd->http_ctx  = http_ctx;
    rd->dlp       = dlp;
    snprintf(rd->rid, sizeof(rd->rid), "%s", rid_buf);
    rd->buf       = buf;
    rd->length    = length;
    rd->offset    = range_start;
    rd->has_range = has_range;
    dlp_cache_read(c->cache, rid_buf, "0", range_start, buf, length,
                    on_cache_read_done, rd);
    return 0;
  }

  /* Cache miss — defer response */
  XDEBUGL0("cache MISS offset=%llu len=%zu — deferring",
          (unsigned long long)range_start, length);
  xHttpCtxYield(http_ctx);

  struct proxy_req *pr = (struct proxy_req *)calloc(1, sizeof(*pr));
  if (!pr) { xHttpCtxResume(http_ctx); return 0; }
  pr->http_ctx  = http_ctx;
  pr->dlp       = dlp;
  pr->offset    = range_start;
  pr->length    = length;
  pr->has_range = has_range;
  snprintf(pr->rid, sizeof(pr->rid), "%s", rid_buf);

  dlp_bus_subscribe(c->bus, rid_buf, on_chunk_ready, pr);

  /* Post scheduler tick after bus subscription is set up */
  if (task->running && task->sched && task->sched->on_tick)
    xEventLoopPost(c->loop, (void(*)(void*))task->sched->on_tick, task);

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

  XDEBUGL0("dlproxy listening on 127.0.0.1:%d", port);
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
