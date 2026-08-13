/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bin.h - Compact length-prefixed binary backend for xpp::serde.
 *
 * Wire format (all multi-byte integers are little-endian):
 *
 *   bool      1 byte                 (0x00 / 0x01)
 *   i32       4 bytes
 *   i64       8 bytes
 *   u32       4 bytes
 *   u64       8 bytes
 *   f32       4 bytes  (IEEE 754)
 *   f64       8 bytes  (IEEE 754)
 *   str       u32 length + UTF-8 bytes (no NUL terminator)
 *   none      1 byte   (0x00)        Option discriminator
 *   some      1 byte   (0x01) + value  Option discriminator
 *   seq       u32 count + count * element
 *   struct    field values back-to-back, no field names, no length prefix
 *
 * The same Serialize<T> / Deserialize<T> specialization produced for
 * the JSON backend works unchanged here — the only backend-side
 * addition is `serialize_some` writing the 0x01 tag (see serde.h for
 * the concept change).
 *
 * Self-delimiting for fixed-width primitives and seq; structs rely on
 * the visitor knowing the field count (passed via deserialize_struct
 * `n`), matching the JSON backend's contract.
 *
 * C++11-compatible. Header-only. No exceptions. No RTTI.
 */

#ifndef XPP_SERDE_BIN_H
#define XPP_SERDE_BIN_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

namespace xpp {
namespace serde {
namespace bin {

/* ─────────────────────────── Serializer ──────────────────────────── */

/**
 * @brief Binary serializer. Appends bytes to an internal Vec<uint8_t>.
 *
 * Single-use: build one buffer, then take it via `into_buffer()` or
 * copy via `buffer()`. Reusing a Serializer after taking the buffer is
 * fine; the next emit starts a fresh tree.
 */
class Serializer {
public:
  Serializer()                              = default;
  Serializer(const Serializer &)            = delete;
  Serializer &operator=(const Serializer &) = delete;

  class StructScope;
  class SeqScope;
  class VariantScope;

  /* ── Primitive emits ── */

  Result<Void, Error> serialize_bool(bool v) {
    write_u8(v ? 0x01 : 0x00);
    return ok(Void{});
  }
  Result<Void, Error> serialize_i32(int32_t v) {
    write_u32(static_cast<uint32_t>(v));
    return ok(Void{});
  }
  Result<Void, Error> serialize_i64(int64_t v) {
    write_u64(static_cast<uint64_t>(v));
    return ok(Void{});
  }
  Result<Void, Error> serialize_u32(uint32_t v) {
    write_u32(v);
    return ok(Void{});
  }
  Result<Void, Error> serialize_u64(uint64_t v) {
    write_u64(v);
    return ok(Void{});
  }
  Result<Void, Error> serialize_f32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32(bits);
    return ok(Void{});
  }
  Result<Void, Error> serialize_f64(double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    write_u64(bits);
    return ok(Void{});
  }
  Result<Void, Error> serialize_str(const String &v) {
    auto bytes = v.as_bytes();
    write_u32(static_cast<uint32_t>(bytes.size()));
    for (size_t i = 0; i < bytes.size(); ++i)
      m_buf.push(bytes.data()[i]);
    return ok(Void{});
  }

  /// Option<T>::None — write 0x00 tag.
  Result<Void, Error> serialize_none() {
    write_u8(0x00);
    return ok(Void{});
  }

  /// Option<T>::Some(v) — write 0x01 tag, then the value. Backends
  /// whose encoding is self-delimiting (JSON: null vs value) implement
  /// this as a plain forward to `serde::serialize(v, *this)`.
  template <class T> Result<Void, Error> serialize_some(const T &v) {
    write_u8(0x01);
    return serde::serialize(v, *this);
  }

  /* ── Composite emits ── */

