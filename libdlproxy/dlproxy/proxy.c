/*
 * proxy.c - Local HTTP proxy server with cache miss deferral
 *
 * Range handling modeled after tvkproxy:
 *   - Non-Range request → 200 OK, first block, Accept-Ranges: bytes
 *   - Range request    → 206 Partial Content, Content-Range: bytes X-Y/TOTAL
 *   - Responses are clamped to one-block (256 KB) boundaries
 *
 * Flow:
 *   1. Browser navigates to http://127.0.0.1:<port>/:rid
 *   2. If no Range header → serve first block as 200
 *   3. Browser creates <video> element, sends Range: bytes=0-1 probe
 *   4. Cache hit  → async cache read → send response
 *      Cache miss → subscribe to bus + trigger scheduler download
 *   5. Scheduler downloads the needed block, writes to cache,
 *      publishes to bus → on_chunk_ready → read cache → send response
 *
 * Event-driven model: handler return does nothing. Responses are sent
 * explicitly via xHttpCtxSend (which finalizes internally).
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
  char      clip_id[32]; /* "0" for MP4, "<seq>" for HLS */
  uint64_t  offset;
  size_t    length;
  int       has_range;
  int       content_type; /* 0=mp4, 1=m3u8, 2=ts */
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
  int       content_type;
};

static const char *content_type_str(int ct) {
  switch (ct) {
  case 1:
    return "application/vnd.apple.mpegurl";
  case 2:
    return "video/mp2t";
  default:
    return "video/mp4";
  }
}

static void on_cache_read_done(xErrno err, void *arg) {
  struct read_done_ctx *p = (struct read_done_ctx *)arg;
  if (err == xErrno_Ok && p->length > 0) {
    xHttpCtxSetStatus(p->http_ctx, p->has_range ? 206 : 200);
    xHttpCtxSetHeader(p->http_ctx, "Content-Type", content_type_str(p->content_type));
    xHttpCtxSetHeader(p->http_ctx, "Accept-Ranges", "bytes");
    xHttpCtxSetHeader(p->http_ctx, "Access-Control-Allow-Origin", "*");
    if (p->has_range) {
      char             cr[96];
      struct dlp_ctx  *c     = (struct dlp_ctx *)p->dlp;
      struct dlp_task *task  = (struct dlp_task *)xMapGet(c->task_map, p->rid);
      uint64_t         total = (task && task->file_size) ? task->file_size : 0;
      if (total > 0)
        snprintf(cr, sizeof(cr), "bytes %llu-%llu/%llu", (unsigned long long)p->offset,
                 (unsigned long long)(p->offset + p->length - 1), (unsigned long long)total);
      else
        snprintf(cr, sizeof(cr), "bytes %llu-%llu/*", (unsigned long long)p->offset,
                 (unsigned long long)(p->offset + p->length - 1));
      xHttpCtxSetHeader(p->http_ctx, "Content-Range", cr);
    }
    xHttpCtxSend(p->http_ctx, (const char *)p->buf, p->length);
  } else {
    xHttpCtxSetStatus(p->http_ctx, 503);
    xHttpCtxSend(p->http_ctx, "cache read error\n", 17);
  }
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
    free(pr);
    return;
  }

  struct read_done_ctx *rd = (struct read_done_ctx *)malloc(sizeof(*rd));
  if (!rd) {
    free(buf);
    xHttpCtxSetStatus(pr->http_ctx, 503);
    xHttpCtxSend(pr->http_ctx, "out of memory\n", 14);
    free(pr);
    return;
  }
  rd->http_ctx = pr->http_ctx;
  rd->dlp      = pr->dlp;
  snprintf(rd->rid, sizeof(rd->rid), "%s", pr->rid);
  rd->buf          = buf;
  rd->length       = len;
  rd->offset       = pr->offset;
  rd->has_range    = pr->has_range;
  rd->content_type = pr->content_type;

  dlp_cache_read(c->cache, pr->rid, pr->clip_id, pr->offset, buf, len, on_cache_read_done, rd);

  free(pr);
}

/* ── 0 ms timer — immediately triggers scheduler download ──────── */

static void on_tick_timer(void *arg) {
  struct dlp_task *task = (struct dlp_task *)arg;
  if (task->running && task->sched && task->sched->on_tick) task->sched->on_tick(task);
}

/* ── Main route handler ── */

