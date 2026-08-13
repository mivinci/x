/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json.h - JSON backend for xpp::serde, wrapping libx/x/json/.
 *
 * Provides `json::Serializer` and `json::Deserializer` that satisfy the
 * Serializer / Deserializer concepts declared in serde.h. The Serializer
 * builds an xJson tree with `xJsonNew*` (malloc-backed) and dumps it via
 * `xJsonStringify`. The Deserializer parses with `xJsonParseCopy`
 * (arena-backed, safe) and walks the DOM.
 *
 * Link target: any TU including this header must link `xjson`.
 *
 * C++11-compatible. Header-only. No exceptions. No RTTI.
 */

#ifndef XPP_SERDE_JSON_H
#define XPP_SERDE_JSON_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

#include <x/json/json.h>

namespace xpp {
namespace serde {
namespace json {

/* ─────────────────────────── Serializer ──────────────────────────── */

/**
 * @brief JSON serializer. Build a tree of `xJson` nodes, then dump with
 *        `to_string()`.
 *
 * Single-use: build one tree, stringify, discard. Reusing a Serializer
 * after `to_string()` is fine but the previous tree is freed when a new
 * root is emitted.
 *
 * The scope handles (`StructScope`, `SeqScope`) borrow the Serializer
 * and must call `end()` before the Serializer is destroyed.
 */
class Serializer {
public:
  Serializer() = default;
  ~Serializer() {
    if (m_root) xJsonFree(m_root);
  }
  Serializer(const Serializer &)            = delete;
  Serializer &operator=(const Serializer &) = delete;

  /* Nested scope types — defined below; forward-declared for the
     composite-emit methods that return them by value. */
  class StructScope;
  class SeqScope;
  class VariantScope;

  /* ── Primitive emits ── */
  Result<Void, Error> serialize_bool(bool v) {
    emit(xJsonNewBool(v ? 1 : 0));
    return ok(Void{});
  }
  Result<Void, Error> serialize_i32(int32_t v) {
    emit(xJsonNewInt(static_cast<int64_t>(v)));
    return ok(Void{});
  }
  Result<Void, Error> serialize_i64(int64_t v) {
    emit(xJsonNewInt(v));
    return ok(Void{});
  }
  Result<Void, Error> serialize_u32(uint32_t v) {
    emit(xJsonNewInt(static_cast<int64_t>(v)));
    return ok(Void{});
  }
  Result<Void, Error> serialize_u64(uint64_t v) {
    emit(xJsonNewInt(static_cast<int64_t>(v)));
    return ok(Void{});
  }
  Result<Void, Error> serialize_f32(float v) {
    return serialize_f64(static_cast<double>(v));
  }
  Result<Void, Error> serialize_f64(double v) {
    // JSON has no representation for NaN / Infinity. Refuse rather
    // than emit non-conforming JSON.
    if (std::isnan(v) || std::isinf(v)) {
      return err(error(ErrorKind::InvalidValue, v > 0
                                                  ? "infinity not representable in JSON"
                                                  : (v < 0 ? "-infinity not representable in JSON"
                                                           : "NaN not representable in JSON")));
    }
    emit(xJsonNewDouble(v));
    return ok(Void{});
  }
  Result<Void, Error> serialize_str(const String &v) {
    emit(xJsonNewStringN(reinterpret_cast<const char *>(v.as_bytes().data()), v.len()));
    return ok(Void{});
  }
  Result<Void, Error> serialize_none() {
    emit(xJsonNewNull());
    return ok(Void{});
  }

  /// Option<T>:Some forward. JSON is self-delimiting (the value itself
  /// records its type), so no out-of-band tag is required — just emit
  /// the value as-is. Binary backends override this to write a tag.
  template <class T> Result<Void, Error> serialize_some(const T &v) {
    return serde::serialize(v, *this);
  }

