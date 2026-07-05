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
 * C++11-compatible. Header-only.
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
  File(File &&o) noexcept : m_fd(o.m_fd), m_open(o.m_open), m_cursor(o.m_cursor) {
    o.m_fd     = -1;
    o.m_open   = false;
    o.m_cursor = 0;
  }
  File &operator=(File &&o) noexcept {
    if (this != &o) {
      close_sync();
      m_fd       = o.m_fd;
      m_open     = o.m_open;
      m_cursor   = o.m_cursor;
      o.m_fd     = -1;
      o.m_open   = false;
      o.m_cursor = 0;
    }
    return *this;
  }
  File(const File &)            = delete;
  File &operator=(const File &) = delete;

  ~File() {
    close_sync();
  }

  /** @brief Read from cursor (auto-advancing). */
  Promise<ssize_t> read(void *buf, size_t len);

  /** @brief Write at cursor (auto-advancing). */
  Promise<ssize_t> write(const void *buf, size_t len);

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
  int   m_fd     = -1;
  bool  m_open   = false;
  off_t m_cursor = 0;

  explicit File(int fd) : m_fd(fd), m_open(fd >= 0) {}
  void close_sync();

  friend class _::FsOpenAdapter;
};

/* ── Free functions ────────────────────────────────────────────────── */

Promise<Stat>                 stat(const char *path);
Promise<bool>                 exists(const char *path);
Promise<std::vector<uint8_t>> read(const char *path);
Promise<void>                 write(const char *path, const void *buf, size_t len);
Promise<void>                 create_dir(const char *path, int mode = 0755);
Promise<void>                 remove_file(const char *path);
Promise<void>                 remove_dir(const char *path);
Promise<void>                 rename(const char *old_path, const char *new_path);

} // namespace fs
} // namespace xpp

/* ── Inline implementations (included after declarations) ──────────── */

#include <xpp/fs/file_impl.h>

#endif // XPP_FS_FILE_H