  Result<SeqScope, Error> serialize_seq(Option<size_t> len) {
    size_t placeholder = m_buf.len();
    // Always write a u32 length prefix. If the caller gave us the
    // count, write it directly; otherwise reserve 4 bytes for back-
    // patching at SeqScope::end().
    if (len.is_some()) {
      write_u32(static_cast<uint32_t>(len.unwrap()));
    } else {
      write_u32(0);
    }
    m_seq_stack.push(SeqFrame{placeholder, 0, !len.is_some()});
    return ok(SeqScope(*this));
  }

  Result<StructScope, Error> serialize_struct(const char *name, size_t n) {
    (void)name; // binary: field names not encoded
    (void)n;    // deserializer learns field count from the visitor
    return ok(StructScope(*this));
  }

  /* ── Variant emit (external tagging) ──
   *
   * Binary writes a u32 tag_index followed by the payload bytes.
   * The tag_string is unused (kept for API symmetry with JSON). */
  Result<VariantScope, Error> serialize_variant(const char *name, size_t tag_index,
                                                const char *tag_string) {
    (void)name;
    (void)tag_string;
    write_u32(static_cast<uint32_t>(tag_index));
    return ok(VariantScope(*this));
  }

  /* ── Readout ── */

  /// @brief Borrow the encoded bytes as a Span. Valid until next emit
  ///        or until the Serializer is destroyed.
  Span<const uint8_t> buffer() const {
    return Span<const uint8_t>(m_buf.data(), m_buf.len());
  }

  /// @brief Move the encoded bytes out. The Serializer is left empty.
  Vec<uint8_t> into_buffer() {
    return std::move(m_buf);
  }

  /// @brief Reset to fresh state.
  void reset() {
    m_buf.clear();
    m_seq_stack.clear();
  }

  /* ── Scope types ── */
  class StructScope {
  public:
    explicit StructScope(Serializer &ser) : m_ser(ser) {}
    template <class T> Result<Void, Error> field(const char *key, const T &v) {
      (void)key; // binary: field name not encoded
      return serde::serialize(v, m_ser);
    }
    Result<Void, Error> end() {
      return ok(Void{});
    }

  private:
    Serializer &m_ser;
  };

  class SeqScope {
  public:
    explicit SeqScope(Serializer &ser) : m_ser(ser) {}
    template <class T> Result<Void, Error> element(const T &v) {
      XPP_SERDE_TRY(serde::serialize(v, m_ser));
      m_ser.m_seq_stack.last().unwrap().count++;
      return ok(Void{});
    }
    Result<Void, Error> end() {
      auto opt = m_ser.m_seq_stack.last();
      if (opt.is_none()) {
        return err(error(ErrorKind::Unexpected, "SeqScope::end with no seq on stack"));
      }
      SeqFrame &f = opt.unwrap();
      if (f.unknown_length) {
        // Back-patch the length placeholder we reserved.
        uint32_t n = static_cast<uint32_t>(f.count);
        uint8_t *p = m_ser.m_buf.data() + f.placeholder_offset;
        p[0]       = static_cast<uint8_t>(n & 0xFF);
        p[1]       = static_cast<uint8_t>((n >> 8) & 0xFF);
        p[2]       = static_cast<uint8_t>((n >> 16) & 0xFF);
        p[3]       = static_cast<uint8_t>((n >> 24) & 0xFF);
      }
      m_ser.m_seq_stack.pop();
      return ok(Void{});
    }

  private:
    Serializer &m_ser;
  };

  /* VariantScope: writes the payload bytes after serialize_variant
   * has already written the tag_index. No key to skip. */
  class VariantScope {
  public:
    explicit VariantScope(Serializer &ser) : m_ser(ser) {}
    template <class T> Result<Void, Error> payload(const T &v) {
      return serde::serialize(v, m_ser);
    }
    Result<Void, Error> end() {
      return ok(Void{});
    }

  private:
    Serializer &m_ser;
  };

private:
  friend class StructScope;
  friend class SeqScope;
  friend class VariantScope;

  struct SeqFrame {
    size_t placeholder_offset; // where the u32 length was written
    size_t count;              // elements emitted so far
    bool   unknown_length;     // true if we need to back-patch
  };