  /* ── Composite emits ── */
  Result<StructScope, Error> serialize_struct(const char *name, size_t n) {
    (void)name;
    (void)n;
    xJson *obj = xJsonNewObject();
    emit(obj);
    m_stack.push(Frame{Frame::kStruct, obj, nullptr});
    return ok(StructScope(*this));
  }

  Result<SeqScope, Error> serialize_seq(Option<size_t> len) {
    (void)len; // JSON doesn't need a length hint
    xJson *arr = xJsonNewArray();
    emit(arr);
    m_stack.push(Frame{Frame::kSeq, arr, nullptr});
    return ok(SeqScope(*this));
  }

  /* ── Variant emit (external tagging) ──
   *
   * Produces `{"tag_string": <payload>}`. The VariantScope returned
   * is used to emit exactly one payload; the key is already set on
   * the frame so payload() takes no key argument. */
  Result<VariantScope, Error> serialize_variant(const char *name, size_t tag_index,
                                                const char *tag_string) {
    (void)name;
    (void)tag_index;
    xJson *obj = xJsonNewObject();
    emit(obj);
    m_stack.push(Frame{Frame::kStruct, obj, tag_string});
    return ok(VariantScope(*this));
  }

  /* ── Readout ── */
  /**
   * @brief Return the serialized tree as a compact JSON string.
   *
   * May be called at most once per tree; calling `to_string()` does not
   * invalidate the Serializer, but emitting more data after `to_string()`
   * without resetting will produce a tree whose root replaces the old
   * one (and the old root is freed).
   */
  String to_string() const {
    if (!m_root) {
      // "null" is always valid UTF-8.
      return String::from_utf8("null").unwrap();
    }
    char *s = xJsonStringify(m_root);
    if (!s) return String();
    String out = String::from_utf8(s).unwrap();
    std::free(s);
    return out;
  }

  /**
   * @brief Release the current tree and clear the scope stack.
   *
   * Safe to call at any time: after `to_string()`, mid-build, or after
   * an error. Brings the Serializer back to its freshly-constructed
   * state so a new tree can be emitted from scratch.
   */
  void reset() {
    if (m_root) {
      xJsonFree(m_root);
      m_root = nullptr;
    }
    m_stack.clear();
  }

  /* ── Scope types (borrow the Serializer) ── */
  class StructScope {
  public:
    explicit StructScope(Serializer &ser) : m_ser(ser) {}
    template <class T> Result<Void, Error> field(const char *key, const T &v) {
      m_ser.set_next_key(key);
      XPP_SERDE_TRY(serde::serialize(v, m_ser));
      return ok(Void{});
    }
    Result<Void, Error> end() {
      m_ser.pop_frame();
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
      return ok(Void{});
    }
    Result<Void, Error> end() {
      m_ser.pop_frame();
      return ok(Void{});
    }

  private:
    Serializer &m_ser;
  };

  /* VariantScope: the key is already set on the frame by
   * serialize_variant, so payload() only needs the value. */
  class VariantScope {
  public:
    explicit VariantScope(Serializer &ser) : m_ser(ser) {}
    template <class T> Result<Void, Error> payload(const T &v) {
      XPP_SERDE_TRY(serde::serialize(v, m_ser));
      return ok(Void{});
    }
    Result<Void, Error> end() {
      m_ser.pop_frame();
      return ok(Void{});
    }

  private:
    Serializer &m_ser;
  };

private:
  friend class StructScope;
  friend class SeqScope;
  friend class VariantScope;

  struct Frame {
    enum Kind {
      kStruct,
      kSeq
    } kind;
    xJson      *parent;
    const char *key; // valid only for kStruct, set per-field
  };

  /**
   * @brief Attach a freshly-built node to the current target (root if
   *        no frame is on the stack, otherwise the top frame's parent
   *        object/array).
   */
  void emit(xJson *node) {
    if (m_stack.empty()) {
      if (m_root) xJsonFree(m_root);
      m_root = node;
      return;
    }
    Frame &f = m_stack[m_stack.len() - 1];
    if (f.kind == Frame::kStruct) {
      xJsonObjectSet(f.parent, f.key, node);
    } else {
      xJsonArrayAppend(f.parent, node);
    }
  }

