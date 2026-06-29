/*
 * task_mp4.c - MP4 task scheduler implementation
 *
 * MP4: single clip "0", 256KB blocks, byte-offset playback.
 * On tick: if remain_time < emergency, find first unfinished block
 * at or after the playback position and fetch it.
 */
#include "dlproxy.h"
#include "dlproxy_internal.h"

#include <string.h>
#include <x/base/event.h>
#include <x/base/log.h>

#define DL_BLOCK_SIZE (256u * 1024u)

static void mp4_on_tick(struct dlp_task *task) {
  struct dlp_ctx *c = task->ctx;
  int remain    = task->remain_time_ms;
  int emergency = task->emergency_ms;
  int safe      = task->safe_ms;

  /* Three-zone decision */
  bool should_pull = false;
  if (remain < emergency) {
    should_pull = true;
  } else if (remain < safe && task->was_pulling) {
    should_pull = true;
  }

  if (!should_pull) {
    task->was_pulling = false;
    return;
  }

  /* Find first unfinished block at or after read_offset */
  uint32_t block  = (uint32_t)(task->read_offset / DL_BLOCK_SIZE);
  uint64_t boff   = (uint64_t)block * DL_BLOCK_SIZE;

  if (dlp_cache_is_ready(c->cache, task->rid, "0", boff, DL_BLOCK_SIZE)) {
    task->was_pulling = true;
    return;
  }

  /* Avoid duplicate downloads of the same block */
  if (task->downloading_off == boff) return;

  XDEBUGL0("mp4 tick: rid=%s remain=%dms emergency=%dms block=%u offset=%llu",
           task->rid, remain, emergency, block, (unsigned long long)boff);
  task->downloading_off = boff;
  xErrno rc = dlp_http_fetch(c->dl_http, task->rid, "0", task->url, boff,
                       DL_BLOCK_SIZE, task);
  XDEBUGL0("mp4 tick: rc=%d block=%u offset=%llu", rc, block, (unsigned long long)boff);
  task->was_pulling = true;
}

static void mp4_on_block_done(struct dlp_task *task, uint64_t offset, size_t len) {
  struct dlp_ctx *c = task->ctx;
  uint64_t boff = (offset / DL_BLOCK_SIZE) * DL_BLOCK_SIZE;
  (void)len;
  if (task->downloading_off == boff) task->downloading_off = 0;

  /* Scan forward from read_offset to estimate remain_time */
  int cached_blocks = 0;
  uint64_t pos = task->read_offset;
  for (int i = 0; i < 256; i++) {
    if (!dlp_cache_is_ready(c->cache, task->rid, "0", pos, DL_BLOCK_SIZE))
      break;
    cached_blocks++;
    pos += DL_BLOCK_SIZE;
  }
  task->remain_time_ms = (int)
    ((uint64_t)cached_blocks * DL_BLOCK_SIZE * 1000 /
     (task->bitrate > 0 ? task->bitrate : 1));
}

static void mp4_on_start(struct dlp_task *task) {
  (void)task;
  XDEBUGL0("mp4 task started rid=%s", task->rid);
}

static void mp4_on_stop(struct dlp_task *task) {
  (void)task;
}

const struct dlp_scheduler_vtable dlp_sched_mp4 = {
  .on_tick       = mp4_on_tick,
  .on_block_done = mp4_on_block_done,
  .on_start      = mp4_on_start,
  .on_stop       = mp4_on_stop,
};