  void write_u8(uint8_t b) {
    m_buf.push(b);
  }
  void write_u32(uint32_t v) {
    m_buf.push(static_cast<uint8_t>(v & 0xFF));
    m_buf.push(static_cast<uint8_t>((v >> 8) & 0xFF));
    m_buf.push(static_cast<uint8_t>((v >> 16) & 0xFF));
    m_buf.push(static_cast<uint8_t>((v >> 24) & 0xFF));
  }
  void write_u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      m_buf.push(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
  }

  Vec<uint8_t>  m_buf;
  Vec<SeqFrame> m_seq_stack;
};

/* ────────────────────────── Deserializer ─────────────────────────── */

/**
 * @brief Binary deserializer. Borrows a byte buffer (non-owning when
 *        constructed directly; owning when constructed via
 *        `from_bytes` which copies into an internal Vec).
 */
class Deserializer {
public:
  /** @brief Borrowed view over `data` of length `len`. Caller must
   *         keep the buffer alive for the deserializer's lifetime. */
  Deserializer(const uint8_t *data, size_t len) : m_data(data), m_len(len), m_pos(0) {}

  /** @brief Borrowed view over a Vec<uint8_t>. Caller retains
   *         ownership. */
  explicit Deserializer(const Vec<uint8_t> &buf) : m_data(buf.data()), m_len(buf.len()), m_pos(0) {}

  ~Deserializer()                               = default;
  Deserializer(const Deserializer &)            = delete;
  Deserializer &operator=(const Deserializer &) = delete;
  Deserializer(Deserializer &&o) noexcept
      : m_data(o.m_data), m_len(o.m_len), m_pos(o.m_pos), m_owned(std::move(o.m_owned)) {
    o.m_data = nullptr;
    o.m_len  = 0;
    o.m_pos  = 0;
  }

  /** @brief Copy `bytes` into an owning Vec and deserialize from it. */
  static Result<Deserializer, Error> from_bytes(const uint8_t *data, size_t len) {
    if (!data && len > 0) {
      return err(error(ErrorKind::Unexpected, "null buffer with non-zero length"));
    }
    Vec<uint8_t> owned;
    for (size_t i = 0; i < len; ++i)
      owned.push(data[i]);
    Deserializer d(owned.data(), owned.len());
    d.m_owned = std::move(owned);
    return ok(std::move(d));
  }

  static Result<Deserializer, Error> from_bytes(const Vec<uint8_t> &bytes) {
    return from_bytes(bytes.data(), bytes.len());
  }

  /** @brief Convenience: borrow a Span<const uint8_t>. */
  static Deserializer borrow(Span<const uint8_t> bytes) {
    return Deserializer(bytes.data(), bytes.size());
  }

  /* ── Nested access types ── */

  class SeqAccess {
  public:
    SeqAccess(const uint8_t *data, size_t len, size_t &pos)
        : m_data(data), m_len(len), m_pos(pos), m_remaining(0) {}

    // Called by Deserializer after reading the length prefix.
    void set_remaining(size_t n) {
      m_remaining = n;
    }

    template <class T> Result<Option<T>, Error> next_element() {
      if (m_remaining == 0) return ok(Option<T>(none));
      --m_remaining;
      Deserializer sub(m_data, m_len, m_pos);
      XPP_SERDE_TRY_VAR(v, Deserialize<T>::run(sub));
      m_pos = sub.m_pos;
      return ok(Option<T>(std::move(v)));
    }

  private:
    const uint8_t *m_data;
    size_t         m_len;
    size_t        &m_pos;
    size_t         m_remaining;
  };

  class MapAccess {
  public:
    MapAccess(const char *const *fields, size_t n, const uint8_t *data, size_t len, size_t &pos)
        : m_fields(fields), m_n(n), m_field_index(0), m_data(data), m_len(len), m_pos(pos),
          m_has_pending(false) {}

