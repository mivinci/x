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

#include <cstddef>
#include <cstring>

#include <xpp/fs/file_adapter.h>

namespace xpp {
namespace fs {

/* ── File methods ──────────────────────────────────────────────────── */

/** @brief Synchronously close (used by destructor and move-assignment). */
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

/** @brief Default-open for reading (O_RDONLY, default mode). */
inline Promise<File> File::open(const char *path) {
  return File::open(path, O_RDONLY, 0);
}

/** @brief Open a file via the FsOpenAdapter. */
inline Promise<File> File::open(const char *path, int flags, int mode) {
  return xpp::adapt<File, _::FsOpenAdapter>(path, flags, mode);
}

/** @brief Create a file via O_WRONLY|O_CREAT|O_TRUNC. */
inline Promise<File> File::create(const char *path, int mode) {
  return File::open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

/** @brief Wrap a raw file descriptor. */
inline File File::from_raw_fd(int fd) {
  return File(fd);
}

/** @brief Cursor-based read via FsReadAdapter. */
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

/** @brief Cursor-based write via FsWriteAdapter. */
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

/** @brief Offset-based read via FsReadAdapter. */
inline Promise<ssize_t> File::read(void *buf, size_t len, off_t offset) {
  return xpp::adapt<ssize_t, _::FsReadAdapter>(reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)),
                                               buf, len, offset);
}

/** @brief Offset-based write via FsWriteAdapter. */
inline Promise<ssize_t> File::write(const void *buf, size_t len, off_t offset) {
  return xpp::adapt<ssize_t, _::FsWriteAdapter>(
    reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)), buf, len, offset);
}

/** @brief Async close via FsCloseAdapter. */
inline Promise<void> File::close() {
  if (!m_open) return xpp::yield();
  auto p =
    xpp::adapt<void, _::FsCloseAdapter>(reinterpret_cast<xFile>(static_cast<intptr_t>(m_fd)));
  m_open = false;
  m_fd   = -1;
  return p;
}

/** @brief fsync the file via worker thread. */
inline Promise<void> File::sync_all() {
  if (m_fd < 0) return xpp::yield();
  int fd = m_fd;
  return xpp::work([fd] { ::fsync(fd); });
}

/** @brief fstat the file via worker thread. */
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

/** @brief Read the entire file into a byte vector using worker-thread pread loop. */
inline Promise<Vec<uint8_t>> File::read_all() {
  if (m_fd < 0) return xpp::resolve(Vec<uint8_t>{});
  int fd = m_fd;
  return xpp::work([fd]() -> Vec<uint8_t> {
    struct ::stat st;
    if (::fstat(fd, &st) != 0) return {};
    size_t       file_size = static_cast<size_t>(st.st_size);
    Vec<uint8_t> buf;
    buf.resize(file_size, static_cast<uint8_t>(0));
    if (file_size > 0) {
      size_t total = 0;
      while (total < file_size) {
        ssize_t n = ::pread(fd, buf.data() + total, file_size - total, static_cast<off_t>(total));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
      }
      if (total < file_size) {
        buf.resize(total, static_cast<uint8_t>(0));
      }
    }
    return buf;
  });
}

/** @brief Read the entire file into a string using worker-thread pread loop. */
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

/** @brief Write all data via worker-thread pwrite loop. */
inline Promise<void> File::write_all(const void *buf, size_t len) {
  if (m_fd < 0) return xpp::yield();
  int                  fd = m_fd;
  Vec<uint8_t> data;
  data.resize(len, static_cast<uint8_t>(0));
  std::memcpy(data.data(), buf, len);
  return xpp::work([fd, data = std::move(data)] {
    size_t total = 0;
    while (total < data.len()) {
      ssize_t n = ::pwrite(fd, data.data() + total, data.len() - total, static_cast<off_t>(total));
      if (n <= 0) break;
      total += static_cast<size_t>(n);
    }
  });
}

/* ── Free functions ────────────────────────────────────────────────── */

/** @brief Stat a path via FsStatAdapter. */
inline Promise<Stat> stat(const char *path) {
  return xpp::adapt<Stat, _::FsStatAdapter>(path);
}

/** @brief Check if a path exists by stat'ing it. */
inline Promise<bool> exists(const char *path) {
  return stat(path).then([](Stat s) { return s.size >= 0; });
}

/** @brief Open and read an entire file into a byte vector. */
inline Promise<Vec<uint8_t>> read(const char *path) {
  return File::open(path).then([](File f) {
    return f.read_all().then(
      [f = std::move(f)](Vec<uint8_t> data) { return std::move(data); });
  });
}

/** @brief Create a file and write the buffer, then close. */
inline Promise<void> write(const char *path, const void *buf, size_t len) {
  return File::create(path).then(
    [buf, len](File f) { return f.write_all(buf, len).then([f = std::move(f)] {}); });
}

/** @brief Create a directory via FsMkdirAdapter. */
inline Promise<void> create_dir(const char *path, int mode) {
  return xpp::adapt<void, _::FsMkdirAdapter>(path, mode);
}

/** @brief Unlink a file via FsUnlinkAdapter. */
inline Promise<void> remove_file(const char *path) {
  return xpp::adapt<void, _::FsUnlinkAdapter>(path);
}

/** @brief Remove a directory via FsRmdirAdapter. */
inline Promise<void> remove_dir(const char *path) {
  return xpp::adapt<void, _::FsRmdirAdapter>(path);
}

/** @brief Rename a file or directory via FsRenameAdapter. */
inline Promise<void> rename(const char *old_path, const char *new_path) {
  return xpp::adapt<void, _::FsRenameAdapter>(old_path, new_path);
}

} // namespace fs
} // namespace xpp

#endif // XPP_FS_FILE_IMPL_H
