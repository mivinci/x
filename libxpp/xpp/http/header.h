/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * header.h — HTTP header map.
 *
 * Mirrors hyper::HeaderMap / reqwest::header::HeaderMap. Stores headers
 * as two parallel Vec<String> arrays (lowercased keys + original values)
 * for cache-friendly linear scan. HTTP message headers are typically
 * 5–20 entries; linear scan beats a hash table at this scale (no hashing
 * overhead, no pointer chasing, no per-node allocation).
 *
 * Key comparison is case-insensitive ASCII (RFC 9110 §5.1). Multiple
 * values for the same key (e.g. Set-Cookie) are preserved in insertion
 * order; `get` returns the first, `get_all` returns a view over all.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_HTTP_HEADER_H
#define XPP_HTTP_HEADER_H

#include <cstdint>
#include <utility>

#include <xpp/option.h>
#include <xpp/string.h>
#include <xpp/vec.h>

#include <x/base/base64.h> // xBase64Encode (for _::basic_auth_value)

namespace xpp {
namespace http {

namespace _ {

/**
 * @brief Build the `Authorization: Basic <base64(user:pass)>` header value.
 *
 * RFC 7617: credentials are `user:pass` base64-encoded. Shared by
 * RequestBuilder::basic_auth and ClientBuilder::basic_auth.
 */
inline String basic_auth_value(String user, String password) {
  // Build "user:pass" first, then base64-encode.
  String credentials = std::move(user);
  credentials.push_str(String::from_utf8(":").unwrap());
  credentials.push_str(password);

  auto bytes = credentials.as_bytes();
  // base64 encodes 3 bytes → 4 chars, ceil(len/3)*4 + 1 for NUL.
  size_t    enc_max = (bytes.size() + 2) / 3 * 4 + 1;
  Vec<char> enc;
  enc.reserve(enc_max);
  size_t enc_len = enc_max;
  int    rc      = xBase64Encode(bytes.data(), bytes.size(), enc.data(), &enc_len);
  XPP_ASSERT(rc == 0, "base64 encode failed (buffer sized correctly)");

  String value        = String::from_utf8(enc.data(), enc_len).unwrap();
  String header_value = String::from_utf8("Basic ").unwrap();
  header_value.push_str(value);
  return header_value;
}

} // namespace _

/**
 * @brief Case-insensitive HTTP header collection.
 *
 * Internally stores two parallel arrays: lowercased keys and original
 * values. Insertion order is preserved. Lookup is linear scan.
 *
 * @code
 *   HeaderMap h;
 *   h.insert("Content-Type", "text/plain");
 *   h.insert("Set-Cookie", "a=1");
 *   h.insert("Set-Cookie", "b=2");
 *
 *   h.get("content-type");        // Some("text/plain")
 *   h.get_all("set-cookie").size();  // 2
 * @endcode
 */
class HeaderMap {
public:
  HeaderMap()                                 = default;
  HeaderMap(HeaderMap &&) noexcept            = default;
  HeaderMap &operator=(HeaderMap &&) noexcept = default;
  HeaderMap(const HeaderMap &)                = default;
  HeaderMap &operator=(const HeaderMap &)     = default;

  /* ── Factories ───────────────────────────────────────────────────── */

  /**
   * @brief Construct from a list of (key, value) pairs.
   *
   * Keys are lowercased on insertion. Order is preserved.
   */
  static HeaderMap from_vec(Vec<std::pair<String, String>> entries) {
    HeaderMap h;
    h.m_keys.reserve(entries.len());
    h.m_values.reserve(entries.len());
    for (size_t i = 0; i < entries.len(); ++i) {
      h.insert_key_lowered(lowercase_ascii(entries[i].first), std::move(entries[i].second));
    }
    return h;
  }

  /* ── Insertion ──────────────────────────────────────────────────── */

  /**
   * @brief Insert a header. Does not replace existing entries with the
   * same key — use `erase` first if you want replace semantics.
   *
   * Key is lowercased internally; @p value is stored verbatim.
   */
  void insert(String key, String value) {
    m_keys.push(lowercase_ascii(key));
    m_values.push(std::move(value));
  }

  /// @copydoc insert(String, String)
  void insert(const char *key, const char *value) {
    XPP_ASSERT(key != nullptr && value != nullptr, "HeaderMap::insert(nullptr)");
    m_keys.push(lowercase_ascii(String::from_utf8(key).unwrap()));
    m_values.push(String::from_utf8(value).unwrap());
  }

  /* ── Lookup ─────────────────────────────────────────────────────── */

  /**
   * @brief Case-insensitive get. Returns the first matching value.
   *
   * Returns None if the key is absent. The query key is lowercased
   * internally before comparison; original key bytes are not modified.
   */
  Option<const String &> get(const String &key) const {
    String lower = lowercase_ascii(key);
    for (size_t i = 0; i < m_keys.len(); ++i) {
      if (m_keys[i] == lower) return Option<const String &>(m_values[i]);
    }
    return none;
  }

  /// @copydoc get(const String&)
  Option<const String &> get(const char *key) const {
    return get(String::from_utf8(key).unwrap_or(String()));
  }

  /**
   * @brief Case-insensitive membership test.
   */
  bool contains(const String &key) const {
    String lower = lowercase_ascii(key);
    for (size_t i = 0; i < m_keys.len(); ++i) {
      if (m_keys[i] == lower) return true;
    }
    return false;
  }

  /// @copydoc contains(const String&)
  bool contains(const char *key) const {
    return contains(String::from_utf8(key).unwrap_or(String()));
  }

