/*
 * sched_hls.c - HLS VOD scheduler implementation
 *
 * HLS: multiple segments (clips), segment-level prefetch.
 * On start: fetch m3u8, parse, build segment list.
 * On tick: if remain_time < emergency, find first uncached segment
 * at or after the player's current segment and fetch it.
 */
#include "dlproxy.h"
#include "dlproxy_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x/base/event.h>
#include <x/base/log.h>
#include <x/http/client.h>

/* ── m3u8 fetch context ────────────────────────────────────────────── */

struct m3u8_fetch_ctx {
  struct dlp_task *task;
  char            *text;     /* accumulated response body */
  size_t           text_len;
  size_t           text_cap;
  int              done;
  long             status;
};

static int on_m3u8_data(const char *data, size_t len, void *arg) {
  struct m3u8_fetch_ctx *fc = (struct m3u8_fetch_ctx *)arg;
  if (fc->text_len + len + 1 > fc->text_cap) {
    size_t ncap = fc->text_cap ? fc->text_cap * 2 : 4096;
    while (ncap < fc->text_len + len + 1) ncap *= 2;
    char *n = (char *)realloc(fc->text, ncap);
    if (!n) return 1; /* abort */
    fc->text = n;
    fc->text_cap = ncap;
  }
  memcpy(fc->text + fc->text_len, data, len);
  fc->text_len += len;
  fc->text[fc->text_len] = '\0';
  return 0;
}

static void on_m3u8_done(xHttpCtx *ctx, void *arg) {
  struct m3u8_fetch_ctx *fc = (struct m3u8_fetch_ctx *)arg;
  fc->done = 1;
  fc->status = ctx->status_code;
  fc->task->playlist_fetching = false;

  if (fc->status >= 200 && fc->status < 300 && fc->text) {
    struct dlp_task *task = fc->task;
    struct hls_playlist *pl = hls_parse_playlist(fc->text, task->url);
    if (pl) {
      /* If master playlist, select first variant and fetch its URL */
      if (pl->is_master && pl->variant_count > 0) {
        XDEBUGL0("hls: master playlist, selecting variant 0: %s",
                 pl->variants[0].uri);
        snprintf(task->url, sizeof(task->url), "%s", pl->variants[0].uri);
        hls_playlist_free(pl);
        /* Re-fetch as media playlist on next tick */
        task->playlist_fetched = false;
      } else {
        /* Media playlist — store it */
        task->playlist = pl;
        task->playlist_fetched = true;
        XDEBUGL0("hls: playlist parsed, %zu segments, vod=%d",
                 pl->segment_count, pl->is_vod);
      }
    } else {
      XDEBUGL0("hls: failed to parse playlist");
    }
  }

  free(fc->text);
  free(fc);
}

/* ── Scheduler vtable implementation ───────────────────────────────── */

static void hls_on_start(struct dlp_task *task) {
  XDEBUGL0("hls task started rid=%s url=%s", task->rid, task->url);
  /* m3u8 fetch will be triggered by first on_tick */
}

static void hls_fetch_playlist(struct dlp_task *task) {
  /* Guard against concurrent fetches (tick timer + request handler) */
  if (task->playlist_fetching) return;
  task->playlist_fetching = true;

  struct dlp_ctx *c = task->ctx;
  XDEBUGL0("hls: fetching playlist %s", task->url);

  struct m3u8_fetch_ctx *fc = (struct m3u8_fetch_ctx *)calloc(1, sizeof(*fc));
  if (!fc) {
    task->playlist_fetching = false;
    return;
  }
  fc->task = task;

  dlp_http_fetch_text(c->dl_http, task->url,
                      (int (*)(const char *, size_t, void *))on_m3u8_data,
                      (void (*)(xHttpCtx *, void *))on_m3u8_done, fc);
}

static void hls_on_tick(struct dlp_task *task) {
  struct dlp_ctx *c = task->ctx;

  /* If playlist not yet fetched, fetch it and return */
  if (!task->playlist_fetched) {
    hls_fetch_playlist(task);
    return;
  }

  if (!task->playlist || task->playlist->segment_count == 0) return;

  /* Compute remain_time from contiguous cached segments */
  int remain_ms = 0;
  uint32_t seg_count = (uint32_t)task->playlist->segment_count;
  for (uint32_t i = task->read_segment; i < seg_count; i++) {
    char clip_id[32];
    snprintf(clip_id, sizeof(clip_id), "%u.ts", i);
    struct hls_segment *seg = &task->playlist->segments[i];
    /* Check if segment is fully cached — read 1 byte at offset 0 */
    if (!dlp_cache_is_ready(c->cache, task->rid, clip_id, 0, 1))
      break;
    remain_ms += (int)(seg->duration * 1000);
  }
  task->remain_time_ms = remain_ms;

  /* Three-zone decision */
  int emergency = task->emergency_ms;
  int safe      = task->safe_ms;
  bool should_pull = false;
  if (remain_ms < emergency) {
    should_pull = true;
  } else if (remain_ms < safe && task->was_pulling) {
    should_pull = true;
  }

  if (!should_pull) {
    task->was_pulling = false;
    return;
  }

  /* Find first uncached segment at or after read_segment */
  for (uint32_t i = task->read_segment; i < seg_count; i++) {
    char clip_id[32];
    snprintf(clip_id, sizeof(clip_id), "%u.ts", i);
    struct hls_segment *seg = &task->playlist->segments[i];

    /* Already cached? Skip. */
    if (dlp_cache_is_ready(c->cache, task->rid, clip_id, 0, 1)) {
      task->was_pulling = true;
      continue;
    }

    /* Duplicate fetch prevention */
    if (task->downloading_seg == i) return;

    /* Open clip and fetch segment */
    dlp_cache_open_clip(c->cache, task->rid, clip_id, 0);

    uint64_t fetch_offset = 0;
    size_t   fetch_len    = 0;

    if (seg->has_byterange) {
      fetch_offset = seg->byte_offset;
      fetch_len    = (size_t)seg->byte_length;
    }

    task->downloading_seg = i;
    XDEBUGL0("hls tick: rid=%s seg=%u remain=%dms fetching %s",
             task->rid, i, remain_ms, seg->uri);

    if (seg->has_byterange) {
      dlp_http_fetch(c->dl_http, task->rid, clip_id, seg->uri,
                     fetch_offset, fetch_len, task);
    } else {
      /* Full segment fetch — no Range header (let CDN serve full file) */
      dlp_http_fetch_full(c->dl_http, task->rid, clip_id, seg->uri, task);
    }

    task->was_pulling = true;
    return;
  }

  /* All segments cached */
  task->was_pulling = false;
}

static void hls_on_block_done(struct dlp_task *task, uint64_t offset, size_t len) {
  (void)offset;
  (void)len;
  /* Clear downloading_seg — the segment that was being fetched is done */
  task->downloading_seg = (uint32_t)-1;

  /* remain_time_ms will be recomputed on next tick */
}

static void hls_on_stop(struct dlp_task *task) {
  /* Playlist freed in dlp_task_destroy */
  (void)task;
}

const struct dlp_scheduler_vtable dlp_sched_hls = {
  .on_tick       = hls_on_tick,
  .on_block_done = hls_on_block_done,
  .on_start      = hls_on_start,
  .on_stop       = hls_on_stop,
};
