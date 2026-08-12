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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <x/json/json.h>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

namespace xpp {
namespace serde {
namespace json {

/* ─────────────────────────── Serializer ──────────────────────────── */

/**
 * @brief JSON serializer. Build a tree of `xJson` nodes, then dump with
 *        `buffer()`.
 *
 * Single-use: build one tree, stringify, discard. Reusing a Serializer
 * after `buffer()` is fine but the previous tree is freed when a new
 * root is emitted.
 *
 * The scope handles (`StructScope`, `SeqScope`) borrow the Serializer
 * and must call `end()` before the Serializer is destroyed.
 */
class Serializer {
 public:
  Serializer() = default;
  ~Serializer() {
    if (root_) xJsonFree(root_);
  }
  Serializer(const Serializer&) = delete;
  Serializer& operator=(const Serializer&) = delete;

  /* Nested scope types — defined below; forward-declared for the
     composite-emit methods that return them by value. */
  class StructScope;
  class SeqScope;

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
    emit(xJsonNewDouble(static_cast<double>(v)));
    return ok(Void{});
  }
  Result<Void, Error> serialize_f64(double v) {
    emit(xJsonNewDouble(v));
    return ok(Void{});
  }
  Result<Void, Error> serialize_str(const String& v) {
    emit(xJsonNewStringN(reinterpret_cast<const char*>(v.as_bytes().data()), v.len()));
    return ok(Void{});
  }
  Result<Void, Error> serialize_none() {
    emit(xJsonNewNull());
    return ok(Void{});
  }

  /* ── Composite emits ── */
  Result<StructScope, Error> serialize_struct(const char* name, size_t n) {
    (void)name;
    (void)n;
    xJson* obj = xJsonNewObject();
    emit(obj);
    stack_.push(Frame{Frame::kStruct, obj, nullptr});
    return ok(StructScope(*this));
  }

  Result<SeqScope, Error> serialize_seq(Option<size_t> len) {
    (void)len;  // JSON doesn't need a length hint
    xJson* arr = xJsonNewArray();
    emit(arr);
    stack_.push(Frame{Frame::kSeq, arr, nullptr});
    return ok(SeqScope(*this));
  }

  /* ── Readout ── */
  /**
   * @brief Return the serialized tree as a compact JSON string.
   *
   * May be called at most once per tree; calling `buffer()` does not
   * invalidate the Serializer, but emitting more data after `buffer()`
   * without resetting will produce a tree whose root replaces the old
   * one (and the old root is freed).
   */
  String buffer() const {
    if (!root_) {
      // "null" is always valid UTF-8.
      return String::from_utf8("null").unwrap();
    }
    char* s = xJsonStringify(root_);
    if (!s) return String();
    String out = String::from_utf8(s).unwrap();
    std::free(s);
    return out;
  }

  /* ── Scope types (borrow the Serializer) ── */
  class StructScope {
   public:
    explicit StructScope(Serializer& ser) : ser_(ser) {}
    template <class T>
    Result<Void, Error> field(const char* key, const T& v) {
      ser_.set_next_key(key);
      XPP_SERDE_TRY(serde::serialize(v, ser_));
      return ok(Void{});
    }
    Result<Void, Error> end() {
      ser_.pop_frame();
      return ok(Void{});
    }

   private:
    Serializer& ser_;
  };

  class SeqScope {
   public:
    explicit SeqScope(Serializer& ser) : ser_(ser) {}
    template <class T>
    Result<Void, Error> element(const T& v) {
      XPP_SERDE_TRY(serde::serialize(v, ser_));
      return ok(Void{});
    }
    Result<Void, Error> end() {
      ser_.pop_frame();
      return ok(Void{});
    }

   private:
    Serializer& ser_;
  };

 private:
  friend class StructScope;
  friend class SeqScope;

  struct Frame {
    enum Kind { kStruct, kSeq } kind;
    xJson* parent;
    const char* key;  // valid only for kStruct, set per-field
  };

  /**
   * @brief Attach a freshly-built node to the current target (root if
   *        no frame is on the stack, otherwise the top frame's parent
   *        object/array).
   */
  void emit(xJson* node) {
    if (stack_.empty()) {
      if (root_) xJsonFree(root_);
      root_ = node;
      return;
    }
    Frame& f = stack_[stack_.len() - 1];
    if (f.kind == Frame::kStruct) {
      xJsonObjectSet(f.parent, f.key, node);
    } else {
      xJsonArrayAppend(f.parent, node);
    }
  }

  void set_next_key(const char* key) {
    Frame& f = stack_[stack_.len() - 1];
    f.key = key;
  }

  void pop_frame() { stack_.pop(); }

  xpp::Vec<Frame> stack_;
  xJson* root_ = nullptr;
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
  explicit Deserializer(const xJson* node) : node_(node), owned_root_(nullptr) {}

  ~Deserializer() {
    if (owned_root_) xJsonFree(owned_root_);
  }
  Deserializer(const Deserializer&) = delete;
  Deserializer& operator=(const Deserializer&) = delete;
  Deserializer(Deserializer&& o) noexcept
      : node_(o.node_), owned_root_(o.owned_root_) {
    o.node_ = nullptr;
    o.owned_root_ = nullptr;
  }

  /** @brief Parse `s` and return an owning Deserializer at the root. */
  static Result<Deserializer, Error> from_string(const String& s) {
    return from_bytes(reinterpret_cast<const char*>(s.as_bytes().data()), s.len());
  }

  /** @brief Parse a NUL-terminated C string and return an owning Deserializer. */
  static Result<Deserializer, Error> from_string(const char* s) {
    return from_bytes(s, std::strlen(s));
  }