static void serve_mp4(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c) {

  /* Parse Range header */
  int      has_range   = 0;
  uint64_t range_start = 0;
  uint64_t range_end   = (uint64_t)(-1);
  if (http_ctx->headers) {
    const char *p = strstr(http_ctx->headers, "Range:");
    if (p) {
      has_range = 1;
      sscanf(p, "Range: bytes=%llu-%llu", (unsigned long long *)&range_start,
             (unsigned long long *)&range_end);
    }
  }

  /* Without Range → serve first block as 200 */
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

  /* Clamp to chunk boundary */
  uint64_t boff_block = (range_start / DL_BLOCK_SIZE) * DL_BLOCK_SIZE;
  uint64_t chunk_end  = boff_block + DL_BLOCK_SIZE - 1;
  if (file_size > 0 && chunk_end >= file_size) chunk_end = file_size - 1;
  if (range_start + length - 1 > chunk_end) length = (size_t)(chunk_end - range_start + 1);

  task->read_offset = range_start;

  XDEBUGL0("REQ: rid=%s has_range=%d start=%llu len=%zu", task->rid, has_range,
           (unsigned long long)range_start, length);

  /* Cache hit — async read, response sent in on_cache_read_done */
  if (dlp_cache_is_ready(c->cache, task->rid, "0.mp4", range_start, length)) {
    XDEBUGL0("cache HIT offset=%llu len=%zu", (unsigned long long)range_start, length);
    uint8_t *buf = (uint8_t *)malloc(length);
    if (!buf) {
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return;
    }
    struct read_done_ctx *rd = (struct read_done_ctx *)malloc(sizeof(*rd));
    if (!rd) {
      free(buf);
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return;
    }
    rd->http_ctx = http_ctx;
    rd->dlp      = (dlp_ctx_t)c;
    snprintf(rd->rid, sizeof(rd->rid), "%s", task->rid);
    rd->buf          = buf;
    rd->length       = length;
    rd->offset       = range_start;
    rd->has_range    = has_range;
    rd->content_type = 0; /* mp4 */
    dlp_cache_read(c->cache, task->rid, "0.mp4", range_start, buf, length, on_cache_read_done, rd);
    return;
  }

  /* Cache miss — subscribe to bus, trigger download */
  XDEBUGL0("cache MISS offset=%llu len=%zu — deferring", (unsigned long long)range_start, length);

  struct proxy_req *pr = (struct proxy_req *)calloc(1, sizeof(*pr));
  if (!pr) {
    xHttpCtxSetStatus(http_ctx, 503);
    xHttpCtxSend(http_ctx, "out of memory\n", 14);
    return;
  }
  pr->http_ctx     = http_ctx;
  pr->dlp          = (dlp_ctx_t)c;
  pr->offset       = range_start;
  pr->length       = length;
  pr->has_range    = has_range;
  pr->content_type = 0; /* mp4 */
  snprintf(pr->rid, sizeof(pr->rid), "%s", task->rid);
  snprintf(pr->clip_id, sizeof(pr->clip_id), "0.mp4");

  char bus_key[192];
  snprintf(bus_key, sizeof(bus_key), "%s:0.mp4", task->rid);
  dlp_bus_subscribe(c->bus, bus_key, on_chunk_ready, pr);

  /* Re-check cache — may have become ready since initial check */
  if (dlp_cache_is_ready(c->cache, task->rid, "0.mp4", range_start, length)) {
    on_chunk_ready(pr);
    return;
  }

  /* Trigger download synchronously. */
  task->sched->on_tick(task);
}

/* ── Forward declarations for handlers ── */

static void serve_mp4(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c);
static void serve_m3u8(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c);
static void serve_segment(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c,
                          uint32_t seq, const char *clip_id);

/* ── Unified dispatch handler ── */

