/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fs.c - Async filesystem I/O implementation
 */
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x/base/event.h>

/* ───────────────────── Worker ───────────────────── */

static void *fs_worker(void *arg) {
  xFsReq *r = (xFsReq *)arg;
  int     fd;
  char   *npath = NULL; /* for rename: new path */

  switch (r->op) {
  case xFsOpOpen:
    r->retval  = open(r->path, r->flags, (mode_t)r->mode);
    r->result  = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    r->out_file = (r->retval >= 0) ? (xFile)(intptr_t)r->retval : NULL;
    break;

  case xFsOpClose:
    if (r->file) {
      fd = (int)(intptr_t)r->file;
      r->retval = close(fd);
    } else {
      r->retval = 0;
    }
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpRead: {
    fd = r->file ? (int)(intptr_t)r->file : -1;
    if (fd < 0) {
      r->result = xErrno_InvalidArg;
      r->retval = -1;
      r->done   = true;
      break;
    }
    size_t chunk = r->len; /* single-chunk read */
    r->retval   = pread(fd, r->buf, chunk, r->offset);
    if (r->retval < 0) {
      r->result = xErrno_SysError;
      r->done   = true;
    } else {
      r->result = xErrno_Ok;
      r->done   = true; /* v1: one-shot read */
    }
    break;
  }

  case xFsOpWrite: {
    fd = r->file ? (int)(intptr_t)r->file : -1;
    if (fd < 0) {
      r->result = xErrno_InvalidArg;
      r->retval = -1;
      r->done   = true;
      break;
    }
    r->retval = pwrite(fd, r->buf, r->len, r->offset);
    if (r->retval < 0) {
      r->result = xErrno_SysError;
    } else {
      r->result = xErrno_Ok;
    }
    r->done = true;
    break;
  }

  case xFsOpStat: {
    struct stat st;
    if (stat(r->path, &st) < 0) {
      r->result = xErrno_SysError;
      r->retval = -1;
    } else {
      r->result  = xErrno_Ok;
      r->retval  = 0;
      r->stat.size  = st.st_size;
      r->stat.mode  = st.st_mode;
      r->stat.mtime = (uint64_t)st.st_mtime * 1000;
      r->stat.ctime = (uint64_t)st.st_ctime * 1000;
    }
    break;
  }

  case xFsOpMkdir:
    r->retval = mkdir(r->path, (mode_t)r->mode);
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpUnlink:
    r->retval = unlink(r->path);
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpRename:
    npath     = (char *)r->buf; /* buf holds new path for rename */
    r->retval = rename(r->path, npath);
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  default:
    r->result = xErrno_InvalidArg;
    r->retval = -1;
    break;
  }
  return NULL;
}

/* ───────────────────── Async done callback ───────────────────── */

static void fs_done(void *arg, void *result) {
  (void)result;
  xFsReq *r = (xFsReq *)arg;
  if (r->cb) r->cb(r);
}

/* ───────────────────── Public API ───────────────────── */

xErrno xFsReqSubmit(xFsReq *req) {
  if (!req) return xErrno_InvalidArg;

  if (!req->cb) {
    /* Synchronous: run directly on calling thread */
    fs_worker(req);
    return req->result;
  }

  /* Async: offload to thread pool */
  xWork w = xWorkSubmit(NULL, fs_worker, fs_done, NULL, req);
  if (!w) return xErrno_NoMemory;
  req->_work = w;
  return xErrno_Pending;
}

xErrno xFsReqCancel(xFsReq *req) {
  if (!req || !req->_work) return xErrno_InvalidArg;
  return xWorkCancel((xWork)req->_work);
}
