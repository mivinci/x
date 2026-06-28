/*
 * dlproxy.c - Context lifecycle and mode switching
 */
#include "dlproxy.h"

#include <stdlib.h>
#include <string.h>
#include <x/base/event.h>
#include <x/base/thread.h>

#include "bus.h"
#include "cache.h"
#include "proxy.h"
#include "scheduler.h"

struct dlp_ctx {
  xEventLoop loop;
  dlp_mode_t mode;
  dlp_conf_t conf;
  dlp_bus_t bus;
  dlp_cache_t cache;
  dlp_scheduler_t scheduler;
  dlp_proxy_t proxy;
  xThread thread;
};

dlp_ctx_t dlp_init(const dlp_conf_t *conf) {
  struct dlp_ctx *ctx = (struct dlp_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (conf) ctx->conf = *conf;
  if (ctx->conf.port == 0) ctx->conf.port = 19080;
  if (!ctx->conf.cache_dir) ctx->conf.cache_dir = "./cache";

  ctx->loop = xEventLoopCreate();
  if (!ctx->loop) goto fail;

  xEventLoopEnter(ctx->loop);

  /* TODO: init bus, cache, scheduler, proxy */

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
  (void)ctx; (void)conf;
  return xErrno_Ok; /* TODO */
}
