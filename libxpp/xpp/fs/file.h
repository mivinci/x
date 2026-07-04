/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * file.h - xpp::fs::File: Promise-based async file I/O.
 *
 * Wraps libx's xfs module (thread-pool-offloaded filesystem operations)
 * into Promise-returning methods that compose with .then(), co_await,
 * all(), and race().
 *
 * Design: pread/pwrite with explicit offset (no seek). RAII destructor
 * closes synchronously. Buffer overloads: void*+size (base) and
 * Span<uint8_t> (type-safe). Errors: negative ssize_t = -errno.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_FS_FILE_H
#define XPP_FS_FILE_H

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/span.h>

#include <x/fs/fs.h>

namespace xpp {
namespace fs {

/* ── Stat ──────────────────────────────────────────────────────────── */

struct Stat {
  off_t    size;
  int      mode;
  uint64_t mtime;
  uint64_t ctime;
};

/* ── File ──────────────────────────────────────────────────────────── */

namespace _ {
class FsOpenAdapter;
}

class File {
public:
  static Promise<File> open(const char *path);
  static Promise<File> open(const char *path, int flags, int mode = 0644);
  static Promise<File> create(const char *path, int mode = 0644);
  static File          from_raw_fd(int fd);

  File() = default;
  File(File &&o) noexcept : m_fd(o.m_fd), m_open(o.m_open) {
    o.m_fd   = -1;
    o.m_open = false;
  }
  File &operator=(File &&o) noexcept {
    if (this != &o) {
      close_sync();
      m_fd     = o.m_fd;
      m_open   = o.m_open;
      o.m_fd   = -1;
      o.m_open = false;
    }
    return *this;
  }
  File(const File &)            = delete;
  File &operator=(const File &) = delete;

  ~File() {
    close_sync();
  }

  Promise<ssize_t> read(void *buf, size_t len, off_t offset);
  Promise<ssize_t> read(Span<uint8_t> buf, off_t offset) {
    return read(buf.data(), buf.size(), offset);
  }

  Promise<ssize_t> write(const void *buf, size_t len, off_t offset);
  Promise<ssize_t> write(Span<const uint8_t> buf, off_t offset) {
    return write(buf.data(), buf.size(), offset);
  }
  Promise<ssize_t> write(const std::string &s, off_t offset) {
    return write(s.data(), s.size(), offset);
  }

  Promise<void> close();
  Promise<void> sync_all();
  Promise<Stat> stat();

  Promise<std::vector<uint8_t>> read_all();
  Promise<std::string>          read_to_string();
  Promise<void>                 write_all(const void *buf, size_t len);
  Promise<void>                 write_all(const std::string &s) {
    return write_all(s.data(), s.size());
  }

  int raw_fd() const {
    return m_fd;
  }
  bool is_open() const {
    return m_open;
  }

private:
  int  m_fd   = -1;
  bool m_open = false;

  explicit File(int fd) : m_fd(fd), m_open(fd >= 0) {}
  void close_sync();

  friend class _::FsOpenAdapter;
};

/* ── Free functions ────────────────────────────────────────────────── */

Promise<Stat>                 stat(const char *path);
Promise<std::vector<uint8_t>> read(const char *path);
Promise<void>                 write(const char *path, const void *buf, size_t len);

/* ── Internal: FsAdapter (bridges xFsReq → PromiseResolver) ────────── */
namespace _ {

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

} // namespace _

/* ── Implementation ────────────────────────────────────────────────── */

inline void File::close_sync() {
  if (m_open && m_fd >= 0) {
    xFsReq req{};
    req.op   = xFsOpClose;
    req.file = reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd));
    req.cb   = nullptr;
    xFsReqSubmit(&req);
  }
  m_fd   = -1;
  m_open = false;
}

inline Promise<File> File::open(const char *path) {
  return File::open(path, O_RDONLY, 0);
}

