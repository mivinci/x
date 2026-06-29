/*
 * cache.c - Chunk cache with Resource→Clip→Block→Piece hierarchy
 *           Async I/O via xfs module.
 */
#include "cache.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x/base/map.h>
#include <x/fs/fs.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define DL_BLOCK_KB   256
#define DL_PIECE_KB   1
#define DL_PIECES_PER_BLOCK  (DL_BLOCK_KB / DL_PIECE_KB)  /* 256 */
#define DL_BLOCK_SIZE  (DL_BLOCK_KB * 1024)
#define DL_PIECE_SIZE  (DL_PIECE_KB * 1024)
#define DL_PIECE_BYTES (DL_PIECES_PER_BLOCK / 8)          /* 32 bytes */

/* ── Block ────────────────────────────────────────────────────────── */

struct dlp_block {
  uint32_t offset;
  uint32_t size;
  bool     done;
  uint8_t  pieces[DL_PIECE_BYTES];
};

static bool dlp_block_piece_done(const struct dlp_block *b, int p) {
  return (b->pieces[p / 8] & (1u << (p % 8))) != 0;
}
static void dlp_block_set_piece(struct dlp_block *b, int p) {
  b->pieces[p / 8] |= (1u << (p % 8));
}
static bool dlp_block_check_done(struct dlp_block *b) {
  if (b->done) return true;
  int n = (int)((b->size + DL_PIECE_SIZE - 1) / DL_PIECE_SIZE);
  for (int i = 0; i < n; i++)
    if (!dlp_block_piece_done(b, i)) return false;
  b->done = true;
  return true;
}
static void dlp_block_mark_range(struct dlp_block *b, uint64_t boff, size_t len) {
  int start = (int)(boff / DL_PIECE_SIZE);
  int end   = (int)((boff + len - 1) / DL_PIECE_SIZE);
  int max   = (int)((b->size + DL_PIECE_SIZE - 1) / DL_PIECE_SIZE);
  if (end >= max) end = max - 1;
  for (int p = start; p <= end; p++) dlp_block_set_piece(b, p);
  dlp_block_check_done(b);
}

/* ── Clip ─────────────────────────────────────────────────────────── */

struct dlp_clip {
  char          id[64];
  int           fd;
  uint32_t      block_count;
  struct dlp_block **blocks;
  uint64_t      total_size;
};

static struct dlp_block *dlp_block_ensure(struct dlp_clip *c, uint32_t bno) {
  if (!c) return NULL;
  /* Auto-grow blocks array if needed (file size was unknown) */
  if (bno >= c->block_count) {
    uint32_t new_count = c->block_count * 2;
    while (bno >= new_count) new_count *= 2;
    struct dlp_block **new_blocks =
        (struct dlp_block **)realloc(c->blocks, new_count * sizeof(struct dlp_block *));
    if (!new_blocks) return NULL;
    memset(new_blocks + c->block_count, 0,
           (new_count - c->block_count) * sizeof(struct dlp_block *));
    c->blocks       = new_blocks;
    c->block_count  = new_count;
  }
  if (c->blocks[bno]) return c->blocks[bno];
  struct dlp_block *b = (struct dlp_block *)calloc(1, sizeof(*b));
  if (!b) return NULL;
  b->offset = bno * DL_BLOCK_SIZE;
  b->size   = DL_BLOCK_SIZE;
  if (c->total_size > 0) {
    uint64_t rem = c->total_size - b->offset;
    if (rem < DL_BLOCK_SIZE) b->size = (uint32_t)(rem > 0 ? rem : DL_BLOCK_SIZE);
  }
  c->blocks[bno] = b;
  return b;
}

/* ── Resource ──────────────────────────────────────────────────────── */

struct dlp_resource {
  char rid[64];
  char dir[256];
  xMap clips;
};

/* ── Cache ─────────────────────────────────────────────────────────── */

struct dlp_cache {
  char       dir[256];
  xEventLoop loop;
  xMap       resources;
};

