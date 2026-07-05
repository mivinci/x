/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * file_impl.h — Internal: inline implementations for fs::File and free functions.
 *
 * Do not include directly — included at the bottom of file.h.
 */

#ifndef XPP_FS_FILE_IMPL_H
#define XPP_FS_FILE_IMPL_H

#include <xpp/fs/file_adapter.h>

namespace xpp {
namespace fs {

/* ── File methods ──────────────────────────────────────────────────── */

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

inline Promise<ssize_t> File::read(void *buf, size_t len) {
  off_t off = m_cursor;
  if (!m_open) return xpp::resolve(static_cast<ssize_t>(-1));
  return xpp::adapt<ssize_t, _::FsReadAdapter>(reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)),
                                               buf, len, off)
    .then([this, len](ssize_t n) {
      if (n > 0) m_cursor += n;
      return n;
    });
}

inline Promise<ssize_t> File::write(const void *buf, size_t len) {
  off_t off = m_cursor;
  if (!m_open) return xpp::resolve(static_cast<ssize_t>(-1));
  return xpp::adapt<ssize_t, _::FsWriteAdapter>(
           reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)), buf, len, off)
    .then([this, len](ssize_t n) {
      if (n > 0) m_cursor += n;
      return n;
    });
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
  if (m_fd < 0) return xpp::yield();
  int fd = m_fd;
  return xpp::work([fd] { ::fsync(fd); });
}

inline Promise<Stat> File::stat() {
  if (m_fd < 0) return xpp::resolve(Stat{-1, 0, 0, 0});
  int fd = m_fd;
  return xpp::work([fd]() -> Stat {
    struct ::stat st;
    if (::fstat(fd, &st) == 0) {
      return Stat{st.st_size, static_cast<int>(st.st_mode), static_cast<uint64_t>(st.st_mtime),
                  static_cast<uint64_t>(st.st_ctime)};
    }
    return Stat{-1, 0, 0, 0};
  });
}

inline Promise<std::vector<uint8_t>> File::read_all() {
  if (m_fd < 0) return xpp::resolve(std::vector<uint8_t>{});
  int fd = m_fd;
  return xpp::work([fd]() -> std::vector<uint8_t> {
    struct ::stat st;
    if (::fstat(fd, &st) != 0) return {};
    size_t               file_size = static_cast<size_t>(st.st_size);
    std::vector<uint8_t> buf(file_size);
    if (file_size > 0) {
      size_t total = 0;
      while (total < file_size) {
        ssize_t n = ::pread(fd, buf.data() + total, file_size - total, static_cast<off_t>(total));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
      }
      buf.resize(total);
    }
    return buf;
  });
}

inline Promise<std::string> File::read_to_string() {
  if (m_fd < 0) return xpp::resolve(std::string{});
  int fd = m_fd;
  return xpp::work([fd]() -> std::string {
    struct ::stat st;
    if (::fstat(fd, &st) != 0) return {};
    size_t      file_size = static_cast<size_t>(st.st_size);
    std::string s(file_size, '\0');
    if (file_size > 0) {
      size_t total = 0;
      while (total < file_size) {
        ssize_t n = ::pread(fd, &s[total], file_size - total, static_cast<off_t>(total));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
      }
      s.resize(total);
    }
    return s;
  });
}

inline Promise<void> File::write_all(const void *buf, size_t len) {
  if (m_fd < 0) return xpp::yield();
  int                  fd = m_fd;
  std::vector<uint8_t> data(static_cast<const uint8_t *>(buf),
                            static_cast<const uint8_t *>(buf) + len);
  return xpp::work([fd, data = std::move(data)] {
    size_t total = 0;
    while (total < data.size()) {
      ssize_t n = ::pwrite(fd, data.data() + total, data.size() - total, static_cast<off_t>(total));
      if (n <= 0) break;
      total += static_cast<size_t>(n);
    }
  });
}

/* ── Free functions ────────────────────────────────────────────────── */

inline Promise<Stat> stat(const char *path) {
  return xpp::adapt<Stat, _::FsStatAdapter>(path);
}

inline Promise<bool> exists(const char *path) {
  return stat(path).then([](Stat s) { return s.size >= 0; });
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

inline Promise<void> create_dir(const char *path, int mode) {
  return xpp::adapt<void, _::FsMkdirAdapter>(path, mode);
}

inline Promise<void> remove_file(const char *path) {
  return xpp::adapt<void, _::FsUnlinkAdapter>(path);
}

inline Promise<void> remove_dir(const char *path) {
  return xpp::adapt<void, _::FsRmdirAdapter>(path);
}

inline Promise<void> rename(const char *old_path, const char *new_path) {
  return xpp::adapt<void, _::FsRenameAdapter>(old_path, new_path);
}

} // namespace fs
} // namespace xpp

#endif // XPP_FS_FILE_IMPL_H