    Result<Option<String>, Error> next_key() {
      if (m_has_pending) {
        // next_key called twice without next_value — programming error.
        return err(error(ErrorKind::Unexpected, "next_key called again without consuming value"));
      }
      if (m_field_index >= m_n) return ok(Option<String>(none));
      const char *k = m_fields[m_field_index];
      // XPP_SERDE's kFields array uses nullptr as the trailing sentinel
      // AND omits SKIP'd fields — so the actual entry count can be
      // smaller than the `n` passed to deserialize_struct. Treat a
      // nullptr entry as "no more fields".
      if (k == nullptr) return ok(Option<String>(none));
      m_has_pending = true;
      auto r        = String::from_utf8(k);
      if (!r.is_ok()) {
        return err(error(ErrorKind::InvalidValue, "field name is not valid UTF-8"));
      }
      return ok(Option<String>(std::move(r).unwrap()));
    }
    template <class V> Result<V, Error> next_value() {
      if (!m_has_pending) {
        return err(error(ErrorKind::Unexpected, "next_value without next_key"));
      }
      m_has_pending = false;
      Deserializer sub(m_data, m_len, m_pos);
      XPP_SERDE_TRY_VAR(v, Deserialize<V>::run(sub));
      m_pos = sub.m_pos;
      ++m_field_index;
      return ok(std::move(v));
    }

    Result<Void, Error> next_value_ignored() {
      if (!m_has_pending) {
        return err(error(ErrorKind::Unexpected, "next_value_ignored without next_key"));
      }
      m_has_pending = false;
      // No way to "skip" a value of unknown type in binary — the
      // visitor must actually deserialize it. We treat ignored as
      // a deserialize<Void> which... can't work. So we error: binary
      // does not support skipping unknown fields.
      //
      // However, the visit_map generated by XPP_SERDE only calls
      // next_value_ignored when the key doesn't match any known field,
      // which cannot happen here because we feed the visitor the
      // exact fields[] list the type declared. So this path is never
      // taken for XPP_SERDE types.
      return err(error(ErrorKind::UnknownField, "binary backend cannot skip unknown fields"));
    }

  private:
    const char *const *m_fields;
    size_t             m_n;
    size_t             m_field_index;
    const uint8_t     *m_data;
    size_t             m_len;
    size_t            &m_pos;
    bool               m_has_pending;
  };

  /* ── Primitive reads ── */

  Result<bool, Error> deserialize_bool() {
    uint8_t b;
    XPP_SERDE_TRY_VAR(ok_, read_u8(b));
    (void)ok_;
    if (b == 0x00) return ok(false);
    if (b == 0x01) return ok(true);
    return err(error(ErrorKind::InvalidValue, "invalid bool byte"));
  }

  Result<int32_t, Error> deserialize_i32() {
    uint32_t u;
    XPP_SERDE_TRY_VAR(ok_, read_u32(u));
    (void)ok_;
    return ok(static_cast<int32_t>(u));
  }
  Result<int64_t, Error> deserialize_i64() {
    uint64_t u;
    XPP_SERDE_TRY_VAR(ok_, read_u64(u));
    (void)ok_;
    return ok(static_cast<int64_t>(u));
  }
  Result<uint32_t, Error> deserialize_u32() {
    uint32_t u;
    XPP_SERDE_TRY_VAR(ok_, read_u32(u));
    (void)ok_;
    return ok(u);
  }
  Result<uint64_t, Error> deserialize_u64() {
    uint64_t u;
    XPP_SERDE_TRY_VAR(ok_, read_u64(u));
    (void)ok_;
    return ok(u);
  }
  Result<float, Error> deserialize_f32() {
    uint32_t bits;
    XPP_SERDE_TRY_VAR(ok_, read_u32(bits));
    (void)ok_;
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return ok(f);
  }
  Result<double, Error> deserialize_f64() {
    uint64_t bits;
    XPP_SERDE_TRY_VAR(ok_, read_u64(bits));
    (void)ok_;
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return ok(d);
  }
  Result<String, Error> deserialize_str() {
    uint32_t n;
    XPP_SERDE_TRY_VAR(ok_, read_u32(n));
    (void)ok_;
    if (!ensure(n)) {
      return err(error(ErrorKind::Eof, "string length exceeds buffer"));
    }
    auto r = String::from_utf8(reinterpret_cast<const char *>(m_data + m_pos), n);
    if (!r.is_ok()) {
      return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in string"));
    }
    m_pos += n;
    return ok(std::move(r).unwrap());
  }

