/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error.h - xpp::io::Error: structured I/O error type.
 *
 * Packed into a single int32_t with 0 as the niche (Ok sentinel):
 *
 *   code > 0, bit 30 clear → OS errno (raw_os_error() returns it)
 *   code > 0, bit 30 set   → libx xErrno (raw_xerrno() returns it)
 *   code < 0               → custom ErrorKind (no errno / xErrno)
 *   code == 0              → niche (unused by Error; available for
 *                            Result niche optimization)
 *
 * Bit 30 (0x40000000) is the xErrno flag. errno and xErrno values are
 * both small (< 256), so bit 30 cleanly separates them. Bit 31 (sign)
 * separates custom kinds. sizeof(Error) == 4 on all platforms.
 *
 * io::Result<T> is a convenience alias for Result<T, io::Error>.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_IO_ERROR_H
#define XPP_IO_ERROR_H

#include <errno.h>
#include <string.h>

#include <cstdint>

#include <xpp/result.h>

#include <x/base/error.h>

namespace xpp {
namespace io {

/* ── ErrorKind ────────────────────────────────────────────────────── */

/**
 * @brief Categorizes an I/O error.
 *
 * Mirrors a subset of Rust's std::io::ErrorKind. HostNotFound is
 * xpp-specific (Rust folds DNS errors into Uncategorized).
 */
enum class ErrorKind : uint8_t {
  InvalidInput = 0,  ///< Malformed input (bad address string, NULL arg, etc.).
  HostNotFound,      ///< DNS resolution returned no results.
  AddrInUse,         ///< EADDRINUSE — address already in use.
  AddrNotAvailable,  ///< EADDRNOTAVAIL — address not available on this host.
  PermissionDenied,  ///< EACCES — permission denied.
  ConnectionRefused, ///< ECONNREFUSED — no listener at the target.
  ConnectionReset,   ///< ECONNRESET — peer reset the connection.
  BrokenPipe,        ///< EPIPE — write to a closed pipe/socket.
  TimedOut,          ///< ETIMEDOUT — operation timed out.
  Other,             ///< Other syscall error (check raw_os_error()).
};

/** @brief Human-readable name of an ErrorKind. */
inline const char *error_kind_message(ErrorKind k) noexcept {
  switch (k) {
  case ErrorKind::InvalidInput:
    return "invalid input";
  case ErrorKind::HostNotFound:
    return "host not found";
  case ErrorKind::AddrInUse:
    return "address already in use";
  case ErrorKind::AddrNotAvailable:
    return "address not available";
  case ErrorKind::PermissionDenied:
    return "permission denied";
  case ErrorKind::ConnectionRefused:
    return "connection refused";
  case ErrorKind::ConnectionReset:
    return "connection reset by peer";
  case ErrorKind::BrokenPipe:
    return "broken pipe";
  case ErrorKind::TimedOut:
    return "operation timed out";
  case ErrorKind::Other:
    return "other error";
  default:
    return "unknown error";
  }
}

/** @brief Map an errno value to its closest ErrorKind. */
inline ErrorKind error_kind_from_errno(int e) noexcept {
  switch (e) {
  case EADDRINUSE:
    return ErrorKind::AddrInUse;
  case EADDRNOTAVAIL:
    return ErrorKind::AddrNotAvailable;
  case EACCES:
    return ErrorKind::PermissionDenied;
  case ECONNREFUSED:
    return ErrorKind::ConnectionRefused;
  case ECONNRESET:
    return ErrorKind::ConnectionReset;
  case EPIPE:
    return ErrorKind::BrokenPipe;
  case ETIMEDOUT:
    return ErrorKind::TimedOut;
  default:
    return ErrorKind::Other;
  }
}

/** @brief Map a libx xErrno to its closest ErrorKind. */
inline ErrorKind error_kind_from_xerrno(xErrno e) noexcept {
  switch (e) {
  case xErrno_InvalidArg:
  case xErrno_InvalidState:
    return ErrorKind::InvalidInput;
  case xErrno_DnsNotFound:
  case xErrno_DnsError:
  case xErrno_DnsTempFail:
    return ErrorKind::HostNotFound;
  case xErrno_Timeout:
    return ErrorKind::TimedOut;
  case xErrno_AlreadyExists:
    return ErrorKind::AddrInUse;
  case xErrno_NotSupported:
    return ErrorKind::Other;
  default:
    return ErrorKind::Other;
  }
}

namespace _ {
// Encoding (int32_t):
//   0x00000001 .. 0x3FFFFFFF → OS errno
//   0x40000000 .. 0x7FFFFFFF → xErrno (low bits = xErrno value, bit 30 set)
//   0x80000000 .. 0xFFFFFFFF → custom ErrorKind (negative: -(kind+1))
//   0x00000000               → niche (Ok sentinel)
constexpr int32_t k_xerrno_flag = static_cast<int32_t>(0x40000000);

inline int32_t encode_kind(ErrorKind k) noexcept {
  return -static_cast<int32_t>(k) - 1;
}
inline int32_t encode_xerrno(xErrno e) noexcept {
  return k_xerrno_flag | static_cast<int32_t>(e);
}
inline bool is_xerrno(int32_t code) noexcept {
  return code > 0 && (code & k_xerrno_flag) != 0;
}
inline bool is_errno(int32_t code) noexcept {
  return code > 0 && !is_xerrno(code);
}
} // namespace _

/* ── Error ────────────────────────────────────────────────────────── */

/**
 * @brief Structured I/O error, packed into 4 bytes.
 *
 * Three sources are distinguishable:
 *   - **OS errno** (from `from_errno`): `raw_os_error()` returns it.
 *   - **libx xErrno** (from `from_xerrno`): `raw_xerrno()` returns it.
 *   - **Custom kind** (from `from_kind`): neither accessor returns nonzero.
 *
 * `kind()` derives the category from whichever source is set.
 * `sizeof(Error) == sizeof(int32_t) == 4` on all platforms.
 */
class Error {
public:
  /** @brief Construct from an ErrorKind with no OS errno or xErrno. */
  explicit Error(ErrorKind kind) noexcept : m_code(_::encode_kind(kind)) {}