inline Promise<File> File::open(const char *path, int flags, int mode) {
  return xpp::adapt<File, _::FsOpenAdapter>(path, flags, mode);
}

inline Promise<File> File::create(const char *path, int mode) {
  return File::open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

inline File File::from_raw_fd(int fd) {
  return File(fd);
}

inline Promise<ssize_t> File::read(void *buf, size_t len, off_t offset) {
  return xpp::adapt<ssize_t, _::FsReadAdapter>(reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)),
                                               buf, len, offset);
}

inline Promise<ssize_t> File::write(const void *buf, size_t len, off_t offset) {
  return xpp::adapt<ssize_t, _::FsWriteAdapter>(
    reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)), buf, len, offset);
}

inline Promise<void> File::close() {
  if (!m_open) return xpp::yield();
  auto p =
    xpp::adapt<void, _::FsCloseAdapter>(reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)));
  m_open = false;
  m_fd   = -1;
  return p;
}

inline Promise<void> File::sync_all() {
  if (m_fd >= 0) ::fsync(m_fd);
  return xpp::yield();
}

inline Promise<Stat> File::stat() {
  if (m_fd < 0) return xpp::resolve(Stat{-1, 0, 0, 0});
  struct ::stat st;
  if (::fstat(m_fd, &st) == 0) {
    return xpp::resolve(Stat{st.st_size, static_cast<int>(st.st_mode),
                             static_cast<uint64_t>(st.st_mtime),
                             static_cast<uint64_t>(st.st_ctime)});
  }
  return xpp::resolve(Stat{-1, 0, 0, 0});
}

inline Promise<std::vector<uint8_t>> File::read_all() {
  struct ::stat st;
  if (::fstat(m_fd, &st) != 0) return xpp::resolve(std::vector<uint8_t>{});
  size_t               file_size = static_cast<size_t>(st.st_size);
  std::vector<uint8_t> buf(file_size);
  if (file_size > 0) {
    size_t total = 0;
    while (total < file_size) {
      ssize_t n = ::pread(m_fd, buf.data() + total, file_size - total, static_cast<off_t>(total));
      if (n <= 0) break;
      total += static_cast<size_t>(n);
    }
    buf.resize(total);
  }
  return xpp::resolve(std::move(buf));
}

inline Promise<std::string> File::read_to_string() {
  struct ::stat st;
  if (::fstat(m_fd, &st) != 0) return xpp::resolve(std::string{});
  size_t      file_size = static_cast<size_t>(st.st_size);
  std::string s(file_size, '\0');
  if (file_size > 0) {
    size_t total = 0;
    while (total < file_size) {
      ssize_t n = ::pread(m_fd, &s[total], file_size - total, static_cast<off_t>(total));
      if (n <= 0) break;
      total += static_cast<size_t>(n);
    }
    s.resize(total);
  }
  return xpp::resolve(std::move(s));
}

inline Promise<void> File::write_all(const void *buf, size_t len) {
  size_t total = 0;
  while (total < len) {
    ssize_t n = ::pwrite(m_fd, static_cast<const char *>(buf) + total, len - total,
                         static_cast<off_t>(total));
    if (n <= 0) break;
    total += static_cast<size_t>(n);
  }
  return xpp::yield();
}

inline Promise<Stat> stat(const char *path) {
  return xpp::adapt<Stat, _::FsStatAdapter>(path);
}

inline Promise<std::vector<uint8_t>> read(const char *path) {
  return File::open(path).then([](File f) {
    return f.read_all().then(
      [f = std::move(f)](std::vector<uint8_t> data) { return std::move(data); });
  });
}

inline Promise<void> write(const char *path, const void *buf, size_t len) {
  return File::create(path).then(
    [buf, len](File f) { return f.write_all(buf, len).then([f = std::move(f)] {}); });
}

} // namespace fs
} // namespace xpp

#endif // XPP_FS_FILE_H