  /* ── Multi-value access (Set-Cookie, etc.) ──────────────────────── */

  /**
   * @brief Lightweight view over all values for a given key.
   *
   * Backed by const pointers into the HeaderMap's value array. The view
   * is valid only while the HeaderMap is alive and unmodified.
   */
  class Values {
  public:
    class const_iterator {
    public:
      const_iterator(const HeaderMap *map, String key, size_t idx)
          : m_map(map), m_key(std::move(key)), m_idx(idx) {
        advance_to_match();
      }

      const String &operator*() const {
        return m_map->m_values[m_idx];
      }
      const String *operator->() const {
        return &m_map->m_values[m_idx];
      }

      const_iterator &operator++() {
        ++m_idx;
        advance_to_match();
        return *this;
      }

      bool operator!=(const const_iterator &other) const {
        return m_idx != other.m_idx;
      }
      bool operator==(const const_iterator &other) const {
        return m_idx == other.m_idx;
      }

    private:
      void advance_to_match() {
        while (m_idx < m_map->m_keys.len() && m_map->m_keys[m_idx] != m_key) {
          ++m_idx;
        }
      }

      const HeaderMap *m_map;
      String           m_key;
      size_t           m_idx;
    };

    const_iterator begin() const {
      return const_iterator(m_map, m_key, 0);
    }
    const_iterator end() const {
      return const_iterator(m_map, m_key, m_map->m_keys.len());
    }

    /// Number of matching values.
    size_t size() const {
      size_t n = 0;
      for (size_t i = 0; i < m_map->m_keys.len(); ++i) {
        if (m_map->m_keys[i] == m_key) ++n;
      }
      return n;
    }

    bool empty() const {
      return size() == 0;
    }

  private:
    friend class HeaderMap;
    Values(const HeaderMap *map, String key) : m_map(map), m_key(std::move(key)) {}

    const HeaderMap *m_map;
    String           m_key;
  };

  /**
   * @brief View over all values for @p key (case-insensitive).
   *
   * Useful for headers like Set-Cookie that may appear multiple times.
   * The returned view is invalidated by any mutation to this HeaderMap.
   */
  Values get_all(const String &key) const {
    return Values(this, lowercase_ascii(key));
  }

  /// @copydoc get_all(const String&)
  Values get_all(const char *key) const {
    return get_all(String::from_utf8(key).unwrap_or(String()));
  }

  /* ── Removal ────────────────────────────────────────────────────── */

  /**
   * @brief Erase all entries matching @p key (case-insensitive).
   *
   * Uses swap-remove: O(n) but preserves no particular order (insertion
   * order of remaining entries may change). Returns the number of
   * entries erased.
   */
  size_t erase(const String &key) {
    String lower  = lowercase_ascii(key);
    size_t erased = 0;
    size_t i      = 0;
    while (i < m_keys.len()) {
      if (m_keys[i] == lower) {
        m_keys.swap_remove(i);
        m_values.swap_remove(i);
        ++erased;
      } else {
        ++i;
      }
    }
    return erased;
  }

  /// @copydoc erase(const String&)
  size_t erase(const char *key) {
    return erase(String::from_utf8(key).unwrap_or(String()));
  }

  /* ── State ─────────────────────────────────────────────────────── */

  bool empty() const noexcept {
    return m_keys.empty();
  }
  size_t size() const noexcept {
    return m_keys.len();
  }

  void clear() {
    m_keys.clear();
    m_values.clear();
  }

  /* ── Iteration (key, value pairs in storage order) ─────────────── */

  class const_iterator {
  public:
    const_iterator(const HeaderMap *map, size_t idx) : m_map(map), m_idx(idx) {}

    std::pair<const String &, const String &> operator*() const {
      return {m_map->m_keys[m_idx], m_map->m_values[m_idx]};
    }

    const_iterator &operator++() {
      ++m_idx;
      return *this;
    }
    bool operator!=(const const_iterator &other) const {
      return m_idx != other.m_idx;
    }
    bool operator==(const const_iterator &other) const {
      return m_idx == other.m_idx;
    }

  private:
    const HeaderMap *m_map;
    size_t           m_idx;
  };

  const_iterator begin() const {
    return const_iterator(this, 0);
  }
  const_iterator end() const {
    return const_iterator(this, m_keys.len());
  }

  /* ── Direct array access (for serialization) ───────────────────── */

  /** @brief Lowercased keys in storage order. */
  const Vec<String> &keys() const noexcept {
    return m_keys;
  }

  /** @brief Values in storage order, parallel to keys(). */
  const Vec<String> &values() const noexcept {
    return m_values;
  }

private:
  // Storage: parallel arrays. m_keys stores lowercased ASCII; m_values
  // stores the original value bytes (case preserved, including non-ASCII
  // for header values that allow it per RFC).
  Vec<String> m_keys;
  Vec<String> m_values;

  /// Lowercase an ASCII string. Non-ASCII bytes are preserved.
  static String lowercase_ascii(const String &s) {
    auto         bytes = s.as_bytes();
    Vec<uint8_t> out;
    out.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
      uint8_t b = bytes.data()[i];
      if (b >= 'A' && b <= 'Z') b = static_cast<uint8_t>(b - 'A' + 'a');
      out.push(b);
    }
    return String::from_utf8_unchecked(std::move(out));
  }

  /// Insert assuming key is already lowercased.
  void insert_key_lowered(String key_lower, String value) {
    m_keys.push(std::move(key_lower));
    m_values.push(std::move(value));
  }
};

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_HEADER_H
