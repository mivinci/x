/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * file_adapter.h — Internal: FsAdapter classes bridging xFsReq → PromiseResolver.
 *
 * Do not include directly — included by file_impl.h.
 */

#ifndef XPP_FS_FILE_ADAPTER_H
#define XPP_FS_FILE_ADAPTER_H

#include <string>

#include <xpp/promise.h>

#include <x/fs/fs.h>

namespace xpp {
namespace fs {
namespace _ {

/* ── FsAdapterBase ─────────────────────────────────────────────────── */

class FsAdapterBase {
protected:
  xFsReq m_req{};
  bool   m_done = false;

  FsAdapterBase() {
    m_req.cb = [](xFsReq *req) {
      auto *self = static_cast<FsAdapterBase *>(req->arg);
      self->on_complete();
    };
    m_req.arg = this;
  }

  ~FsAdapterBase() {
    if (!m_done) xFsReqCancel(&m_req);
  }

  FsAdapterBase(FsAdapterBase &&)            = delete;
  FsAdapterBase &operator=(FsAdapterBase &&) = delete;
  FsAdapterBase(const FsAdapterBase &)       = delete;

  virtual void on_complete() = 0;
};

/* ── Typed adapters ────────────────────────────────────────────────── */

class FsOpenAdapter : public FsAdapterBase {
private:
  PromiseResolver<File> m_resolver;

public:
  FsOpenAdapter(PromiseResolver<File> r, const char *path, int flags, int mode)
      : m_resolver(std::move(r)) {
    m_req.op    = xFsOpOpen;
    m_req.path  = path;
    m_req.flags = flags;
    m_req.mode  = mode;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    if (m_req.result == xErrno_Ok && m_req.retval >= 0) {
      m_resolver.resolve(File(static_cast<int>(reinterpret_cast<intptr_t>(m_req.out_file))));
    } else {
      m_resolver.resolve(File(-1));
    }
  }
};

class FsReadAdapter : public FsAdapterBase {
private:
  PromiseResolver<ssize_t> m_resolver;

public:
  FsReadAdapter(PromiseResolver<ssize_t> r, xFile file, void *buf, size_t len, off_t offset)
      : m_resolver(std::move(r)) {
    m_req.op     = xFsOpRead;
    m_req.file   = file;
    m_req.buf    = buf;
    m_req.len    = len;
    m_req.offset = offset;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    if (m_req.result == xErrno_Ok) {
      m_resolver.resolve(m_req.retval);
    } else {
      m_resolver.resolve(static_cast<ssize_t>(-m_req.result));
    }
  }
};

class FsWriteAdapter : public FsAdapterBase {
private:
  PromiseResolver<ssize_t> m_resolver;

public:
  FsWriteAdapter(PromiseResolver<ssize_t> r, xFile file, const void *buf, size_t len, off_t offset)
      : m_resolver(std::move(r)) {
    m_req.op     = xFsOpWrite;
    m_req.file   = file;
    m_req.buf    = const_cast<void *>(buf);
    m_req.len    = len;
    m_req.offset = offset;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    if (m_req.result == xErrno_Ok) {
      m_resolver.resolve(m_req.retval);
    } else {
      m_resolver.resolve(static_cast<ssize_t>(-m_req.result));
    }
  }
};

class FsCloseAdapter : public FsAdapterBase {
private:
  PromiseResolver<void> m_resolver;

public:
  FsCloseAdapter(PromiseResolver<void> r, xFile file) : m_resolver(std::move(r)) {
    m_req.op   = xFsOpClose;
    m_req.file = file;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    m_resolver.resolve();
  }
};

class FsStatAdapter : public FsAdapterBase {
private:
  PromiseResolver<Stat> m_resolver;

public:
  FsStatAdapter(PromiseResolver<Stat> r, const char *path) : m_resolver(std::move(r)) {
    m_req.op   = xFsOpStat;
    m_req.path = path;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    if (m_req.result == xErrno_Ok) {
      m_resolver.resolve(
        Stat{m_req.stat.size, m_req.stat.mode, m_req.stat.mtime, m_req.stat.ctime});
    } else {
      m_resolver.resolve(Stat{-1, 0, 0, 0});
    }
  }
};

class FsMkdirAdapter : public FsAdapterBase {
private:
  PromiseResolver<void> m_resolver;

public:
  FsMkdirAdapter(PromiseResolver<void> r, const char *path, int mode) : m_resolver(std::move(r)) {
    m_req.op   = xFsOpMkdir;
    m_req.path = path;
    m_req.mode = mode;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    m_resolver.resolve();
  }
};

class FsUnlinkAdapter : public FsAdapterBase {
private:
  PromiseResolver<void> m_resolver;

public:
  FsUnlinkAdapter(PromiseResolver<void> r, const char *path) : m_resolver(std::move(r)) {
    m_req.op   = xFsOpUnlink;
    m_req.path = path;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    m_resolver.resolve();
  }
};

class FsRenameAdapter : public FsAdapterBase {
private:
  PromiseResolver<void> m_resolver;
  std::string           m_new_path;

public:
  FsRenameAdapter(PromiseResolver<void> r, const char *old_path, const char *new_path)
      : m_resolver(std::move(r)), m_new_path(new_path) {
    m_req.op     = xFsOpRename;
    m_req.path   = old_path;
    m_req.buf    = const_cast<char *>(m_new_path.c_str());
    m_req.offset = static_cast<off_t>(m_new_path.size());
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    m_resolver.resolve();
  }
};

class FsRmdirAdapter : public FsAdapterBase {
private:
  PromiseResolver<void> m_resolver;

public:
  FsRmdirAdapter(PromiseResolver<void> r, const char *path) : m_resolver(std::move(r)) {
    m_req.op   = xFsOpRmdir;
    m_req.path = path;
    xFsReqSubmit(&m_req);
  }
  void on_complete() override {
    m_done = true;
    m_resolver.resolve();
  }
};

} // namespace _
} // namespace fs
} // namespace xpp

#endif // XPP_FS_FILE_ADAPTER_H
