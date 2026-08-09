/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * storage_fs.c - Filesystem-backed storage via async xfs (.part + rename)
 *
 * Layout: xdl_storage_file_t is the first member of sf_state, so casting
 * between struct sf_state * and xdl_storage_file_t * is always valid.
 */

#include <xdl/storage.h>
#include <xdl/storage_fs.h>
#include <x/base/base.h>
#include <x/fs/fs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── State ─────────────────────────────────────── */

struct sf_state {
  xdl_storage_file_t  base;   /* must be first */
  xFile               file;
  char                dest[1024];
  char                part[1056];
  bool                opened;
  bool                flushed;
  bool                closed;
};

static const xdl_storage_vtable_t kVtable;

static int sf_submit(xFsReq *req) {
  xErrno e = xFsReqSubmit(req);
  return (e == xErrno_Ok || e == xErrno_Pending) ? 0 : -1;
}

/* ─── Open ──────────────────────────────────────── */

struct sf_open_req {
  xFsReq                 req;
  xdl_storage_open_cb_t  cb;
  void                  *arg;
  struct sf_state       *st;
};

static void sf_open_cb(xFsReq *req) {
  struct sf_open_req *r = xContainerOf(req, struct sf_open_req, req);
  if (r->st->closed) {
    /* close() was already called — clean up and free */
    xFsReq req2 = {.op = xFsOpClose, .file = req->out_file};
    xFsReqSubmit(&req2);
    req2 = (xFsReq){.op = xFsOpUnlink, .path = r->st->part};
    xFsReqSubmit(&req2);
    free(r->st);
    free(r);
    return;
  }
  r->st->file   = req->out_file;
  r->st->opened = (req->result == xErrno_Ok);
  if (r->cb) r->cb(r->arg, req->result, &r->st->base);
  free(r);
}

int xdl_storage_fs_open(const char *dest, xdl_storage_open_cb_t cb, void *arg) {
  struct sf_state *st = calloc(1, sizeof(*st));
  if (!st) { cb(arg, xErrno_NoMemory, NULL); return -1; }
  st->base.vt = &kVtable;
  snprintf(st->dest, sizeof(st->dest), "%s", dest);
  snprintf(st->part, sizeof(st->part), "%s.part", dest);

  struct sf_open_req *r = calloc(1, sizeof(*r));
  if (!r) goto err_free_st;
  r->cb        = cb;
  r->arg       = arg;
  r->st        = st;
  r->req.op    = xFsOpOpen;
  r->req.path  = st->part;
  r->req.flags = O_RDWR | O_CREAT | O_TRUNC;
  r->req.mode  = 0644;
  r->req.cb    = sf_open_cb;
  if (sf_submit(&r->req) < 0) goto err_free_r;

  return 0;

err_free_r:
  free(r);
err_free_st:
  free(st);
  cb(arg, xErrno_SysError, NULL);
  return -1;
}

/* ─── Write ─────────────────────────────────────── */

struct sf_write_req {
  xFsReq                  req;
  xdl_storage_write_cb_t  cb;
  void                   *arg;
};

static void sf_write_cb(xFsReq *req) {
  struct sf_write_req *r       = xContainerOf(req, struct sf_write_req, req);
  ssize_t             written = req->retval > 0 ? req->retval : -1;
  if (r->cb) r->cb(r->arg, req->result, req->result == xErrno_Ok ? written : -1);
  free(req->buf);
  free(r);
}

static int sf_write(xdl_storage_file_t *f, uint64_t offset, const uint8_t *data, size_t len,
                    xdl_storage_write_cb_t cb, void *arg) {
  struct sf_state *st = (struct sf_state *)f;
  if (!st->opened) return -1;

  uint8_t *buf = malloc(len);
  if (!buf) return -1;
  memcpy(buf, data, len);

  struct sf_write_req *r = calloc(1, sizeof(*r));
  if (!r) goto err_free_buf;
  r->cb         = cb;
  r->arg        = arg;
  r->req.op     = xFsOpWrite;
  r->req.file   = st->file;
  r->req.buf    = buf;
  r->req.len    = len;
  r->req.offset = (off_t)offset;
  r->req.cb     = sf_write_cb;
  if (sf_submit(&r->req) < 0) goto err_free_r;

  return 0;

err_free_r:
  free(r);
err_free_buf:
  free(buf);
  return -1;
}

/* ─── Read ──────────────────────────────────────── */

struct sf_read_req {
  xFsReq                 req;
  xdl_storage_read_cb_t  cb;
  void                  *arg;
};

static void sf_read_cb(xFsReq *req) {
  struct sf_read_req *r = xContainerOf(req, struct sf_read_req, req);
  ssize_t             n = req->retval > 0 ? req->retval : -1;
  if (r->cb) r->cb(r->arg, n > 0 ? (const uint8_t *)req->buf : NULL, n);
  free(r);
}

