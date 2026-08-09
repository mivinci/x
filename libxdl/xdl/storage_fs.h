/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * storage_fs.h - Filesystem-backed storage (open + .part + rename)
 */

#ifndef XDL_STORAGE_FS_H
#define XDL_STORAGE_FS_H

#include <x/base/base.h>
#include <xdl/storage.h>

/**
 * @brief Open a filesystem-backed storage file.
 *
 * Creates / truncates <dest>.part.  When all writes are done, call
 * xdl_storage_flush() to close and rename .part → dest.
 *
 * @param dest   Destination file path.
 * @param cb     Called on completion: cb(arg, err, f).
 *               On sync error, cb is called with f=NULL before this returns.
 * @param arg    User context passed to @p cb.
 * @return 0 on submit success, -1 on sync error.
 */
XCAPI_LOCAL(int) xdl_storage_fs_open(const char *dest, xdl_storage_open_cb_t cb, void *arg);

#endif /* XDL_STORAGE_FS_H */
