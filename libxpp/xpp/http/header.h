/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * header.h — xpp::http::HeaderMap: case-insensitive HTTP header map.
 *
 * Keys are normalised to lowercase (like Go's http.Header and Rust's
 * http::HeaderName).  Lookup is O(log n) case-insensitive.
 * Values are stored as plain std::string (not bytes::Bytes — header
 * values are small and single-owner, refcounting adds no value).
 */

#ifndef XPP_HTTP_HEADER_H
#define XPP_HTTP_HEADER_H

#include <cctype>
#include <map>
#include <string>

#include <xpp/option.h>

namespace xpp {
namespace http {

class HeaderMap {
public:
  using Map            = std::multimap<std::string, std::string>;
  using iterator       = Map::iterator;
  using const_iterator = Map::const_iterator;

  HeaderMap() = default;

  /* ── Mutators ──────────────────────────────────────────────────── */

  /// Insert a header.  @p key is lowercased.
  void insert(std::string key, std::string value) {
    m_map.emplace(lower(key), std::move(value));
  }

  /// Erase all headers matching @p key (case-insensitive).
  void erase(const std::string &key) { m_map.erase(lower(key)); }

  /* ── Query ──────────────────────────────────────────────────────── */

  /// Get the first value for @p key, or none (zero-copy, points into map).
  Option<const std::string &> get(const std::string &key) const {
    auto it = m_map.find(lower(key));
    if (it != m_map.end()) return Option<const std::string &>(it->second);
    return none;
  }

  /// True if any header matches @p key.
  bool contains(const std::string &key) const { return m_map.find(lower(key)) != m_map.end(); }

  bool   empty() const { return m_map.empty(); }
  size_t size() const { return m_map.size(); }

  /* ── Iteration ──────────────────────────────────────────────────── */

  // clang-format off
  const_iterator begin() const { return m_map.begin(); }
  const_iterator end()   const { return m_map.end();   }
  iterator       begin()       { return m_map.begin(); }
  iterator       end()         { return m_map.end();   }
  // clang-format on

  /// All values for @p key (multi-valued headers like Set-Cookie).
  /// Returns an iterator pair — use in range-for via the Values proxy below.
  std::pair<const_iterator, const_iterator> get_all(const std::string &key) const {
    return m_map.equal_range(lower(key));
  }

  /// STL-compatible alias for get_all().
  std::pair<const_iterator, const_iterator> equal_range(const std::string &key) const {
    return get_all(key);
  }

  const Map &raw() const { return m_map; }

private:
  static std::string lower(std::string s) {
    // Fast-path: only ASCII lowercase
    for (auto &c : s) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
  }

  Map m_map;
};

} // namespace http
} // namespace xpp

#endif // XPP_HTTP_HEADER_H