static void serve_dispatch(xHttpCtx *http_ctx, void *arg) {
  dlp_ctx_t       dlp = (dlp_ctx_t)arg;
  struct dlp_ctx *c   = (struct dlp_ctx *)dlp;

  if (strcmp(http_ctx->method, "GET") != 0) {
    xHttpCtxSetStatus(http_ctx, 405);
    xHttpCtxSend(http_ctx, "only GET\n", 9);
    return;
  }

  size_t      rid_len = 0;
  const char *rid     = xHttpCtxParam(http_ctx, "rid", &rid_len);
  size_t      seg_len = 0;
  const char *seg     = xHttpCtxParam(http_ctx, "seg", &seg_len);

  if (!rid || rid_len == 0 || !seg || seg_len == 0) {
    xHttpCtxSetStatus(http_ctx, 404);
    xHttpCtxSend(http_ctx, "missing rid or seg\n", 19);
    return;
  }

  char rid_buf[64];
  snprintf(rid_buf, sizeof(rid_buf), "%.*s", (int)rid_len, rid);

  struct dlp_task *task = (struct dlp_task *)xMapGet(c->task_map, rid_buf);
  if (!task) {
    xHttpCtxSetStatus(http_ctx, 404);
    xHttpCtxSend(http_ctx, "unknown rid\n", 12);
    return;
  }

  char seg_buf[64];
  snprintf(seg_buf, sizeof(seg_buf), "%.*s", (int)seg_len, seg);

  /* Dispatch by format */
  if (task->format == DLP_FMT_HLS) {
    if (strcmp(seg_buf, "vod.m3u8") == 0 || strstr(seg_buf, ".m3u8")) {
      serve_m3u8(http_ctx, task, c);
    } else {
      /* Segment: "N.ts" — parse seq, use seg_buf as clip_id */
      uint32_t seq = (uint32_t)strtoul(seg_buf, NULL, 10);
      serve_segment(http_ctx, task, c, seq, seg_buf);
    }
  } else {
    /* MP4 — seg is just a filename (e.g. "vod.mp4"), ignored */
    serve_mp4(http_ctx, task, c);
  }
}

/* ── HLS m3u8 serving ── */

static void serve_m3u8(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c) {
  (void)c;

  /* If playlist not yet fetched, defer — trigger scheduler tick */
  if (!task->playlist_fetched || !task->playlist) {
    XDEBUGL0("m3u8: playlist not ready, triggering fetch");
    if (task->running && task->sched && task->sched->on_tick) task->sched->on_tick(task);
    xHttpCtxSetStatus(http_ctx, 503);
    xHttpCtxSend(http_ctx, "playlist not ready\n", 19);
    return;
  }

  /* Build rewritten m3u8: replace segment URIs with ./<seq>.ts */
  struct hls_playlist *pl   = task->playlist;
  size_t               cap  = 4096;
  size_t               len  = 0;
  char                *m3u8 = (char *)malloc(cap);
  if (!m3u8) {
    xHttpCtxSetStatus(http_ctx, 503);
    xHttpCtxSend(http_ctx, "out of memory\n", 14);
    return;
  }

#define APPEND(s, n)                         \
  do {                                       \
    if (len + (n) + 1 > cap) {               \
      while (len + (n) + 1 > cap)            \
        cap *= 2;                            \
      char *nb = (char *)realloc(m3u8, cap); \
      if (!nb) {                             \
        free(m3u8);                          \
        xHttpCtxSetStatus(http_ctx, 503);    \
        xHttpCtxSend(http_ctx, "oom\n", 4);  \
        return;                              \
      }                                      \
      m3u8 = nb;                             \
    }                                        \
    memcpy(m3u8 + len, (s), (n));            \
    len += (n);                              \
  } while (0)

  APPEND("#EXTM3U\n", 8);
  {
    char line[64];
    int  nl = snprintf(line, sizeof(line), "#EXT-X-VERSION:%u\n", pl->version);
    APPEND(line, nl);
    nl = snprintf(line, sizeof(line), "#EXT-X-TARGETDURATION:%u\n", pl->target_duration);
    APPEND(line, nl);
    nl = snprintf(line, sizeof(line), "#EXT-X-MEDIA-SEQUENCE:%u\n", pl->media_seq);
    APPEND(line, nl);
  }

  for (size_t i = 0; i < pl->segment_count; i++) {
    struct hls_segment *seg = &pl->segments[i];
    char                line[128];
    int                 nl = snprintf(line, sizeof(line), "#EXTINF:%.3f,\n", seg->duration);
    APPEND(line, nl);

    if (seg->has_byterange) {
      nl = snprintf(line, sizeof(line), "#EXT-X-BYTERANGE:%llu@%llu\n",
                    (unsigned long long)seg->byte_length, (unsigned long long)seg->byte_offset);
      APPEND(line, nl);
    }

    nl = snprintf(line, sizeof(line), "./%u.ts\n", seg->seq);
    APPEND(line, nl);
  }

  if (pl->is_vod) {
    APPEND("#EXT-X-ENDLIST\n", 15);
  }
#undef APPEND

  xHttpCtxSetStatus(http_ctx, 200);
  xHttpCtxSetHeader(http_ctx, "Content-Type", "application/vnd.apple.mpegurl");
  xHttpCtxSetHeader(http_ctx, "Access-Control-Allow-Origin", "*");
  xHttpCtxSend(http_ctx, m3u8, len);
  free(m3u8);
}

