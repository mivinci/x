/*
 * cache.c - Chunk cache with Resource->Clip->Block->Piece hierarchy
 *           Async I/O via xfs module.
 *           .meta file persistence for bitmap resume across restarts.
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

/* -- Constants ---------------------------------------------------- */

#define DL_BLOCK_KB   256
#define DL_PIECE_KB   1
#define DL_PIECES_PER_BLOCK  (DL_BLOCK_KB / DL_PIECE_KB)  /* 256 */
#define DL_BLOCK_SIZE  (DL_BLOCK_KB * 1024)
#define DL_PIECE_SIZE  (DL_PIECE_KB * 1024)
#define DL_PIECE_BYTES (DL_PIECES_PER_BLOCK / 8)          /* 32 bytes */

/* .meta file format:
 *   [16]  magic: "DLPROXY_META\0\0\0\0"
 *   [ 4]  block_count  (uint32 LE)
 *   [ 4]  block_size   (uint32 LE)
 *   [ 8]  total_size   (uint64 LE)
 *   [ N*32 ] bitmap: block_count * DL_PIECE_BYTES bytes of raw pieces[]
 */
#define META_MAGIC "DLPROXY_META\0\0\0\0"
#define META_MAGIC_LEN 16
#define META_HEADER_LEN (META_MAGIC_LEN + 4 + 4 + 8)

/* -- Block -------------------------------------------------------- */

struct dlp_block {
  uint32_t offset;
  uint32_t size;
  bool     done;
  bool     meta_dirty;  /* pieces changed since last .meta save */
  uint8_t  pieces[DL_PIECE_BYTES];
};

