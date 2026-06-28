/*
 * cache.c - Chunk cache with Resource → Clip → Block → Piece hierarchy
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

/* ── Piece ────────────────────────────────────────────────────────── */

struct dlp_piece {
  /* Pieces are tracked via bitmap in the block, not individual structs */
};

/* ── Block ────────────────────────────────────────────────────────── */

struct dlp_block {
  uint32_t offset;                         /* byte offset in clip file     */
  uint32_t size;                           /* actual size (last block may
                                              be smaller)                  */
  bool     done;                           /* all pieces downloaded        */
  uint8_t  pieces[DL_PIECE_BYTES];         /* 32 bytes = 256 bits         */
};

static inline bool dlp_block_done(const struct dlp_block *b) {
  return b->done;
}

static inline bool dlp_block_piece_done(const struct dlp_block *b, int p) {
  return (b->pieces[p / 8] & (1u << (p % 8))) != 0;
}

static inline void dlp_block_set_piece(struct dlp_block *b, int p) {
  b->pieces[p / 8] |= (1u << (p % 8));
}

static bool dlp_block_check_done(struct dlp_block *b) {
  if (b->done) return true;
  int n = (b->size + DL_PIECE_SIZE - 1) / DL_PIECE_SIZE;
  for (int i = 0; i < n; i++) {
    if (!dlp_block_piece_done(b, i)) return false;
  }
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
  int           fd;                        /* data file descriptor        */
  uint32_t      block_count;
  struct dlp_block **blocks;               /* sparse array, NULL if not
                                              allocated                   */
  uint64_t      total_size;               /* known total, or 0           */
};

static struct dlp_block *dlp_block_ensure(struct dlp_clip *c, uint32_t bno) {
  if (!c || bno >= c->block_count) return NULL;
  if (c->blocks[bno]) return c->blocks[bno];

  struct dlp_block *b = (struct dlp_block *)calloc(1, sizeof(*b));
  if (!b) return NULL;

  b->offset = bno * DL_BLOCK_SIZE;
  b->size   = DL_BLOCK_SIZE;
  uint64_t remaining = c->total_size - b->offset;
  if (c->total_size > 0 && remaining < DL_BLOCK_SIZE) {
    b->size = (uint32_t)remaining;
    if (b->size == 0) b->size = DL_BLOCK_SIZE; /* exact multiple */
  }
  c->blocks[bno] = b;
  return b;
}

static void dlp_clip_free(struct dlp_clip *c) {
  if (!c) return;
  if (c->fd >= 0) close(c->fd);
  for (uint32_t i = 0; i < c->block_count; i++) {
    free(c->blocks[i]);
  }
  free(c->blocks);
  free(c);
}

/* ── Resource ──────────────────────────────────────────────────────── */

struct dlp_resource {
  char rid[64];
  char dir[256];
  xMap clips;  /* clip_id → dlp_clip_t* */
};

/* ── Cache ─────────────────────────────────────────────────────────── */

struct dlp_cache {
  char dir[256];
  xMap resources;  /* rid → dlp_resource_t* */
};

dlp_cache_t dlp_cache_init(const char *dir) {
  struct dlp_cache *c = (struct dlp_cache *)calloc(1, sizeof(*c));
  if (!c) return NULL;

  snprintf(c->dir, sizeof(c->dir), "%s", dir ? dir : "./cache");
  mkdir(c->dir, 0755);

  c->resources = xMapCreate(xMapType_Hash, 64, xMapStrHash, xMapStrEq);
  if (!c->resources) { free(c); return NULL; }
  return c;
}

void dlp_cache_deinit(dlp_cache_t c) {
  if (!c) return;
  xMapDestroy(c->resources); /* TODO: free all resources */
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
  cl->total_size = size;

  /* Calculate block count */
  cl->block_count = (uint32_t)((size + DL_BLOCK_SIZE - 1) / DL_BLOCK_SIZE);
  if (cl->block_count == 0 && size > 0) cl->block_count = 1;

  cl->blocks = (struct dlp_block **)calloc(cl->block_count, sizeof(struct dlp_block *));
  if (!cl->blocks) { free(cl); return xErrno_NoMemory; }

  /* Open data file */
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.data", r->dir, clip_id);
  cl->fd = open(path, O_RDWR | O_CREAT, 0644);
  if (cl->fd < 0) {
    free(cl->blocks); free(cl);
    return xErrno_SysError;
  }

  xMapSet(r->clips, clip_id, cl);
  return xErrno_Ok;
}