  void set_next_key(const char *key) {
    Frame &f = m_stack[m_stack.len() - 1];
    f.key    = key;
  }

  void pop_frame() {
    m_stack.pop();
  }

  xpp::Vec<Frame> m_stack;
  xJson          *m_root = nullptr;
};

/* ────────────────────────── Deserializer ─────────────────────────── */

/**
 * @brief JSON deserializer. Holds a parsed JSON tree (owned when
 *        constructed via `from_string`) and exposes the Deserializer
 *        concept against the current node.
 *
 * Top-level: construct via `from_string`. Sub-deserializers (for
 * map values, seq elements) are constructed internally and borrow
 * their node — they do not own memory.
 */
class Deserializer {
public:
  /** @brief Sub-deserializer: borrows `node`, no ownership. */
  explicit Deserializer(const xJson *node) : m_node(node), m_owned_root(nullptr) {}

  ~Deserializer() {
    if (m_owned_root) xJsonFree(m_owned_root);
  }
  Deserializer(const Deserializer &)            = delete;
  Deserializer &operator=(const Deserializer &) = delete;
  Deserializer(Deserializer &&o) noexcept : m_node(o.m_node), m_owned_root(o.m_owned_root) {
    o.m_node       = nullptr;
    o.m_owned_root = nullptr;
  }

  /** @brief Parse `s` and return an owning Deserializer at the root. */
  static Result<Deserializer, Error> from_string(const String &s) {
    return from_bytes(reinterpret_cast<const char *>(s.as_bytes().data()), s.len());
  }

  /** @brief Parse a NUL-terminated C string and return an owning Deserializer. */
  static Result<Deserializer, Error> from_string(const char *s) {
    return from_bytes(s, std::strlen(s));
  }

  /** @brief Parse `len` bytes starting at `s`. */
  static Result<Deserializer, Error> from_bytes(const char *s, size_t len) {
    xJson *root = xJsonParseCopy(s, len);
    if (!root) {
      return err(error(ErrorKind::Unexpected, "failed to parse JSON"));
    }
    Deserializer d(root);
    d.m_owned_root = root;
    return ok(std::move(d));
  }

  /* ── Nested access types ── */
  class SeqAccess {
  public:
    explicit SeqAccess(const xJson *arr) : m_arr(arr), m_size(xJsonArraySize(arr)), m_index(0) {}

    template <class T> Result<Option<T>, Error> next_element() {
      if (m_index >= m_size) return ok(Option<T>(none));
      xJson *elem = xJsonArrayGet(m_arr, m_index);
      ++m_index;
      if (!elem) {
        // m_size said the element exists, but the lookup failed — this
        // can only happen if the underlying xJson tree is corrupt.
        return err(
          error(ErrorKind::Unexpected, "array index in bounds but xJsonArrayGet returned NULL"));
      }
      Deserializer sub(elem);
      XPP_SERDE_TRY_VAR(v, Deserialize<T>::run(sub));
      return ok(Option<T>(std::move(v)));
    }

  private:
    const xJson *m_arr;
    int          m_size;
    int          m_index;
  };

  class MapAccess {
  public:
    explicit MapAccess(const xJson *obj) : m_iter(xJsonNewIterator(obj)) {}
    ~MapAccess() {
      if (m_iter) xJsonFree(m_iter);
    }
    MapAccess(const MapAccess &)            = delete;
    MapAccess &operator=(const MapAccess &) = delete;

    Result<Option<String>, Error> next_key() {
      if (m_done) return ok(Option<String>(none));
      if (!m_has_pending) {
        if (!xJsonIteratorNext(m_iter)) {
          m_done = true;
          return ok(Option<String>(none));
        }
        m_has_pending = true;
      }
      size_t      len = 0;
      const char *key = xJsonIteratorKey(m_iter, &len);
      auto        r   = String::from_utf8(key, len);
      if (!r.is_ok()) {
        return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in object key"));
      }
      return ok(Option<String>(std::move(r).unwrap()));
    }