static bool dlp_block_piece_done(const struct dlp_block *b, int p) {
  return (b->pieces[p / 8] & (1u << (p % 8))) != 0;
}
static void dlp_block_set_piece(struct dlp_block *b, int p) {
  b->pieces[p / 8] |= (1u << (p % 8));
  b->meta_dirty = true;
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

/* -- Clip --------------------------------------------------------- */

struct dlp_clip {
  char          id[64];
  int           fd;
  uint32_t      block_count;
  struct dlp_block **blocks;
  uint64_t      total_size;
};

static struct dlp_block *dlp_block_ensure(struct dlp_clip *c, uint32_t bno) {
  if (!c) return NULL;
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

/* .meta path helper */
static void meta_path(const char *res_dir, const char *clip_id, char *out, size_t out_len) {
  snprintf(out, out_len, "%s/%s.meta", res_dir, clip_id);
}

/* ── .meta Save / Load / Delete ──────────────────────────────────────
 *
 * Use xfs in synchronous mode (cb = NULL) — all I/O goes through the
 * same backend, so platform differences (POSIX vs Windows) are
 * handled transparently.  The .meta file is small (few KB), so
 * blocking the calling thread for sub-ms is acceptable. */

static void meta_save(struct dlp_clip *cl, const char *res_dir) {
  char path[512];
  meta_path(res_dir, cl->id, path, sizeof(path));

  size_t  bitmap_bytes = (size_t)cl->block_count * DL_PIECE_BYTES;
  size_t  total        = META_HEADER_LEN + bitmap_bytes;
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return;

  memcpy(buf, META_MAGIC, META_MAGIC_LEN);
  uint32_t bc = cl->block_count;
  uint32_t bs = DL_BLOCK_SIZE;
  uint64_t ts = cl->total_size;
  memcpy(buf + META_MAGIC_LEN,      &bc, 4);
  memcpy(buf + META_MAGIC_LEN + 4,  &bs, 4);
  memcpy(buf + META_MAGIC_LEN + 8,  &ts, 8);

  uint8_t *bp = buf + META_HEADER_LEN;
  for (uint32_t i = 0; i < cl->block_count; i++) {
    if (cl->blocks[i])
      memcpy(bp, cl->blocks[i]->pieces, DL_PIECE_BYTES);
    else
      memset(bp, 0, DL_PIECE_BYTES);
    bp += DL_PIECE_BYTES;
  }

  /* Sync open → write → close via xfs (cb=NULL = sync on caller thread) */
  xFsReq r = {0};
  r.op = xFsOpOpen; r.path = path;
  r.flags = O_WRONLY | O_CREAT | O_TRUNC; r.mode = 0644;
  if (xFsReqSubmit(&r) != xErrno_Ok || !r.out_file) { free(buf); return; }

  r.op = xFsOpWrite; r.file = r.out_file;
  r.buf = buf; r.len = total; r.offset = 0;
  xFsReqSubmit(&r);

  r.op = xFsOpClose;
  xFsReqSubmit(&r);
  free(buf);
}

static bool meta_load(struct dlp_clip *cl, const char *res_dir) {
  char path[512];
  meta_path(res_dir, cl->id, path, sizeof(path));

  /* Stat to check existence and size */
  xFsReq r = {0};
  r.op = xFsOpStat; r.path = path;
  if (xFsReqSubmit(&r) != xErrno_Ok) return false;

  size_t expect_size = META_HEADER_LEN + (size_t)cl->block_count * DL_PIECE_BYTES;
  if ((size_t)r.stat.size < META_HEADER_LEN) return false;

  /* Open + read + close via sync xfs */
  uint8_t *buf = (uint8_t *)malloc((size_t)r.stat.size);
  if (!buf) return false;

  r.op = xFsOpOpen; r.path = path; r.flags = O_RDONLY; r.mode = 0;
  if (xFsReqSubmit(&r) != xErrno_Ok || !r.out_file) { free(buf); return false; }

  r.op = xFsOpRead; r.file = r.out_file;
  r.buf = buf; r.len = (size_t)r.stat.size; r.offset = 0;
  xFsReqSubmit(&r);
  if (r.retval < (ssize_t)META_HEADER_LEN) {
    r.op = xFsOpClose; xFsReqSubmit(&r);
    free(buf); return false;
  }

  r.op = xFsOpClose; xFsReqSubmit(&r);

  /* Validate magic and header */
  if (memcmp(buf, META_MAGIC, META_MAGIC_LEN) != 0) { free(buf); return false; }

  uint32_t bc = 0, bs = 0; uint64_t ts = 0;
  memcpy(&bc, buf + META_MAGIC_LEN,      4);
  memcpy(&bs, buf + META_MAGIC_LEN + 4,  4);
  memcpy(&ts, buf + META_MAGIC_LEN + 8,  8);

  if (bc != cl->block_count || bs != DL_BLOCK_SIZE || ts != cl->total_size) {
    free(buf);
    r.op = xFsOpUnlink; r.path = path; xFsReqSubmit(&r);
    return false;
  }

  /* Restore piece bitmaps */
  uint8_t *bp = buf + META_HEADER_LEN;
  for (uint32_t i = 0; i < cl->block_count; i++) {
    struct dlp_block *b = cl->blocks[i];
    if (!b) { b = dlp_block_ensure(cl, i); if (!b) continue; }
    memcpy(b->pieces, bp, DL_PIECE_BYTES);
    b->meta_dirty = false;
    dlp_block_check_done(b);
    bp += DL_PIECE_BYTES;
  }

  free(buf);

  /* If all done, delete meta */
  bool all_done = true;
  for (uint32_t i = 0; i < cl->block_count; i++) {
    if (!cl->blocks[i] || !cl->blocks[i]->done) { all_done = false; break; }
  }
  if (all_done) {
    r.op = xFsOpUnlink; r.path = path; xFsReqSubmit(&r);
  }
  return true;
}

static void meta_delete(struct dlp_clip *cl, const char *res_dir) {
  char path[512];
  meta_path(res_dir, cl->id, path, sizeof(path));
  xFsReq r = {0};
  r.op = xFsOpUnlink; r.path = path;
  xFsReqSubmit(&r);
}

/* -- Resource ----------------------------------------------------- */

struct dlp_resource {
  char rid[64];
  char dir[256];
  xMap clips;
};

/* -- Cache -------------------------------------------------------- */

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
    cl->block_count = 4096;
  }
  cl->blocks = (struct dlp_block **)calloc(cl->block_count, sizeof(struct dlp_block *));
  if (!cl->blocks) { free(cl); return xErrno_NoMemory; }

  char path[512];
  snprintf(path, sizeof(path), "%s/%s.data", r->dir, clip_id);
  cl->fd = open(path, O_RDWR | O_CREAT, 0644);
  if (cl->fd < 0) { free(cl->blocks); free(cl); return xErrno_SysError; }

  xMapSet(r->clips, strdup(clip_id), cl);

  /* Try to load persisted bitmap from .meta file */
  meta_load(cl, r->dir);

  return xErrno_Ok;
}