dlp_cache_t dlp_cache_init(const char *dir, xEventLoop loop) {
  struct dlp_cache *c = (struct dlp_cache *)calloc(1, sizeof(*c));
  if (!c) return NULL;
  snprintf(c->dir, sizeof(c->dir), "%s", dir ? dir : "./cache");
  mkdir(c->dir, 0755);
  c->loop      = loop;
  c->resources = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!c->resources) { free(c); return NULL; }
  return c;
}

void dlp_cache_deinit(dlp_cache_t c) {
  if (!c) return;
  xMapDestroy(c->resources);
  free(c);
}

xErrno dlp_cache_open_resource(dlp_cache_t c, const char *rid) {
  if (!c || !rid) return xErrno_InvalidArg;
  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (r) return xErrno_Ok;
  r = (struct dlp_resource *)calloc(1, sizeof(*r));
  if (!r) return xErrno_NoMemory;
  snprintf(r->rid, sizeof(r->rid), "%s", rid);
  snprintf(r->dir, sizeof(r->dir), "%s/%s", c->dir, rid);
  mkdir(r->dir, 0755);
  r->clips = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!r->clips) { free(r); return xErrno_NoMemory; }
  xMapSet(c->resources, rid, r);
  return xErrno_Ok;
}

xErrno dlp_cache_open_clip(dlp_cache_t c, const char *rid, const char *clip_id, uint64_t size) {
  if (!c || !rid || !clip_id) return xErrno_InvalidArg;
  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return xErrno_NotFound;
  if (xMapGet(r->clips, clip_id)) return xErrno_Ok;

  struct dlp_clip *cl = (struct dlp_clip *)calloc(1, sizeof(*cl));
  if (!cl) return xErrno_NoMemory;
  snprintf(cl->id, sizeof(cl->id), "%s", clip_id);
  cl->total_size  = size;
  cl->block_count = (uint32_t)((size + DL_BLOCK_SIZE - 1) / DL_BLOCK_SIZE);
  if (cl->block_count == 0) {
    /* Unknown size — start with 4096 blocks (1 GB); grows on demand */
    cl->block_count = 4096;
  }
  cl->blocks = (struct dlp_block **)calloc(cl->block_count, sizeof(struct dlp_block *));
  if (!cl->blocks) { free(cl); return xErrno_NoMemory; }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s.data", r->dir, clip_id);
  cl->fd = open(path, O_RDWR | O_CREAT, 0644);
  if (cl->fd < 0) { free(cl->blocks); free(cl); return xErrno_SysError; }

  xMapSet(r->clips, clip_id, cl);
  return xErrno_Ok;
}

/* ── Static helpers ───────────────────────────────────────────────── */

static struct dlp_clip *cache_find_clip(struct dlp_cache *c, const char *rid, const char *clip_id) {
  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return NULL;
  return (struct dlp_clip *)xMapGet(r->clips, clip_id ? clip_id : "0");
}

/* ── Async I/O ─────────────────────────────────────────────────────── */

struct write_ctx {
  dlp_cache_cb cb;
  void        *arg;
  uint8_t     *buf;  /* copied data to keep alive during async write */
};

static void on_write_done(xFsReq *req) {
  struct write_ctx *wctx = (struct write_ctx *)req->arg;
  if (wctx->cb) wctx->cb(req->result, wctx->arg);
  free(wctx->buf);
  free(wctx);
}