    template <class V> Result<V, Error> next_value() {
      if (!m_has_pending) {
        return err(error(ErrorKind::Unexpected, "next_value without next_key"));
      }
      m_has_pending    = false;
      xJson       *val = xJsonIteratorValue(m_iter);
      Deserializer sub(val);
      return Deserialize<V>::run(sub);
    }

    Result<Void, Error> next_value_ignored() {
      if (!m_has_pending) {
        return err(error(ErrorKind::Unexpected, "next_value_ignored without next_key"));
      }
      m_has_pending = false;
      return ok(Void{});
    }

  private:
    xJsonIterator *m_iter;
    bool           m_has_pending = false;
    bool           m_done        = false;
  };

  /* ── Primitive reads ── */
  Result<bool, Error> deserialize_bool() {
    if (xJsonType(m_node) != XJSON_BOOL) {
      return err(error(ErrorKind::InvalidValue, "expected bool"));
    }
    return ok(xJsonBool(m_node) != 0);
  }

  Result<int32_t, Error> deserialize_i32() {
    int     t = xJsonType(m_node);
    int64_t v;
    if (t == XJSON_INT) {
      v = xJsonInt(m_node);
    } else if (t == XJSON_DOUBLE) {
      double d = xJsonDouble(m_node);
      if (d != static_cast<double>(static_cast<int64_t>(d))) {
        return err(error(ErrorKind::InvalidValue, "expected integer, got fractional number"));
      }
      v = static_cast<int64_t>(d);
    } else {
      return err(error(ErrorKind::InvalidValue, "expected int"));
    }
    if (v < INT32_MIN || v > INT32_MAX) {
      return err(error(ErrorKind::InvalidValue, "int32 out of range"));
    }
    return ok(static_cast<int32_t>(v));
  }

  Result<int64_t, Error> deserialize_i64() {
    int t = xJsonType(m_node);
    if (t == XJSON_INT) return ok(xJsonInt(m_node));
    if (t == XJSON_DOUBLE) {
      double d = xJsonDouble(m_node);
      if (d != static_cast<double>(static_cast<int64_t>(d))) {
        return err(error(ErrorKind::InvalidValue, "expected integer, got fractional number"));
      }
      return ok(static_cast<int64_t>(d));
    }
    return err(error(ErrorKind::InvalidValue, "expected int"));
  }

  Result<uint32_t, Error> deserialize_u32() {
    XPP_SERDE_TRY_VAR(v, deserialize_i64());
    if (v < 0 || v > UINT32_MAX) {
      return err(error(ErrorKind::InvalidValue, "u32 out of range"));
    }
    return ok(static_cast<uint32_t>(v));
  }

  Result<uint64_t, Error> deserialize_u64() {
    XPP_SERDE_TRY_VAR(v, deserialize_i64());
    if (v < 0) {
      return err(error(ErrorKind::InvalidValue, "u64 negative"));
    }
    return ok(static_cast<uint64_t>(v));
  }

  Result<float, Error> deserialize_f32() {
    XPP_SERDE_TRY_VAR(v, deserialize_f64());
    return ok(static_cast<float>(v));
  }

  Result<double, Error> deserialize_f64() {
    int t = xJsonType(m_node);
    if (t == XJSON_DOUBLE) return ok(xJsonDouble(m_node));
    if (t == XJSON_INT) return ok(static_cast<double>(xJsonInt(m_node)));
    return err(error(ErrorKind::InvalidValue, "expected number"));
  }

