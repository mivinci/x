/*
 * http.c - Async HTTP Range download helper
 */
#include "http.h"
#include "bus.h"
#include "cache.h"
#include "dlproxy_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <x/base/log.h>

struct dlp_http {
  dlp_ctx_t     ctx;
  xEventLoop    loop;
  xHttpClient   client;
};

struct fetch_ctx {
  dlp_http_t     h;
  char            rid[64];
  char            clip_id[64];
  uint64_t        offset;
  size_t          len;
  size_t          received;
  int             status_ok;
  int             pending_writes;  /* async cache writes in progress */
  int             all_received;    /* on_http_done has fired */
  struct dlp_task *task;
};

/* Write completion callback — publish to bus when all writes + data done */
static void on_fetch_write_done(xErrno err, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  (void)err;
  int remaining = --fc->pending_writes;
  XDEBUGL0("write_done: pending=%d all_recv=%d", remaining, fc->all_received);
  if (remaining == 0 && fc->all_received) {
    struct dlp_ctx *c = (struct dlp_ctx *)fc->h->ctx;
    char key[128];
    snprintf(key, sizeof(key), "%s", fc->rid);
    XDEBUGL0("write_done: publishing bus key=%s", key);
    dlp_bus_publish(c->bus, key);
    if (fc->task && fc->task->sched && fc->task->sched->on_block_done)
      fc->task->sched->on_block_done(fc->task, fc->offset, fc->received);
    free(fc);
  }
}

static int on_http_response(xHttpCtx *ctx, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  fc->status_ok = (ctx->status_code >= 200 && ctx->status_code < 300);

  /* Extract total file size from Content-Range in response headers */
  if (fc->task && fc->task->file_size == 0 && ctx->headers) {
    const char *cr = strcasestr(ctx->headers, "content-range:");
    if (cr) {
      uint64_t total = 0;
      /* Parse header: bytes start-end/total */
      const char *slash = strrchr(cr, '/');
      if (slash && slash[1] != '*') {
        char *end;
        total = strtoull(slash + 1, &end, 10);
        if (total > 0 && end != slash + 1) {
          fc->task->file_size = total;
          /* Propagate file size to cache so the last block's size
           * is recalculated and can be marked as done. */
          struct dlp_ctx *c = (struct dlp_ctx *)fc->h->ctx;
          dlp_cache_set_file_size(c->cache, fc->rid, fc->clip_id, total);
        }
      }
    }
  }
  (void)ctx;
  return 0;
}

static int on_http_data(const char *data, size_t len, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  if (!fc->status_ok) return 0;

  struct dlp_ctx *c = (struct dlp_ctx *)fc->h->ctx;
  fc->pending_writes++;
  dlp_cache_write(c->cache, fc->rid, fc->clip_id,
                  fc->offset + fc->received,
                  (const uint8_t *)data, len,
                  on_fetch_write_done, fc);
  fc->received += len;
  return 0;
}

static void on_http_done(xHttpCtx *ctx, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  fc->all_received = 1;
  XDEBUGL0("http_done: pending=%d all_recv=1", fc->pending_writes);
  /* If no pending writes, publish now directly */
  if (fc->pending_writes == 0) {
    struct dlp_ctx *c = (struct dlp_ctx *)fc->h->ctx;
    char key[128];
    snprintf(key, sizeof(key), "%s", fc->rid);
    dlp_bus_publish(c->bus, key);
    if (fc->task && fc->task->sched && fc->task->sched->on_block_done)
      fc->task->sched->on_block_done(fc->task, fc->offset, fc->received);
    free(fc);
  }
  (void)ctx;
}

dlp_http_t dlp_http_init(dlp_ctx_t ctx) {
  struct dlp_http *h = (struct dlp_http *)calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->ctx  = ctx;
  h->loop = xEventLoopCurrent();
  h->client = xHttpClientCreate(NULL);
  if (!h->client) { free(h); return NULL; }
  return h;
}

void dlp_http_deinit(dlp_http_t h) {
  if (!h) return;
  if (h->client) xHttpClientDestroy(h->client);
  free(h);
}

xErrno dlp_http_fetch(dlp_http_t h, const char *rid,
                       const char *clip_id, const char *url,
                       uint64_t offset, size_t len,
                       struct dlp_task *task) {
  if (!h || !rid || !url || len == 0) return xErrno_InvalidArg;

  char range[128];
  snprintf(range, sizeof(range), "Range: bytes=%llu-%llu",
           (unsigned long long)offset,
           (unsigned long long)(offset + len - 1));

  XDEBUGL0("FETCH: rid=%s offset=%llu len=%zu url=%s", rid, (unsigned long long)offset, len, url);

  struct fetch_ctx *fc = (struct fetch_ctx *)calloc(1, sizeof(*fc));
  if (!fc) return xErrno_NoMemory;
  fc->h      = h;
  fc->task   = task;
  fc->offset = offset;
  fc->len    = len;
  snprintf(fc->rid, sizeof(fc->rid), "%s", rid);
  snprintf(fc->clip_id, sizeof(fc->clip_id), "%s", clip_id);

  xHttpRequestConf conf = {0};
  conf.method         = xHttpMethod_GET;
  conf.url            = url;
  conf.on_response    = on_http_response;
  conf.on_data        = on_http_data;
  conf.on_done        = on_http_done;
  conf.timeout_ms     = 30000;

  const char *headers[] = { range, NULL };
  conf.headers = headers;

  xErrno rv = xHttpClientDo(h->client, &conf, fc);
  XDEBUGL0("FETCH_DONE: rc=%d", rv);
  return rv;
}