xErrno dlp_cache_write(dlp_cache_t c, const char *rid, const char *clip_id,
                        uint64_t offset, const uint8_t *data, size_t len,
                        dlp_cache_cb cb, void *arg) {
  if (!c || !rid || !data || len == 0) return xErrno_InvalidArg;

  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return xErrno_NotFound;

  /* Update bitmap synchronously before dispatching async I/O */
  uint64_t end = offset + len;
  for (uint64_t pos = offset; pos < end;) {
    uint32_t bno  = (uint32_t)(pos / DL_BLOCK_SIZE);
    uint64_t boff = pos % DL_BLOCK_SIZE;
    size_t   chunk = (size_t)(DL_BLOCK_SIZE - boff);
    if (chunk > end - pos) chunk = (size_t)(end - pos);
    struct dlp_block *b = dlp_block_ensure(cl, bno);
    if (b) dlp_block_mark_range(b, boff, chunk);
    pos += chunk;
  }

  /* Allocate the req context and dispatch async write via xfs.
   * Copy data — caller's buffer may be freed before write completes. */
  struct write_ctx *wctx = (struct write_ctx *)malloc(sizeof(*wctx));
  if (!wctx) return xErrno_NoMemory;
  wctx->cb  = cb;
  wctx->arg = arg;
  wctx->buf = (uint8_t *)malloc(len);
  if (!wctx->buf) { free(wctx); return xErrno_NoMemory; }
  memcpy(wctx->buf, data, len);

  xFsReq *req = (xFsReq *)calloc(1, sizeof(*req));
  if (!req) { free(wctx->buf); free(wctx); return xErrno_NoMemory; }
  req->op     = xFsOpWrite;
  req->file   = (xFile)(intptr_t)cl->fd;
  req->buf    = wctx->buf;
  req->len    = len;
  req->offset = (off_t)offset;
  req->cb     = on_write_done;
  req->arg    = wctx;

  xErrno err = xFsReqSubmit(req);
  if (err == xErrno_Ok) {
    /* Synchronous — callback frees buf and wctx */
    if (wctx->cb) wctx->cb(req->result, wctx->arg);
    free(req);
  } else if (err != xErrno_Pending) {
    free(req); free(wctx->buf); free(wctx);
    return err;
  }
  return xErrno_Ok;
}

struct read_ctx {
  dlp_cache_cb cb;
  void        *arg;
  xFsReq       req;
};

static void on_read_done(xFsReq *req) {
  struct read_ctx *rctx = (struct read_ctx *)req->arg;
  if (rctx->cb) rctx->cb(req->result, rctx->arg);
  free(req->buf); /* buf was allocated by caller, free here */
  free(rctx);
}

xErrno dlp_cache_read(dlp_cache_t c, const char *rid, const char *clip_id,
                       uint64_t offset, uint8_t *buf, size_t len,
                       dlp_cache_cb cb, void *arg) {
  if (!c || !rid || !buf || len == 0) return xErrno_InvalidArg;

  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return xErrno_NotFound;

  /* Allocate context and dispatch async read via xfs */
  struct read_ctx *rctx = (struct read_ctx *)malloc(sizeof(*rctx));
  if (!rctx) return xErrno_NoMemory;
  rctx->cb  = cb;
  rctx->arg = arg;

  memset(&rctx->req, 0, sizeof(rctx->req));
  rctx->req.op     = xFsOpRead;
  rctx->req.file   = (xFile)(intptr_t)cl->fd;
  rctx->req.buf    = buf;
  rctx->req.len    = len;
  rctx->req.offset = (off_t)offset;
  rctx->req.cb     = on_read_done;
  rctx->req.arg    = rctx;

  xErrno err = xFsReqSubmit(&rctx->req);
  if (err != xErrno_Pending && err != xErrno_Ok) {
    free(rctx);
    return err;
  }
  return xErrno_Ok;
}

int dlp_cache_is_ready(dlp_cache_t c, const char *rid, const char *clip_id,
                        uint64_t offset, size_t len) {
  if (!c || !rid || len == 0) return 0;
  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return 0;

  uint64_t end = offset + len;
  for (uint64_t pos = offset; pos < end;) {
    uint32_t bno  = (uint32_t)(pos / DL_BLOCK_SIZE);
    uint64_t boff = pos % DL_BLOCK_SIZE;
    if (bno >= cl->block_count) return 0;
    struct dlp_block *b = cl->blocks[bno];
    if (!b || !b->done) return 0;
    pos += (DL_BLOCK_SIZE - boff);
  }
  return 1;
}