static int sf_read(xdl_storage_file_t *f, uint64_t offset, uint8_t *buf, size_t len,
                   xdl_storage_read_cb_t cb, void *arg) {
  struct sf_state *st = (struct sf_state *)f;
  if (!st->opened) return -1;

  struct sf_read_req *r = calloc(1, sizeof(*r));
  if (!r) return -1;
  r->cb         = cb;
  r->arg        = arg;
  r->req.op     = xFsOpRead;
  r->req.file   = st->file;
  r->req.buf    = buf;
  r->req.len    = len;
  r->req.offset = (off_t)offset;
  r->req.cb     = sf_read_cb;
  if (sf_submit(&r->req) < 0) goto err_free_r;

  return 0;

err_free_r:
  free(r);
  return -1;
}

/* ─── Flush ─────────────────────────────────────── */

struct sf_flush_req {
  xFsReq                  req;
  struct sf_state        *st;
  xdl_storage_flush_cb_t  cb;
  void                   *arg;
};

static void sf_flush_rename_cb(xFsReq *req) {
  struct sf_flush_req *r = req->arg;
  xErrno              e  = req->result;
  free(req);
  if (r->cb) r->cb(r->arg, e);
  free(r);
}

static void sf_flush_close_cb(xFsReq *req) {
  struct sf_flush_req *r = xContainerOf(req, struct sf_flush_req, req);

  xFsReq *r2 = calloc(1, sizeof(*r2));
  if (!r2) {
    if (r->cb) r->cb(r->arg, xErrno_NoMemory);
    free(r);
    return;
  }
  r2->op   = xFsOpRename;
  r2->path = r->st->part;
  r2->buf  = (void *)r->st->dest;
  r2->cb   = sf_flush_rename_cb;
  r2->arg  = r;
  if (sf_submit(r2) < 0) {
    free(r2);
    if (r->cb) r->cb(r->arg, xErrno_SysError);
    free(r);
    return;
  }

  r->st->flushed = true;
}

static int sf_flush(xdl_storage_file_t *f, xdl_storage_flush_cb_t cb, void *arg) {
  struct sf_state *st = (struct sf_state *)f;
  if (!st->opened) return -1;
  st->opened = false;

  struct sf_flush_req *r = calloc(1, sizeof(*r));
  if (!r) return -1;
  r->st       = st;
  r->cb       = cb;
  r->arg      = arg;
  r->req.op   = xFsOpClose;
  r->req.file = st->file;
  r->req.cb   = sf_flush_close_cb;
  if (sf_submit(&r->req) < 0) { free(r); st->opened = true; return -1; }
  return 0;
}

/* ─── Close ─────────────────────────────────────── */

struct sf_close_req {
  xFsReq                  req;
  struct sf_state        *st;
  xdl_storage_close_cb_t cb;
  void                   *arg;
};

static void sf_close_unlink_cb(xFsReq *req) {
  struct sf_close_req *r = req->arg;
  free(req);
  if (r->cb) r->cb(r->arg);
  free(r);
}

static void sf_close_cb(xFsReq *req) {
  struct sf_close_req *r = xContainerOf(req, struct sf_close_req, req);

  xFsReq *r2 = calloc(1, sizeof(*r2));
  if (!r2) {
    if (r->cb) r->cb(r->arg);
    free(r);
    return;
  }
  r2->op   = xFsOpUnlink;
  r2->path = r->st->part;
  r2->cb   = sf_close_unlink_cb;
  r2->arg  = r;
  if (sf_submit(r2) < 0) {
    free(r2);
    if (r->cb) r->cb(r->arg);
    free(r);
    return;
  }
}

static void sf_close(xdl_storage_file_t *f, xdl_storage_close_cb_t cb, void *arg) {
  struct sf_state *st = (struct sf_state *)f;
  if (!st) { if (cb) cb(arg); return; }
  st->closed = true;

  if (st->flushed) {
    if (cb) cb(arg);
    free(st);
    return;
  }
  if (!st->opened) {
    /* Not yet opened — pending sf_open_cb will close/unlink/free */
    return;
  }

  struct sf_close_req *r = calloc(1, sizeof(*r));
  if (!r) {
    xFsReq req = {.op = xFsOpClose, .file = st->file};
    xFsReqSubmit(&req);
    req = (xFsReq){.op = xFsOpUnlink, .path = st->part};
    xFsReqSubmit(&req);
    if (cb) cb(arg);
    free(st);
    return;
  }
  r->st       = st;
  r->cb       = cb;
  r->arg      = arg;
  r->req.op   = xFsOpClose;
  r->req.file = st->file;
  r->req.cb   = sf_close_cb;
  if (sf_submit(&r->req) < 0) {
    xFsReq req = {.op = xFsOpClose, .file = st->file};
    xFsReqSubmit(&req);
    req = (xFsReq){.op = xFsOpUnlink, .path = st->part};
    xFsReqSubmit(&req);
    free(r);
    if (cb) cb(arg);
    free(st);
  }
}

/* ─── Vtable singleton ─────────────────────────── */

static const xdl_storage_vtable_t kVtable = {
    .write = sf_write,
    .read  = sf_read,
    .flush = sf_flush,
    .close = sf_close,
};