  /* ── Composite reads ── */

  template <class V>
  auto deserialize_option(V &&visitor) -> decltype(std::declval<V>().visit_none()) {
    uint8_t tag;
    XPP_SERDE_TRY_VAR(ok_, read_u8(tag));
    (void)ok_;
    if (tag == 0x00) return visitor.visit_none();
    if (tag == 0x01) return visitor.visit_some(*this);
    return err(error(ErrorKind::InvalidValue, "invalid option tag"));
  }

  template <class V>
  auto deserialize_seq(V &&visitor)
    -> decltype(std::declval<V>().visit_seq(std::declval<SeqAccess &>())) {
    uint32_t n;
    XPP_SERDE_TRY_VAR(ok_, read_u32(n));
    (void)ok_;
    SeqAccess seq(m_data, m_len, m_pos);
    seq.set_remaining(n);
    return visitor.visit_seq(seq);
  }

  template <class V>
  auto deserialize_struct(const char *name, const char *const *fields, size_t n, V &&visitor)
    -> decltype(std::declval<V>().visit_map(std::declval<MapAccess &>())) {
    (void)name;
    MapAccess map(fields, n, m_data, m_len, m_pos);
    return visitor.visit_map(map);
  }

  /* ── Variant read (external tagging) ──
   *
   * Binary reads a u32 tag_index and dispatches to the visitor.
   * tags[] is unused (kept for API symmetry with JSON). */
  template <class V>
  auto deserialize_variant(const char *name, const char *const *tags, size_t n, V &&visitor)
    -> decltype(std::declval<V>().visit_variant(0, std::declval<Deserializer &>())) {
    (void)name;
    (void)tags;
    uint32_t tag_index;
    XPP_SERDE_TRY_VAR(ok_, read_u32(tag_index));
    (void)ok_;
    if (tag_index >= n) {
      return err(error(ErrorKind::UnknownField, "variant tag index out of range"));
    }
    return visitor.visit_variant(static_cast<size_t>(tag_index), *this);
  }

private:
  /// Sub-deserializer sharing the buffer at a different cursor.
  /// Used by SeqAccess::next_element and MapAccess::next_value.
  Deserializer(const uint8_t *data, size_t len, size_t pos)
      : m_data(data), m_len(len), m_pos(pos) {}

  Result<Void, Error> read_u8(uint8_t &out) {
    if (!ensure(1)) return err(error(ErrorKind::Eof, "unexpected end of buffer"));
    out = m_data[m_pos++];
    return ok(Void{});
  }
  Result<Void, Error> read_u32(uint32_t &out) {
    if (!ensure(4)) return err(error(ErrorKind::Eof, "unexpected end of buffer"));
    out = static_cast<uint32_t>(m_data[m_pos]) | (static_cast<uint32_t>(m_data[m_pos + 1]) << 8) |
          (static_cast<uint32_t>(m_data[m_pos + 2]) << 16) |
          (static_cast<uint32_t>(m_data[m_pos + 3]) << 24);
    m_pos += 4;
    return ok(Void{});
  }
  Result<Void, Error> read_u64(uint64_t &out) {
    if (!ensure(8)) return err(error(ErrorKind::Eof, "unexpected end of buffer"));
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(m_data[m_pos + i]) << (8 * i);
    }
    out = v;
    m_pos += 8;
    return ok(Void{});
  }

  bool ensure(size_t need) const {
    return m_pos + need <= m_len;
  }

  const uint8_t *m_data;
  size_t         m_len;
  size_t         m_pos;
  Vec<uint8_t>   m_owned; // non-empty only when constructed via from_bytes
};

} // namespace bin
} // namespace serde
} // namespace xpp

#endif // XPP_SERDE_BIN_H
