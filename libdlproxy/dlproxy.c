/*
 * dlproxy.c - Context lifecycle and mode switching
 */
#include "dlproxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/event.h>
#include <x/base/map.h>
#include <x/base/thread.h>

#include "bus.h"
#include "cache.h"
#include "dlproxy_internal.h"
#include "proxy.h"
#include "scheduler.h"

dlp_ctx_t dlp_init(const dlp_conf_t *conf) {
  struct dlp_ctx *ctx = (struct dlp_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (conf) ctx->conf = *conf;
  if (ctx->conf.port == 0) ctx->conf.port = 19080;
  if (!ctx->conf.cache_dir) ctx->conf.cache_dir = "./cache";

  ctx->loop = xEventLoopCreate();
  if (!ctx->loop) goto fail;

  xEventLoopEnter(ctx->loop);

  ctx->url_map  = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  ctx->bus      = dlp_bus_create();
  ctx->cache    = dlp_cache_init(ctx->conf.cache_dir, ctx->loop);
  ctx->scheduler = dlp_scheduler_init(ctx, ctx->loop);

  xEventLoopLeave();
  return ctx;

fail:
  free(ctx);
  return NULL;
}

xErrno dlp_run(dlp_ctx_t ctx, dlp_mode_t mode) {
  if (!ctx) return xErrno_InvalidArg;
  struct dlp_ctx *c = (struct dlp_ctx *)ctx;
  c->mode = mode;

  if (mode == DL_MODE_POLL) {
    xEventLoopEnter(c->loop);
    c->proxy = dlp_proxy_init(ctx, c->loop, c->conf.port);
    if (!c->proxy) { xEventLoopLeave(); return xErrno_SysError; }
    xEventLoopRun(c->loop, X_RUN_DEFAULT);
    dlp_proxy_deinit(c->proxy);
    xEventLoopLeave();
  } else {
    /* DETACHED: spawn background thread */
    /* TODO */
  }
  return xErrno_Ok;
}

static void dlp_deinit_post(void *arg) {
  xEventLoopStop((xEventLoop)arg);
}

void dlp_stop(dlp_ctx_t ctx) {
  if (!ctx) return;
  struct dlp_ctx *c = (struct dlp_ctx *)ctx;
  xEventLoopPost(c->loop, dlp_deinit_post, c->loop);
}

uint16_t dlp_port(dlp_ctx_t ctx) {
  if (!ctx) return 0;
  struct dlp_ctx *c = (struct dlp_ctx *)ctx;
  return c->conf.port;
}

xErrno dlp_task_add(dlp_ctx_t ctx, const dlp_task_conf_t *conf) {
  if (!ctx || !conf || !conf->rid || !conf->url) return xErrno_InvalidArg;
  struct dlp_ctx *c = (struct dlp_ctx *)ctx;

  /* Store rid → cdn_url mapping */
  xMapSet(c->url_map, conf->rid, strdup(conf->url));

  /* Open cache resource and default clip */
  dlp_cache_open_resource(c->cache, conf->rid);
  dlp_cache_open_clip(c->cache, conf->rid, "0", conf->size);

  fprintf(stderr, "dlproxy: task added rid=%s url=%s size=%llu\n",
          conf->rid, conf->url, (unsigned long long)conf->size);
  return xErrno_Ok;
}
