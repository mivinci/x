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
#include <xpp/vec.h>

#include <xpp/option.h>
#include <xpp/promise.h>
#include <xpp/span.h>

#include <x/fs/fs.h>

namespace xpp {
namespace fs {

/* ── Stat ──────────────────────────────────────────────────────────── */

/** @brief File metadata returned by stat() or File::stat(). */
struct Stat {
  off_t    size;   ///< File size in bytes, or -1 on error.
  int      mode;   ///< File mode (permissions and type bits).
  uint64_t mtime;  ///< Last modification time (Unix timestamp).
  uint64_t ctime;  ///< Last status change time (Unix timestamp).
};

/* ── File ──────────────────────────────────────────────────────────── */

namespace _ {
class FsOpenAdapter;
}

/** @brief Promise-based async file handle with pread/pwrite and cursor support. */
class File {
public:
  /** @brief Open a file for reading (O_RDONLY).
   *  @param path Filesystem path.
   *  @return Promise that resolves to the opened File. */
  static Promise<File> open(const char *path);

  /** @brief Open a file with custom flags and mode.
   *  @param path Filesystem path.
   *  @param flags Open flags (O_RDONLY, O_WRONLY, O_RDWR, etc.).
   *  @param mode Permission mode (default 0644).
   *  @return Promise that resolves to the opened File. */
  static Promise<File> open(const char *path, int flags, int mode = 0644);

  /** @brief Create a new file (or truncate an existing one) for writing.
   *  @param path Filesystem path.
   *  @param mode Permission mode (default 0644).
   *  @return Promise that resolves to the created File. */
  static Promise<File> create(const char *path, int mode = 0644);

  /** @brief Wrap an existing raw file descriptor in a File handle.
   *  @param fd A valid file descriptor, or -1 for an invalid/closed handle.
   *  @return A File that takes ownership of the descriptor. */
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

  /** @brief Read from the file's internal cursor, advancing it by the bytes read.
   *  @param buf Destination buffer.
   *  @param len Maximum bytes to read.
   *  @return Promise resolving to bytes read, or -1 on error. */
  Promise<ssize_t> read(void *buf, size_t len);

  /** @brief Write at the file's internal cursor, advancing it by the bytes written.
   *  @param buf Source buffer.
   *  @param len Bytes to write.
   *  @return Promise resolving to bytes written, or -1 on error. */
  Promise<ssize_t> write(const void *buf, size_t len);

  /** @brief Read at an explicit offset without affecting the cursor.
   *  @param buf Destination buffer.
   *  @param len Maximum bytes to read.
   *  @param offset File offset to read from.
   *  @return Promise resolving to bytes read, or -1 on error. */
  Promise<ssize_t> read(void *buf, size_t len, off_t offset);
  /** @overload */
  Promise<ssize_t> read(Span<uint8_t> buf, off_t offset) {
    return read(buf.data(), buf.size(), offset);
  }

  /** @brief Write at an explicit offset without affecting the cursor.
   *  @param buf Source buffer.
   *  @param len Bytes to write.
   *  @param offset File offset to write to.
   *  @return Promise resolving to bytes written, or -1 on error. */
  Promise<ssize_t> write(const void *buf, size_t len, off_t offset);
  /** @overload */
  Promise<ssize_t> write(Span<const uint8_t> buf, off_t offset) {
    return write(buf.data(), buf.size(), offset);
  }
  /** @overload */
  Promise<ssize_t> write(const std::string &s, off_t offset) {
    return write(s.data(), s.size(), offset);
  }

  /** @brief Close the file asynchronously.
   *  @return Promise that resolves when the close completes. */
  Promise<void> close();

  /** @brief Flush all pending data to disk.
   *  @return Promise that resolves when fsync completes. */
  Promise<void> sync_all();

  /** @brief Retrieve metadata for the open file.
   *  @return Promise resolving to a Stat struct, or size=-1 on error. */
  Promise<Stat> stat();

  /** @brief Read the entire file contents into a byte vector.
   *  @return Promise resolving to the file's contents. */
  Promise<Vec<uint8_t>> read_all();

  /** @brief Read the entire file contents into a string.
   *  @return Promise resolving to the file's contents as a string. */
  Promise<std::string>          read_to_string();

  /** @brief Write all data to the file (potentially across multiple pwrite calls).
   *  @param buf Source buffer.
   *  @param len Total bytes to write.
   *  @return Promise that resolves when all data has been written. */
  Promise<void>                 write_all(const void *buf, size_t len);
  /** @overload */
  Promise<void>                 write_all(const std::string &s) {
    return write_all(s.data(), s.size());
  }

  /** @brief Return the underlying raw file descriptor. */
  int raw_fd() const {
    return m_fd;
  }
  /** @brief Return whether the file is currently open. */
  bool is_open() const {
    return m_open;
  }

private:
  int   m_fd     = -1;
  bool  m_open   = false;
  off_t m_cursor = 0;

  explicit File(int fd) : m_fd(fd), m_open(fd >= 0) {}
  /** @brief Synchronously close (used by destructor and move-assignment). */
  void close_sync();

  friend class _::FsOpenAdapter;
};

/* ── Free functions ────────────────────────────────────────────────── */

/** @brief Get file metadata by path.
 *  @param path Filesystem path.
 *  @return Promise resolving to a Stat struct, or size=-1 on error. */
Promise<Stat>                 stat(const char *path);

/** @brief Check whether a file exists at the given path.
 *  @param path Filesystem path.
 *  @return Promise resolving to true if the file exists. */
Promise<bool>                 exists(const char *path);

/** @brief Read an entire file into a byte vector.
 *  @param path Filesystem path.
 *  @return Promise resolving to the file's contents. */
Promise<Vec<uint8_t>> read(const char *path);

/** @brief Write a buffer to a file (creates/truncates the file).
 *  @param path Filesystem path.
 *  @param buf Source buffer.
 *  @param len Bytes to write.
 *  @return Promise that resolves when the write completes. */
Promise<void>                 write(const char *path, const void *buf, size_t len);

/** @brief Create a directory.
 *  @param path Directory path.
 *  @param mode Permission mode (default 0755).
 *  @return Promise that resolves when the directory is created. */
Promise<void>                 create_dir(const char *path, int mode = 0755);

/** @brief Remove (unlink) a file.
 *  @param path Filesystem path.
 *  @return Promise that resolves when the file is removed. */
Promise<void>                 remove_file(const char *path);

/** @brief Remove an empty directory.
 *  @param path Directory path.
 *  @return Promise that resolves when the directory is removed. */
Promise<void>                 remove_dir(const char *path);

/** @brief Rename a file or directory.
 *  @param old_path Current path.
 *  @param new_path New path.
 *  @return Promise that resolves when the rename completes. */
Promise<void>                 rename(const char *old_path, const char *new_path);

} // namespace fs
} // namespace xpp

/* ── Inline implementations (included after declarations) ──────────── */

#include <xpp/fs/file_impl.h>

#endif // XPP_FS_FILE_H
