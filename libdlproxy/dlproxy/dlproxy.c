/*
 * dlproxy.c - Context lifecycle, mode switching, task scheduling
 */
#include "dlproxy.h"
#include "dlproxy_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/event.h>
#include <x/base/log.h>
#include <x/base/map.h>
#include <x/base/thread.h>
#include <x/http/client.h>

/* ── Scheduling defaults ──────────────────────────────────────────── */

#define DL_EMERGENCY_MS  10000
#define DL_SAFE_MS       30000
#define DL_BLOCK_SIZE    (256 * 1024)
#define DL_TICK_MS       1000

/* ── Forward declarations ──────────────────────────────────────────── */

static void on_tick(void *arg);

/* ── Context lifecycle ─────────────────────────────────────────────── */

dlp_ctx_t dlp_init(const dlp_conf_t *conf) {
  struct dlp_ctx *ctx = (struct dlp_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) return NULL;

  if (conf) ctx->conf = *conf;
  if (ctx->conf.port == 0) ctx->conf.port = 19080;
  if (!ctx->conf.cache_dir) ctx->conf.cache_dir = "./cache";
  if (ctx->conf.emergency_ms <= 0) ctx->conf.emergency_ms = DL_EMERGENCY_MS;
  if (ctx->conf.safe_ms <= 0) ctx->conf.safe_ms = DL_SAFE_MS;

  ctx->loop = xEventLoopCreate();
  if (!ctx->loop) goto fail;

  xEventLoopEnter(ctx->loop);

  ctx->url_map   = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  ctx->task_map  = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  ctx->bus       = dlp_bus_create();
  ctx->cache     = dlp_cache_init(ctx->conf.cache_dir, ctx->loop);
  ctx->dl_http = dlp_http_init(ctx);

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
    /* DETACHED: TODO */
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
  return ((struct dlp_ctx *)ctx)->conf.port;
}

/* ── Task lifecycle ────────────────────────────────────────────────── */

dlp_task_t dlp_task_create(dlp_ctx_t ctx, const dlp_task_conf_t *conf) {
  if (!ctx || !conf || !conf->rid || !conf->url) return NULL;
  struct dlp_ctx *c = (struct dlp_ctx *)ctx;

  struct dlp_task *t = (struct dlp_task *)calloc(1, sizeof(*t));
  if (!t) return NULL;

  snprintf(t->rid, sizeof(t->rid), "%s", conf->rid);
  snprintf(t->url, sizeof(t->url), "%s", conf->url);
  t->ctx          = c;
  t->format       = conf->format;
  t->emergency_ms = c->conf.emergency_ms;
  t->safe_ms      = c->conf.safe_ms;
  t->bitrate      = conf->bitrate ? conf->bitrate : 500 * 1024;
  t->sched        = (conf->format == DLP_FMT_HLS) ? &dlp_sched_hls : &dlp_sched_mp4;
  t->downloading_off = (uint64_t)-1;
  t->downloading_seg = (uint32_t)-1;

  /* Store mapping and open cache */
  xMapSet(c->url_map, t->rid, strdup(t->url));
  xMapSet(c->task_map, t->rid, t);
  dlp_cache_open_resource(c->cache, t->rid);
  if (t->format != DLP_FMT_HLS) {
    /* MP4: open clip "0" immediately. HLS: clips opened per-segment. */
    dlp_cache_open_clip(c->cache, t->rid, "0", conf->size);
  }

  XDEBUGL0("task created rid=%s url=%s size=%llu",
          conf->rid, conf->url, (unsigned long long)conf->size);
  return t;
}

/* ── Scheduling tick ────────────────────────────────────────────────── */

static void on_tick(void *arg) {
  struct dlp_task *t = (struct dlp_task *)arg;
  if (!t->running || !t->sched) return;
  t->sched->on_tick(t);
}

xErrno dlp_task_start(dlp_task_t task) {
  if (!task) return xErrno_InvalidArg;
  struct dlp_task *t = (struct dlp_task *)task;

  t->running     = true;
  t->was_pulling = false;
  t->downloading_off = (uint64_t)-1;  /* no download in progress */
  t->downloading_seg = (uint32_t)-1;

  /* Enter event loop for timer creation — dlp_task_start may be called
   * before dlp_run() enters the loop. Without this, xTimerStart uses
   * xEventLoopCurrent() which returns NULL and the timer never fires. */
  xEventLoopEnter(t->ctx->loop);

  /* Start 1-second scheduling timer */
  t->tick_timer = xTimerStart(on_tick, t, DL_TICK_MS, DL_TICK_MS);

  xEventLoopLeave();

  if (!t->tick_timer) return xErrno_SysError;

  /* Call scheduler on_start */
  if (t->sched && t->sched->on_start) t->sched->on_start(t);

  XDEBUGL0("task started rid=%s", t->rid);
  return xErrno_Ok;
}

xErrno dlp_task_stop(dlp_task_t task) {
  if (!task) return xErrno_InvalidArg;
  struct dlp_task *t = (struct dlp_task *)task;

  t->running = false;
  if (t->sched && t->sched->on_stop) t->sched->on_stop(t);
  if (t->tick_timer) {
    xTimerStop(t->tick_timer);
    t->tick_timer = NULL;
  }
  return xErrno_Ok;
}

void dlp_task_update_position(dlp_task_t task, uint64_t byte_offset,
                               int remain_ms) {
  if (!task) return;
  struct dlp_task *t = (struct dlp_task *)task;
  t->read_offset    = byte_offset;
  t->remain_time_ms = remain_ms;
}

void dlp_task_destroy(dlp_task_t task) {
  if (!task) return;
  struct dlp_task *t = (struct dlp_task *)task;
  struct dlp_ctx *c = t->ctx;

  dlp_task_stop(task);

  char *url = (char *)xMapDel(c->url_map, t->rid);
  if (url) free(url);
  if (t->dl_client) xHttpClientDestroy(t->dl_client);
  if (t->playlist) hls_playlist_free(t->playlist);
  free(t);
}