  Result<String, Error> deserialize_str() {
    if (xJsonType(m_node) != XJSON_STRING) {
      return err(error(ErrorKind::InvalidValue, "expected string"));
    }
    size_t      len = xJsonStringLength(m_node);
    const char *s   = xJsonString(m_node);
    auto        r   = String::from_utf8(s, len);
    if (!r.is_ok()) {
      return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in JSON string"));
    }
    return ok(std::move(r).unwrap());
  }

  /* ── Composite reads (visitor-driven) ── */
  template <class V>
  auto deserialize_option(V &&visitor) -> decltype(std::declval<V>().visit_none()) {
    if (xJsonType(m_node) == XJSON_NULL) {
      return visitor.visit_none();
    }
    return visitor.visit_some(*this);
  }

  template <class V>
  auto deserialize_seq(V &&visitor)
    -> decltype(std::declval<V>().visit_seq(std::declval<SeqAccess &>())) {
    if (xJsonType(m_node) != XJSON_ARRAY) {
      return err(error(ErrorKind::InvalidValue, "expected JSON array"));
    }
    SeqAccess seq(m_node);
    return visitor.visit_seq(seq);
  }

  template <class V>
  auto deserialize_struct(const char *name, const char *const *fields, size_t n, V &&visitor)
    -> decltype(std::declval<V>().visit_map(std::declval<MapAccess &>())) {
    (void)name;
    (void)fields;
    (void)n; // JSON does not use these hints.
    if (xJsonType(m_node) != XJSON_OBJECT) {
      return err(error(ErrorKind::InvalidValue, "expected JSON object"));
    }
    MapAccess map(m_node);
    return visitor.visit_map(map);
  }

  /* ── Variant read (external tagging) ──
   *
   * Expects `{"tag_string": <payload>}`. Reads the first key, matches
   * it against `tags[]` to get tag_index, then hands the payload
   * node to the visitor via visit_variant(tag_index, sub_deser). */
  template <class V>
  auto deserialize_variant(const char *name, const char *const *tags, size_t n, V &&visitor)
    -> decltype(std::declval<V>().visit_variant(0, std::declval<Deserializer &>())) {
    (void)name;
    if (xJsonType(m_node) != XJSON_OBJECT) {
      return err(error(ErrorKind::InvalidValue, "expected JSON object for variant"));
    }
    xJsonIterator *iter = xJsonNewIterator(m_node);
    if (!iter || !xJsonIteratorNext(iter)) {
      if (iter) xJsonFree(iter);
      return err(error(ErrorKind::InvalidValue, "empty object for variant"));
    }
    size_t      key_len = 0;
    const char *key     = xJsonIteratorKey(iter, &key_len);
    auto        key_r   = String::from_utf8(key, key_len);
    if (!key_r.is_ok()) {
      xJsonFree(iter);
      return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in variant tag"));
    }
    String tag = std::move(key_r).unwrap();
    xJson *val = xJsonIteratorValue(iter);
    xJsonFree(iter);

    size_t tag_index = n;
    for (size_t i = 0; i < n; ++i) {
      if (tag == tags[i]) {
        tag_index = i;
        break;
      }
    }
    if (tag_index == n) {
      return err(error(ErrorKind::UnknownField, "unknown variant tag"));
    }
    Deserializer sub(val);
    return visitor.visit_variant(tag_index, sub);
  }

private:
  const xJson *m_node;
  xJson       *m_owned_root; // nullptr for sub-deserializers
};

/**
 * @brief Serialize `value` to a JSON string in one step.
 *
 * Convenience wrapper around `Serializer` + `serde::serialize` +
 * `Serializer::to_string()`, mirroring `serde_json::to_string` in Rust.
 *
 *   auto s = xpp::serde::json::to_string(person);
 *   if (s.is_ok()) { use s.unwrap(); }
 */
template <class T> Result<String, Error> to_string(const T &value) {
  Serializer ser;
  XPP_SERDE_TRY(serde::serialize(value, ser));
  return ok(ser.to_string());
}

} // namespace json
} // namespace serde
} // namespace xpp

#endif // XPP_SERDE_JSON_H
