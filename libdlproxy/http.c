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
  struct dlp_task *task;
};

static int on_http_response(xHttpCtx *ctx, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  fc->status_ok = (ctx->status_code >= 200 && ctx->status_code < 300);
  (void)ctx;
  return 0;
}

static int on_http_data(const char *data, size_t len, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  if (!fc->status_ok) return 0;

  struct dlp_ctx *c = (struct dlp_ctx *)fc->h->ctx;
  dlp_cache_write(c->cache, fc->rid, fc->clip_id,
                  fc->offset + fc->received,
                  (const uint8_t *)data, len, NULL, NULL);
  fc->received += len;
  return 0;
}

static void on_http_done(xHttpCtx *ctx, void *arg) {
  struct fetch_ctx *fc = (struct fetch_ctx *)arg;
  struct dlp_ctx  *c  = (struct dlp_ctx *)fc->h->ctx;

  char key[128];
  snprintf(key, sizeof(key), "%s", fc->rid);
  dlp_bus_publish(c->bus, key);

  if (fc->task && fc->task->sched && fc->task->sched->on_block_done)
    fc->task->sched->on_block_done(fc->task, fc->offset, fc->received);

  free(fc);
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

  struct fetch_ctx *fc = (struct fetch_ctx *)calloc(1, sizeof(*fc));
  if (!fc) return xErrno_NoMemory;
  fc->h      = h;
  fc->task   = task;
  fc->offset = offset;
  fc->len    = len;
  snprintf(fc->rid, sizeof(fc->rid), "%s", rid);
  snprintf(fc->clip_id, sizeof(fc->clip_id), "%s", clip_id);

  char range[128];
  snprintf(range, sizeof(range), "bytes=%llu-%llu",
           (unsigned long long)offset,
           (unsigned long long)(offset + len - 1));

  xHttpRequestConf conf = {0};
  conf.method         = xHttpMethod_GET;
  conf.url            = url;
  conf.on_response    = on_http_response;
  conf.on_data        = on_http_data;
  conf.on_done        = on_http_done;
  conf.timeout_ms     = 30000;

  const char *headers[] = { range, NULL };
  conf.headers = headers;

  return xHttpClientDo(h->client, &conf, fc);
}