/* ── I/O ───────────────────────────────────────────────────────────── */

static int dlp_cache_write_sync(struct dlp_clip *cl, uint64_t offset,
                                 const uint8_t *data, size_t len) {
  if (!cl || cl->fd < 0) return -1;

  ssize_t n = pwrite(cl->fd, data, len, (off_t)offset);
  if (n < 0) return -1;

  /* Mark blocks and pieces */
  uint64_t end = offset + len;
  for (uint64_t pos = offset; pos < end;) {
    uint32_t bno   = (uint32_t)(pos / DL_BLOCK_SIZE);
    uint64_t boff  = pos % DL_BLOCK_SIZE;
    size_t   chunk = (size_t)(DL_BLOCK_SIZE - boff);
    if (chunk > end - pos) chunk = (size_t)(end - pos);

    struct dlp_block *b = dlp_block_ensure(cl, bno);
    if (b) dlp_block_mark_range(b, boff, chunk);

    pos += chunk;
  }
  return 0;
}

static int dlp_cache_read_sync(struct dlp_clip *cl, uint64_t offset,
                                uint8_t *buf, size_t len) {
  if (!cl || cl->fd < 0) return -1;

  /* Verify readiness first */
  uint64_t end = offset + len;
  for (uint64_t pos = offset; pos < end;) {
    uint32_t bno   = (uint32_t)(pos / DL_BLOCK_SIZE);
    uint64_t boff  = pos % DL_BLOCK_SIZE;

    struct dlp_block *b = cl->blocks[bno];
    if (!b || !b->done) return -1;

    pos += (DL_BLOCK_SIZE - boff);
  }

  ssize_t n = pread(cl->fd, buf, len, (off_t)offset);
  return (n == (ssize_t)len) ? 0 : -1;
}

xErrno dlp_cache_write(dlp_cache_t c, const char *rid, uint64_t offset,
                        const uint8_t *data, size_t len) {
  if (!c || !rid || !data || len == 0) return xErrno_InvalidArg;

  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return xErrno_NotFound;

  /* Find or create default clip "0" */
  const char *clip_id = "0";
  struct dlp_clip *cl = (struct dlp_clip *)xMapGet(r->clips, clip_id);
  if (!cl) {
    dlp_cache_open_clip(c, rid, clip_id, 0);
    cl = (struct dlp_clip *)xMapGet(r->clips, clip_id);
  }
  if (!cl) return xErrno_NotFound;

  if (dlp_cache_write_sync(cl, offset, data, len) != 0) return xErrno_SysError;
  return xErrno_Ok;
}

xErrno dlp_cache_read(dlp_cache_t c, const char *rid, uint64_t offset,
                       uint8_t *buf, size_t len) {
  if (!c || !rid || !buf || len == 0) return xErrno_InvalidArg;

  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return xErrno_NotFound;

  struct dlp_clip *cl = (struct dlp_clip *)xMapGet(r->clips, "0");
  if (!cl) return xErrno_NotFound;

  if (dlp_cache_read_sync(cl, offset, buf, len) != 0) return xErrno_NotFound;
  return xErrno_Ok;
}

int dlp_cache_is_ready(dlp_cache_t c, const char *rid, uint64_t offset, size_t len) {
  if (!c || !rid || len == 0) return 0;

  struct dlp_resource *r = (struct dlp_resource *)xMapGet(c->resources, rid);
  if (!r) return 0;

  struct dlp_clip *cl = (struct dlp_clip *)xMapGet(r->clips, "0");
  if (!cl) return 0;

  uint64_t end = offset + len;
  for (uint64_t pos = offset; pos < end;) {
    uint32_t bno  = (uint32_t)(pos / DL_BLOCK_SIZE);
    uint64_t boff = pos % DL_BLOCK_SIZE;
    struct dlp_block *b = cl->blocks[bno];
    if (!b || !b->done) return 0;
    pos += (DL_BLOCK_SIZE - boff);
  }
  return 1;
}