  /** @brief Construct from an ErrorKind and an OS errno. */
  Error(ErrorKind kind, int os_errno) noexcept
      : m_code(os_errno != 0 ? static_cast<int32_t>(os_errno) : _::encode_kind(kind)) {}

  /** @brief Categorical kind of the error. */
  ErrorKind kind() const noexcept {
    if (_::is_xerrno(m_code)) return error_kind_from_xerrno(raw_xerrno());
    if (m_code > 0) return error_kind_from_errno(m_code);
    return static_cast<ErrorKind>(-m_code - 1);
  }

  /**
   * @brief Raw OS errno value, or 0 if not from a syscall.
   *
   * Pass to strerror() for a human-readable OS message.
   */
  int raw_os_error() const noexcept {
    return _::is_errno(m_code) ? m_code : 0;
  }

  /**
   * @brief Raw libx xErrno value, or xErrno_Ok if not from libx.
   *
   * Use to inspect the original libx error code when the error
   * originated from a libx API.
   */
  xErrno raw_xerrno() const noexcept {
    return _::is_xerrno(m_code) ? static_cast<xErrno>(m_code & ~_::k_xerrno_flag) : xErrno_Ok;
  }

  /**
   * @brief Human-readable message.
   *
   * Returns strerror(errno) when from a syscall, the ErrorKind
   * description for custom kinds, or a generic message for xErrno
   * sources. Not thread-safe (uses strerror); for thread-safety use
   * raw_os_error() + strerror_r().
   */
  const char *message() const noexcept {
    if (_::is_errno(m_code)) return ::strerror(m_code);
    return error_kind_message(kind());
  }

  /** @brief Construct an Error from an errno value (kind derived on demand). */
  static Error from_errno(int e) noexcept {
    return Error(e);
  }

  /** @brief Construct an Error from an ErrorKind (no OS errno). */
  static Error from_kind(ErrorKind k) noexcept {
    return Error(k);
  }

  /** @brief Construct an Error from a libx xErrno (preserves the xErrno value).
   *
   *  xErrno_Ok (0) is treated as a no-error sentinel — produces an Error
   *  indistinguishable from a zero-valued errno (raw_os_error() == 0,
   *  kind() derived via error_kind_from_errno(0)). */
  static Error from_xerrno(xErrno e) noexcept {
    if (e == xErrno_Ok) return Error(0);  // encode as errno 0 (not bit-30 xerrno)
    return Error(Encoded{}, _::encode_xerrno(e));
  }

  bool operator==(const Error &o) const noexcept {
    return m_code == o.m_code;
  }
  bool operator!=(const Error &o) const noexcept {
    return m_code != o.m_code;
  }

private:
  int32_t m_code;

  /** @brief Tag for constructing from a pre-encoded code. */
  struct Encoded {};

  /** @brief Construct from a raw errno value (kind derived on demand). */
  explicit Error(int errno_value) noexcept : m_code(static_cast<int32_t>(errno_value)) {}

  /** @brief Construct from a pre-encoded code (used by from_xerrno). */
  Error(Encoded, int32_t code) noexcept : m_code(code) {}
};

static_assert(sizeof(Error) == sizeof(int32_t),
              "io::Error must pack into 4 bytes (niche-optimized)");

/* ── Result<T> alias ──────────────────────────────────────────────── */

/**
 * @brief Convenience alias: Result<T, io::Error>.
 *
 * Mirrors Rust's std::io::Result<T>.
 */
template <class T> using Result = xpp::Result<T, Error>;

} // namespace io
} // namespace xpp

#endif // XPP_IO_ERROR_H
