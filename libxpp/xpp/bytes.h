/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bytes.h — Bytes: refcounted immutable byte block with O(1) copy and slice.
 *
 * Modeled after Rust's bytes::Bytes. Complements xpp::String and xpp::Vec as
 * a basic type for byte-oriented data (HTTP bodies, file I/O buffers, etc.).
 *
 * Design:
 *   - Copies are O(1) — reference count increment only.
 *   - Slices are O(1) — adjust offset + length, share the underlying buffer.
 *   - The default-constructed (empty) state holds a null Shared<Impl>.
 *   - sizeof(Bytes) == sizeof(Shared<Impl>) + 2*sizeof(size_t) == 24 on
 *     64-bit (Shared<Impl> is one pointer — see xpp/rc.h / xpp/arc.h).
 *
 * Shared<Impl> resolves to Rc<Impl> by default (single-threaded) and
 * Arc<Impl> when -DXPP_MT is defined (multi-threaded). Bytes is therefore
 * safe to move across threads only when built with -DXPP_MT.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_BYTES_H
#define XPP_BYTES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/shared.h>
#include <xpp/span.h>
#include <xpp/string.h>
#include <xpp/vec.h>

namespace xpp {

/* ── Internal: safe UTF-8 step for Bytes::to_string_lossy ────────── */
namespace _ {

/**
 * @brief Try to consume one UTF-8 codepoint starting at @p p (within @p len bytes).
 *
 * @return The number of bytes consumed (1-4) if the sequence is a valid
 *         codepoint; 0 if @p p points at an invalid byte or a truncated
 *         sequence. Caller should emit U+FFFD and advance by 1 on 0.
 *
 * Unlike @ref decode_one, this function does NOT assume valid UTF-8 —
 * it validates before consuming. Used only by Bytes::to_string_lossy.
 */
inline size_t safe_utf8_step(const uint8_t* p, size_t len) {
  if (len == 0) return 0;
  uint8_t b = p[0];
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) {
    if (len < 2 || (p[1] & 0xC0) != 0x80) return 0;
    uint32_t cp = ((b & 0x1F) << 6) | (p[1] & 0x3F);
    if (cp < 0x80) return 0;  // overlong
    return 2;
  }
  if ((b & 0xF0) == 0xE0) {
    if (len < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
    uint32_t cp = ((b & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (cp < 0x800) return 0;  // overlong
    if (0xD800 <= cp && cp <= 0xDFFF) return 0;  // surrogate
    return 3;
  }
  if ((b & 0xF8) == 0xF0) {
    if (len < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
      return 0;
    uint32_t cp = ((b & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    if (cp < 0x10000) return 0;  // overlong
    if (cp > 0x10FFFF) return 0;  // beyond Unicode
    return 4;
  }
  return 0;  // invalid leading byte
}

}  // namespace _

/**
 * @brief Immutable, reference-counted byte block.
 *
 * Copies and slices are O(1) — they share the underlying buffer via
 * @ref Shared<Impl>. Use `Bytes` for HTTP body chunks, file I/O buffers,
 * and any byte data that needs cheap slicing or shared ownership.
 *
 * For a growable, uniquely-owned byte buffer, use `Vec<uint8_t>` instead.
 * For a UTF-8 validated byte buffer, use `String` instead.
 *
 * @see Shared  Rc/Arc selection at compile time
 * @see Vec     Growable buffer (unique ownership)
 * @see String  UTF-8 validated bytes
 */
class Bytes {
public:
  /* ── Nested types ───────────────────────────────────────────────── */

  /**
   * @brief Internal storage: the byte buffer + (under -DXPP_MT) refcount.
   *
   * `Bytes` holds a `Shared<Impl>` and an offset/length pair into the
   * Impl's buffer. Slicing just adjusts offset/length — no copy.
   */
  struct Impl {
    Vec<uint8_t> buf;

    Impl() = default;
    explicit Impl(Vec<uint8_t> b) : buf(std::move(b)) {}
  };

  /* ── Construction ───────────────────────────────────────────────── */

  /** @brief Empty bytes. No allocation. */
  Bytes() noexcept : m_impl(none), m_offset(0), m_len(0) {}

  Bytes(const Bytes&)            = default;
  Bytes& operator=(const Bytes&) = default;
  Bytes(Bytes&&) noexcept       = default;
  Bytes& operator=(Bytes&&) noexcept = default;

  /* ── Factories ─────────────────────────────────────────────────── */

  /** @brief Take ownership of a Vec<uint8_t>. Zero-copy. */
  static Bytes from(Vec<uint8_t> vec) {
    size_t n = vec.len();
    if (n == 0) return Bytes();
    Shared<Impl> impl = Shared<Impl>::make(std::move(vec));
    Bytes b;
    b.m_impl   = some(std::move(impl));
    b.m_offset = 0;
    b.m_len    = n;
    return b;
  }

  /** @brief Copy bytes from a String (UTF-8 byte view). */
  static Bytes from(String s) {
    Vec<uint8_t> bytes = std::move(s).into_bytes();
    return from(std::move(bytes));
  }

  /** @brief Copy a C string (without the trailing NUL). */
  static Bytes from(const char* s) {
    XPP_ASSERT(s != nullptr, "Bytes::from(nullptr)");
    size_t len = std::strlen(s);
    return from(s, len);
  }

  /** @brief Copy @p len bytes from @p data. */
  static Bytes from(const uint8_t* data, size_t len) {
    Vec<uint8_t> v;
    v.reserve(len);
    for (size_t i = 0; i < len; ++i) v.push(data[i]);
    return from(std::move(v));
  }

  /** @brief Copy @p len bytes from @p data (char* overload). */
  static Bytes from(const char* data, size_t len) {
    return from(reinterpret_cast<const uint8_t*>(data), len);
  }

  /** @brief Explicit-copy factory. Same as `from(data, len)`, but the
   *         name makes the copy visible at the call site. */
  static Bytes copy(const char* data, size_t len) {
    return from(data, len);
  }

  /* ── Observers ─────────────────────────────────────────────────── */

  /** @brief Pointer to the first byte (nullptr if empty). */
  const uint8_t* data() const noexcept {
    const Impl* impl = m_impl.as_deref();
    return impl ? impl->buf.data() + m_offset : nullptr;
  }

  /** @brief Number of bytes. */
  size_t size() const noexcept { return m_len; }

  /** @brief True if size() == 0. */
  bool empty() const noexcept { return m_len == 0; }

  /** @brief STL-compatible alias for empty(). */
  bool is_empty() const noexcept { return m_len == 0; }

  /** @brief Read-only view over the bytes. */
  Span<const uint8_t> as_span() const noexcept {
    return Span<const uint8_t>(data(), m_len);
  }

  /** @brief Byte at @p idx. Debug-checked bounds. */
  uint8_t operator[](size_t idx) const {
    XPP_DEBUG_ASSERT(idx < m_len, "Bytes::operator[]: index %zu out of range (size %zu)", idx,
                     m_len);
    return m_impl.as_deref()->buf.data()[m_offset + idx];
  }

  /* ── Slicing (zero-copy) ───────────────────────────────────────── */

  /**
   * @brief Sub-range starting at @p offset, length @p len.
   *
   * O(1): shares the underlying buffer, adjusts offset/length.
   * Debug-asserts that @p offset + @p len <= size().
   */
  Bytes slice(size_t offset, size_t len) const {
    XPP_DEBUG_ASSERT(offset <= m_len, "Bytes::slice: offset %zu exceeds size %zu", offset, m_len);
    XPP_DEBUG_ASSERT(len <= m_len - offset, "Bytes::slice: offset %zu + len %zu exceeds size %zu",
                     offset, len, m_len);
    Bytes b;
    b.m_impl   = m_impl;             // Option copy → Shared clone → refcount++
    b.m_offset = m_offset + offset;
    b.m_len    = len;
    return b;
  }

  /**
   * @brief Sub-range starting at @p offset, to the end.
   *
   * O(1). Debug-asserts that @p offset <= size().
   */
  Bytes slice_from(size_t offset) const {
    XPP_DEBUG_ASSERT(offset <= m_len, "Bytes::slice_from: offset %zu exceeds size %zu", offset,
                     m_len);
    return slice(offset, m_len - offset);
  }

  /* ── Conversion ────────────────────────────────────────────────── */

  /**
   * @brief Copy into a new Vec<uint8_t>.
   *
   * O(n): the returned Vec owns its own buffer, independent of this Bytes.
   */
  Vec<uint8_t> to_vec() const {
    Vec<uint8_t> v;
    v.reserve(m_len);
    const Impl* impl = m_impl.as_deref();
    const uint8_t* src = impl ? impl->buf.data() + m_offset : nullptr;
    for (size_t i = 0; i < m_len; ++i) v.push(src[i]);
    return v;
  }

  /**
   * @brief Decode as UTF-8. Returns Err if the bytes are not valid UTF-8.
   *
   * Delegates to @ref String::from_utf8.
   */
  Result<String, Utf8Error> to_string() const {
    // Copy into a Vec because String::from_utf8 takes ownership of a Vec.
    Vec<uint8_t> v = to_vec();
    return String::from_utf8(std::move(v));
  }

  /**
   * @brief Decode as UTF-8, replacing invalid byte sequences with U+FFFD.
   *
   * Always succeeds. Mirrors Rust's String::from_utf8_lossy.
   */
  String to_string_lossy() const {
    // Fast path: valid UTF-8 → no allocation beyond the String's own buffer.
    Vec<uint8_t> v = to_vec();
    if (_::validate_utf8(v.data(), v.len()) == SIZE_MAX) {
      // Valid — from_utf8_unchecked avoids a second validation pass.
      return String::from_utf8_unchecked(std::move(v));
    }
    // Slow path: walk the buffer, replace invalid sequences with U+FFFD.
    Vec<uint8_t> out;
    out.reserve(v.len());
    const uint8_t* p   = v.data();
    size_t         len = v.len();
    for (size_t i = 0; i < len;) {
      size_t consumed = _::safe_utf8_step(p + i, len - i);
      if (consumed == 0) {
        // Invalid leading byte or truncated sequence — emit U+FFFD.
        out.push(0xEF);
        out.push(0xBF);
        out.push(0xBD);
        i += 1;
      } else {
        for (size_t k = 0; k < consumed; ++k) out.push(p[i + k]);
        i += consumed;
      }
    }
    return String::from_utf8_unchecked(std::move(out));
  }

  /* ── Iterators (raw pointers) ──────────────────────────────────── */

  const uint8_t* begin() const noexcept { return data(); }
  const uint8_t* end()   const noexcept { return data() + m_len; }

  /* ── Comparison ────────────────────────────────────────────────── */

  /** @brief Byte-wise equality. */
  friend bool operator==(const Bytes& a, const Bytes& b) {
    if (a.m_len != b.m_len) return false;
    if (a.m_len == 0) return true;
    return std::memcmp(a.data(), b.data(), a.m_len) == 0;
  }

  friend bool operator!=(const Bytes& a, const Bytes& b) { return !(a == b); }

private:
  Option<Shared<Impl>> m_impl;
  size_t               m_offset;
  size_t               m_len;
};

/* ── Static size guarantee ────────────────────────────────────────── */

static_assert(sizeof(Bytes) == sizeof(Shared<Bytes::Impl>) + 2 * sizeof(size_t),
              "Bytes must be Shared<Impl> + offset + len");

}  // namespace xpp

#endif  // XPP_BYTES_H