/* ── HLS segment serving ── */

static void serve_segment(xHttpCtx *http_ctx, struct dlp_task *task, struct dlp_ctx *c,
                          uint32_t seq, const char *clip_id) {
  /* Update player position */
  task->read_segment = seq;

  /* Determine segment size for readiness check and read */
  size_t              seg_size = 1; /* default: just check 1 byte for readiness */
  struct hls_segment *seg      = NULL;
  if (task->playlist && seq < task->playlist->segment_count) {
    seg = &task->playlist->segments[seq];
    if (seg->has_byterange) {
      seg_size = (size_t)seg->byte_length;
    }
  }

  /* Get actual cached size (set by dlp_cache_set_file_size after download) */
  uint64_t clip_size = dlp_cache_get_size(c->cache, task->rid, clip_id);
  size_t   read_len  = clip_size > 0 ? (size_t)clip_size : seg_size;
  if (read_len == 1 && seg && seg->has_byterange) {
    read_len = (size_t)seg->byte_length;
  }

  /* Cache hit? */
  if (dlp_cache_is_ready(c->cache, task->rid, clip_id, 0, seg_size)) {
    uint8_t *buf = (uint8_t *)malloc(read_len);
    if (!buf) {
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return;
    }

    struct read_done_ctx *rd = (struct read_done_ctx *)malloc(sizeof(*rd));
    if (!rd) {
      free(buf);
      xHttpCtxSetStatus(http_ctx, 503);
      xHttpCtxSend(http_ctx, "out of memory\n", 14);
      return;
    }
    rd->http_ctx = http_ctx;
    rd->dlp      = (dlp_ctx_t)c;
    snprintf(rd->rid, sizeof(rd->rid), "%s", task->rid);
    rd->buf          = buf;
    rd->length       = read_len;
    rd->offset       = 0;
    rd->has_range    = 0;
    rd->content_type = 2; /* ts */
    dlp_cache_read(c->cache, task->rid, clip_id, 0, buf, read_len, on_cache_read_done, rd);
    return;
  }

  /* Cache miss — defer */
  XDEBUGL0("seg MISS: rid=%s seq=%u — deferring", task->rid, seq);

  struct proxy_req *pr = (struct proxy_req *)calloc(1, sizeof(*pr));
  if (!pr) {
    xHttpCtxSetStatus(http_ctx, 503);
    xHttpCtxSend(http_ctx, "out of memory\n", 14);
    return;
  }
  pr->http_ctx     = http_ctx;
  pr->dlp          = (dlp_ctx_t)c;
  pr->offset       = 0;
  pr->length       = read_len;
  pr->has_range    = 0;
  pr->content_type = 2; /* ts */
  snprintf(pr->rid, sizeof(pr->rid), "%s", task->rid);
  snprintf(pr->clip_id, sizeof(pr->clip_id), "%s", clip_id);

  char bus_key[192];
  snprintf(bus_key, sizeof(bus_key), "%s:%s", task->rid, clip_id);
  dlp_bus_subscribe(c->bus, bus_key, on_chunk_ready, pr);

  /* Re-check cache */
  if (dlp_cache_is_ready(c->cache, task->rid, clip_id, 0, seg_size)) {
    on_chunk_ready(pr);
    return;
  }

  /* Trigger scheduler */
  if (task->running && task->sched && task->sched->on_tick) task->sched->on_tick(task);
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

  /* Single unified route: GET /:rid/:seg
   * Dispatch by task format (MP4 vs HLS) inside serve_dispatch.
   * Use on_done because HLS handlers call xHttpCtxSend directly. */
  xHttpRouteConf rc = {0};
  rc.pattern        = "GET /:rid/:seg";
  rc.on_done        = serve_dispatch;
  rc.arg            = ctx;
  xHttpMuxHandle(p->mux, &rc);

  xHttpServerConf sc = {0};
  sc.resolve         = xHttpMuxResolve;
  sc.router          = p->mux;
  p->server          = xHttpServerCreate(&sc);
  if (!p->server) goto fail;

  if (xHttpServerListen(p->server, "127.0.0.1", port) != xErrno_Ok) goto fail;

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