  /** @brief Parse `len` bytes starting at `s`. */
  static Result<Deserializer, Error> from_bytes(const char* s, size_t len) {
    xJson* root = xJsonParseCopy(s, len);
    if (!root) {
      return err(error(ErrorKind::Unexpected, "failed to parse JSON"));
    }
    Deserializer d(root);
    d.owned_root_ = root;
    return ok(std::move(d));
  }

  /* ── Nested access types ── */
  class SeqAccess {
   public:
    explicit SeqAccess(const xJson* arr)
        : arr_(arr), size_(xJsonArraySize(arr)), index_(0) {}

    template <class T>
    Result<Option<T>, Error> next_element() {
      if (index_ >= size_) return ok(Option<T>(none));
      xJson* elem = xJsonArrayGet(arr_, index_);
      ++index_;
      if (!elem) return ok(Option<T>(none));
      Deserializer sub(elem);
      XPP_SERDE_TRY_VAR(v, Deserialize<T>::run(sub));
      return ok(Option<T>(std::move(v)));
    }

   private:
    const xJson* arr_;
    int size_;
    int index_;
  };

  class MapAccess {
   public:
    explicit MapAccess(const xJson* obj) : iter_(xJsonNewIterator(obj)) {}
    ~MapAccess() {
      if (iter_) xJsonFree(iter_);
    }
    MapAccess(const MapAccess&) = delete;
    MapAccess& operator=(const MapAccess&) = delete;

    Result<Option<String>, Error> next_key() {
      if (done_) return ok(Option<String>(none));
      if (!has_pending_) {
        if (!xJsonIteratorNext(iter_)) {
          done_ = true;
          return ok(Option<String>(none));
        }
        has_pending_ = true;
      }
      size_t len = 0;
      const char* key = xJsonIteratorKey(iter_, &len);
      auto r = String::from_utf8(key, len);
      if (!r.is_ok()) {
        return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in object key"));
      }
      return ok(Option<String>(std::move(r).unwrap()));
    }

    template <class V>
    Result<V, Error> next_value() {
      if (!has_pending_) {
        return err(error(ErrorKind::Unexpected, "next_value without next_key"));
      }
      has_pending_ = false;
      xJson* val = xJsonIteratorValue(iter_);
      Deserializer sub(val);
      return Deserialize<V>::run(sub);
    }

    Result<Void, Error> next_value_ignored() {
      if (!has_pending_) {
        return err(error(ErrorKind::Unexpected, "next_value_ignored without next_key"));
      }
      has_pending_ = false;
      return ok(Void{});
    }

   private:
    xJsonIterator* iter_;
    bool has_pending_ = false;
    bool done_ = false;
  };

  /* ── Primitive reads ── */
  Result<bool, Error> deserialize_bool() {
    if (xJsonType(node_) != XJSON_BOOL) {
      return err(error(ErrorKind::InvalidValue, "expected bool"));
    }
    return ok(xJsonBool(node_) != 0);
  }

  Result<int32_t, Error> deserialize_i32() {
    int t = xJsonType(node_);
    int64_t v;
    if (t == XJSON_INT) {
      v = xJsonInt(node_);
    } else if (t == XJSON_DOUBLE) {
      double d = xJsonDouble(node_);
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
    int t = xJsonType(node_);
    if (t == XJSON_INT) return ok(xJsonInt(node_));
    if (t == XJSON_DOUBLE) {
      double d = xJsonDouble(node_);
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
    int t = xJsonType(node_);
    if (t == XJSON_DOUBLE) return ok(xJsonDouble(node_));
    if (t == XJSON_INT) return ok(static_cast<double>(xJsonInt(node_)));
    return err(error(ErrorKind::InvalidValue, "expected number"));
  }

  Result<String, Error> deserialize_str() {
    if (xJsonType(node_) != XJSON_STRING) {
      return err(error(ErrorKind::InvalidValue, "expected string"));
    }
    size_t len = xJsonStringLength(node_);
    const char* s = xJsonString(node_);
    auto r = String::from_utf8(s, len);
    if (!r.is_ok()) {
      return err(error(ErrorKind::InvalidValue, "invalid UTF-8 in JSON string"));
    }
    return ok(std::move(r).unwrap());
  }

  /* ── Composite reads (visitor-driven) ── */
  template <class V>
  auto deserialize_option(V&& visitor) -> decltype(std::declval<V>().visit_none()) {
    if (xJsonType(node_) == XJSON_NULL) {
      return visitor.visit_none();
    }
    return visitor.visit_some(*this);
  }

  template <class V>
  auto deserialize_seq(V&& visitor)
      -> decltype(std::declval<V>().visit_seq(std::declval<SeqAccess&>())) {
    if (xJsonType(node_) != XJSON_ARRAY) {
      return err(error(ErrorKind::InvalidValue, "expected JSON array"));
    }
    SeqAccess seq(node_);
    return visitor.visit_seq(seq);
  }

  template <class V>
  auto deserialize_struct(const char* name, const char* const* fields, size_t n,
                          V&& visitor)
      -> decltype(std::declval<V>().visit_map(std::declval<MapAccess&>())) {
    (void)name;
    (void)fields;
    (void)n;
    if (xJsonType(node_) != XJSON_OBJECT) {
      return err(error(ErrorKind::InvalidValue, "expected JSON object"));
    }
    MapAccess map(node_);
    return visitor.visit_map(map);
  }

 private:
  const xJson* node_;
  xJson* owned_root_;  // nullptr for sub-deserializers
};

}  // namespace json
}  // namespace serde
}  // namespace xpp

#endif  // XPP_SERDE_JSON_H