/* -- Static helpers ------------------------------------------------ */

static struct dlp_clip *cache_find_clip(struct dlp_cache *c, const char *rid, const char *clip_id) {
  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return NULL;
  return (struct dlp_clip *)xMapGet(r->clips, clip_id ? clip_id : "0");
}

static struct dlp_resource *cache_find_resource(struct dlp_cache *c, const char *rid) {
  return (struct dlp_resource *)xMapGet(c->resources, rid);
}

/* Check if all blocks in a clip are done; if so delete .meta */
static void cache_check_all_done(struct dlp_clip *cl, struct dlp_resource *r) {
  if (!cl || !r) return;
  for (uint32_t i = 0; i < cl->block_count; i++) {
    if (!cl->blocks[i] || !cl->blocks[i]->done) return;
  }
  meta_delete(cl, r->dir);
}

xErrno dlp_cache_set_file_size(dlp_cache_t c, const char *rid, const char *clip_id,
                                uint64_t file_size) {
  if (!c || !rid || file_size == 0) return xErrno_InvalidArg;
  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return xErrno_NotFound;

  cl->total_size = file_size;

  /* Recalculate last block's size and re-check done status */
  uint32_t last_bno = (uint32_t)((file_size - 1) / DL_BLOCK_SIZE);
  if (last_bno < cl->block_count && cl->blocks[last_bno]) {
    struct dlp_block *b = cl->blocks[last_bno];
    uint64_t rem = file_size - b->offset;
    b->size = (uint32_t)(rem > 0 ? rem : DL_BLOCK_SIZE);
    dlp_block_check_done(b);
  }

  return xErrno_Ok;
}

/* -- Async I/O ---------------------------------------------------- */

struct write_ctx {
  dlp_cache_cb cb;
  void        *arg;
  uint8_t     *buf;
  struct dlp_clip      *clip;
  struct dlp_resource  *resource;
};

static void on_write_done(xFsReq *req) {
  struct write_ctx *wctx = (struct write_ctx *)req->arg;

  /* Save dirty block bitmaps to .meta */
  if (wctx->clip && wctx->resource) {
    bool any_dirty = false;
    for (uint32_t i = 0; i < wctx->clip->block_count; i++) {
      if (wctx->clip->blocks[i] && wctx->clip->blocks[i]->meta_dirty) {
        any_dirty = true;
        break;
      }
    }
    if (any_dirty) {
      meta_save(wctx->clip, wctx->resource->dir);
      for (uint32_t i = 0; i < wctx->clip->block_count; i++) {
        if (wctx->clip->blocks[i]) wctx->clip->blocks[i]->meta_dirty = false;
      }
    }
    /* Check if all blocks are now done */
    cache_check_all_done(wctx->clip, wctx->resource);
  }

  if (wctx->cb) wctx->cb(req->result, wctx->arg);
  free(wctx->buf);
  free(wctx);
  free(req);
}

xErrno dlp_cache_write(dlp_cache_t c, const char *rid, const char *clip_id,
                        uint64_t offset, const uint8_t *data, size_t len,
                        dlp_cache_cb cb, void *arg) {
  if (!c || !rid || !data || len == 0) return xErrno_InvalidArg;

  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return xErrno_NotFound;

  struct dlp_resource *r = cache_find_resource(c, rid);

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

  struct write_ctx *wctx = (struct write_ctx *)malloc(sizeof(*wctx));
  if (!wctx) return xErrno_NoMemory;
  wctx->cb       = cb;
  wctx->arg      = arg;
  wctx->clip     = cl;
  wctx->resource = r;
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
  if (err != xErrno_Pending) {
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
  free(rctx);
}

xErrno dlp_cache_read(dlp_cache_t c, const char *rid, const char *clip_id,
                       uint64_t offset, uint8_t *buf, size_t len,
                       dlp_cache_cb cb, void *arg) {
  if (!c || !rid || !buf || len == 0) return xErrno_InvalidArg;

  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return xErrno_NotFound;

  if (!dlp_cache_is_ready(c, rid, clip_id, offset, len))
    return xErrno_NotFound;

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

uint64_t dlp_cache_get_size(dlp_cache_t c, const char *rid, const char *clip_id) {
  struct dlp_clip *cl = cache_find_clip(c, rid, clip_id);
  if (!cl) return 0;
  return cl->total_size;
}
