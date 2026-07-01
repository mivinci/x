/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fs.h - Async filesystem I/O via thread pool offload
 */
#ifndef XFS_FS_H
#define XFS_FS_H

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <x/base/base.h>
#include <x/base/error.h>

/* ───────────────────── Types ───────────────────── */

/** File handle (returned by xFsOpOpen). */
XDEF_HANDLE(xFile);

/** Filesystem operation type. */
XDEF_ENUM(xFsOp){
  xFsOpOpen, xFsOpClose, xFsOpRead, xFsOpWrite, xFsOpStat, xFsOpMkdir, xFsOpUnlink, xFsOpRename,
};

/** Stat result. */
XDEF_STRUCT(xFsStat) {
  off_t    size;
  int      mode;
  uint64_t mtime;
  uint64_t ctime;
};

/** Per-operation request. */
typedef struct xFsReq xFsReq;
typedef void (*xFsFunc)(xFsReq *req);

struct xFsReq {
  xFsOp op;
  /* Input */
  const char *path;
  void       *buf;
  size_t      len;
  off_t       offset;
  int         flags;
  int         mode;
  xFile       file;
  xFsFunc     cb; /* NULL = synchronous blocking */
  void       *arg;
  /* Output */
  xErrno  result;
  ssize_t retval;
  bool    done; /* streaming: true = last chunk */
  xFsStat stat;
  xFile   out_file; /* xFsOpOpen result */

  /* Internal */
  void *_work;
  void *_private;
};

/* ───────────────────── API ───────────────────── */

/**
 * @brief Submit an async filesystem operation.
 *
 * @p req->op specifies the operation. Required fields depend on op:
 *   Open  : path, flags, mode, cb
 *   Close : file, cb
 *   Read  : file, buf, len, offset, cb
 *   Write : file, buf, len, offset, cb
 *   Stat  : path, cb
 *   Mkdir : path, mode, cb
 *   Unlink: path, cb
 *   Rename: path (old), buf/offset (new name), cb
 *
 * Read/write invoke @p cb one or more times with @p done=true on the
 * final call. All callbacks run on the event loop thread.
 *
 * When @p cb is NULL, the call blocks until the operation completes
 * and the result is available in @p req.
 *
 * @param req  Request struct (must not be NULL).
 * @return     xErrno_Ok on success, xErrno_InvalidArg if req is NULL.
 */
XCAPI(xErrno) xFsReqSubmit(xFsReq *req);

/**
 * @brief Cancel a pending or running fs operation.
 *
 * After calling, @p req->cb will NOT be invoked. The caller may safely
 * release @p req and its buffers. Delegates to xWorkCancel internally.
 *
 * @param req  Request struct. NULL is safe (no-op).
 * @return     xErrno_Ok on success (xWorkCancel result).
 */
XCAPI(xErrno) xFsReqCancel(xFsReq *req);

#endif /* XFS_FS_H */
