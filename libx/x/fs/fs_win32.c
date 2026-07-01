/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fs_win32.c - Async filesystem I/O for Windows (MSVC/MinGW)
 *
 * Uses Win32 APIs (CreateFile / ReadFile / WriteFile / CloseHandle)
 * for open/close/read/write, matching libuv's approach.  OVERLAPPED
 * structs carry the file offset so positional I/O is thread-safe
 * without any global lock.
 *
 * Metadata operations (stat, mkdir, unlink, rename) use CRT or
 * simple Win32 wrappers — they are infrequent and need no offset
 * tracking.
 */

#if defined(_WIN32)

#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

#include <x/base/event.h>

/* ────── Win32 helpers ──────────────────────────────────────────── */

#define INVALID_FILE_HANDLE ((xFile)(intptr_t)-1)

static xFile win32_open(const char *path, int oflag, int mode) {
  DWORD access = 0;
  DWORD disposition;
  DWORD attr = FILE_ATTRIBUTE_NORMAL;
  (void)mode;

  if (oflag & O_WRONLY) access = GENERIC_WRITE;
  if (oflag & O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
  if (!access) access = GENERIC_READ; /* O_RDONLY */

  /* Map POSIX open flags to CreateFile disposition */
  if (oflag & O_CREAT) {
    disposition = (oflag & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
  } else if (oflag & O_TRUNC) {
    disposition = TRUNCATE_EXISTING;
  } else {
    disposition = OPEN_EXISTING;
  }

  HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         disposition, attr, NULL);
  return (h != INVALID_HANDLE_VALUE) ? (xFile)h : INVALID_FILE_HANDLE;
}

static inline HANDLE xfile_to_handle(xFile f) {
  /* Cache / dlproxy open files via CRT open() → int fd.
   * Convert to Win32 HANDLE so ReadFile / WriteFile work. */
  return (HANDLE)_get_osfhandle((int)(intptr_t)f);
}

/* ───────────────────── Worker ───────────────────── */

static void *fs_worker(void *arg) {
  xFsReq *r = (xFsReq *)arg;
  HANDLE  h;
  BOOL    ok;
  DWORD   n;

  switch (r->op) {
  case xFsOpOpen: {
    xFile f = win32_open(r->path, r->flags, (int)r->mode);
    if (f == INVALID_FILE_HANDLE) {
      r->result   = xErrno_SysError;
      r->out_file = NULL;
    } else {
      r->result   = xErrno_Ok;
      r->out_file = f;
    }
    break;
  }

  case xFsOpClose:
    if (r->file && r->file != INVALID_FILE_HANDLE) {
      h         = xfile_to_handle(r->file);
      r->retval = CloseHandle(h) ? 0 : -1;
    } else {
      r->retval = 0;
    }
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpRead: {
    h = xfile_to_handle(r->file);
    if (!h || h == INVALID_HANDLE_VALUE) {
      r->result = xErrno_InvalidArg;
      r->retval = -1;
      r->done   = true;
      break;
    }
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(r->offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((r->offset >> 32) & 0xFFFFFFFFu);
    ok            = ReadFile(h, r->buf, (DWORD)r->len, &n, &ov);
    r->retval     = ok ? (ssize_t)n : -1;
    r->result     = ok ? xErrno_Ok : xErrno_SysError;
    r->done       = true;
    break;
  }

  case xFsOpWrite: {
    h = xfile_to_handle(r->file);
    if (!h || h == INVALID_HANDLE_VALUE) {
      r->result = xErrno_InvalidArg;
      r->retval = -1;
      r->done   = true;
      break;
    }
    OVERLAPPED ov = {0};
    ov.Offset     = (DWORD)(r->offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((r->offset >> 32) & 0xFFFFFFFFu);
    ok            = WriteFile(h, r->buf, (DWORD)r->len, &n, &ov);
    r->retval     = ok ? (ssize_t)n : -1;
    r->result     = ok ? xErrno_Ok : xErrno_SysError;
    r->done       = true;
    break;
  }

  case xFsOpStat: {
    struct _stat64 st;
    if (_stat64(r->path, &st) < 0) {
      r->result = xErrno_SysError;
      r->retval = -1;
    } else {
      r->result     = xErrno_Ok;
      r->retval     = 0;
      r->stat.size  = (uint64_t)st.st_size;
      r->stat.mode  = st.st_mode;
      r->stat.mtime = (uint64_t)st.st_mtime * 1000;
      r->stat.ctime = (uint64_t)st.st_ctime * 1000;
    }
    break;
  }

  case xFsOpMkdir:
    r->retval = _mkdir(r->path);
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpUnlink:
    r->retval = DeleteFileA(r->path) ? 0 : -1;
    r->result = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;

  case xFsOpRename: {
    const char *npath = (const char *)r->buf;
    r->retval         = MoveFileA(r->path, npath) ? 0 : -1;
    r->result         = (r->retval < 0) ? xErrno_SysError : xErrno_Ok;
    break;
  }

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

#endif /* _WIN32 */
