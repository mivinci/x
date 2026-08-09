/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * storage.h - Storage file I/O abstraction
 *
 * A storage file is an opaque handle whose first member is a vtable.
 * Callers use the convenience inline functions (xdl_storage_write/read/...)
 * which dispatch through the vtable.  This lets decorators (e.g. cache)
 * wrap a storage file without exposing their internal layout.
 *
 * Lifecycle:
 *
 *   f = xdl_storage_fs_open(dest, cb, arg)   // factory (see storage_fs.h)
 *        → cb(arg, err, f) fires when file is ready
 *
 *   xdl_storage_write(f, off, data, len, cb, arg)
 *   xdl_storage_read (f, off, buf,  len, cb, arg)
 *
 *   xdl_storage_flush(f, cb, arg)            // rename .part → dest
 *        → cb(arg, err)
 *
 *   xdl_storage_close(f, cb, arg)            // free f (deletes .part if not flushed)
 *        → cb(arg)
 */

#ifndef XDL_STORAGE_H
#define XDL_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <x/base/base.h>
#include <x/base/error.h>

/* ─── Callbacks ─────────────────────────────────── */

XDEF_STRUCT(xdl_storage_file_t);
XDEF_STRUCT(xdl_storage_vtable_t);

typedef void (*xdl_storage_open_cb_t) (void *arg, xErrno err, xdl_storage_file_t *f);
typedef void (*xdl_storage_read_cb_t)(void *arg, const uint8_t *data, ssize_t len);
typedef void (*xdl_storage_write_cb_t)(void *arg, xErrno err, ssize_t written);
typedef void (*xdl_storage_flush_cb_t)(void *arg, xErrno err);
typedef void (*xdl_storage_close_cb_t)(void *arg);

/* ─── Vtable ───────────────────────────────────── */

XDEF_STRUCT(xdl_storage_vtable_t) {
  int  (*write)(xdl_storage_file_t *f, uint64_t offset, const uint8_t *data, size_t len,
                xdl_storage_write_cb_t cb, void *arg);
  int  (*read) (xdl_storage_file_t *f, uint64_t offset, uint8_t *buf, size_t len,
                xdl_storage_read_cb_t cb, void *arg);
  int  (*flush)(xdl_storage_file_t *f, xdl_storage_flush_cb_t cb, void *arg);
  void (*close)(xdl_storage_file_t *f, xdl_storage_close_cb_t cb, void *arg);
};

/* ─── File handle ──────────────────────────────── */

/* Defined inline so decorators can embed it as first member */
struct xdl_storage_file_t {
  xdl_storage_vtable_t *vt;
};

/* ─── Convenience dispatchers ──────────────────── */

static inline int xdl_storage_write(xdl_storage_file_t *f, uint64_t offset,
                                    const uint8_t *data, size_t len,
                                    xdl_storage_write_cb_t cb, void *arg) {
  return f->vt->write(f, offset, data, len, cb, arg);
}

static inline int xdl_storage_read(xdl_storage_file_t *f, uint64_t offset,
                                   uint8_t *buf, size_t len,
                                   xdl_storage_read_cb_t cb, void *arg) {
  return f->vt->read(f, offset, buf, len, cb, arg);
}

static inline int xdl_storage_flush(xdl_storage_file_t *f,
                                    xdl_storage_flush_cb_t cb, void *arg) {
  return f->vt->flush(f, cb, arg);
}

static inline void xdl_storage_close(xdl_storage_file_t *f,
                                     xdl_storage_close_cb_t cb, void *arg) {
  if (!f) { if (cb) cb(arg); return; }
  f->vt->close(f, cb, arg);
}

#endif /* XDL_STORAGE_H */
