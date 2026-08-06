/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.h — UTF-8 String backed by Vec<uint8_t>.
 *
 * Modeled after Rust's std::string::String.
 * Guarantees valid UTF-8 at the type level.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_STRING_H
#define XPP_STRING_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/vec.h>

namespace xpp {

/* ── Forward declarations ──────────────────────────────────────────── */

class Chars;
class Utf8Error;

/* ── UTF-8 helpers (internal) ──────────────────────────────────────── */

namespace _ {

// Returns offset of first error, or SIZE_MAX if valid UTF-8.
inline size_t validate_utf8(const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] < 0x80) continue;
        uint32_t cp;
        if ((p[i] & 0xE0) == 0xC0) {
            if (i + 1 >= len) return i;
            if ((p[i+1] & 0xC0) != 0x80) return i;
            cp = ((p[i] & 0x1F) << 6) | (p[i+1] & 0x3F);
            if (cp < 0x80) return i;           // overlong
            i += 1;
        } else if ((p[i] & 0xF0) == 0xE0) {
            if (i + 2 >= len) return i;
            if ((p[i+1] & 0xC0) != 0x80) return i;
            if ((p[i+2] & 0xC0) != 0x80) return i;
            cp = ((p[i] & 0x0F) << 12) | ((p[i+1] & 0x3F) << 6) | (p[i+2] & 0x3F);
            if (cp < 0x800) return i;          // overlong
            if (0xD800 <= cp && cp <= 0xDFFF) return i;  // surrogate
            i += 2;
        } else if ((p[i] & 0xF8) == 0xF0) {
            if (i + 3 >= len) return i;
            if ((p[i+1] & 0xC0) != 0x80) return i;
            if ((p[i+2] & 0xC0) != 0x80) return i;
            if ((p[i+3] & 0xC0) != 0x80) return i;
            cp = ((p[i] & 0x07) << 18) | ((p[i+1] & 0x3F) << 12)
               | ((p[i+2] & 0x3F) << 6)  | (p[i+3] & 0x3F);
            if (cp < 0x10000) return i;        // overlong
            if (cp > 0x10FFFF) return i;        // beyond Unicode
            i += 3;
        } else {
            return i;  // invalid leading byte
        }
    }
    return SIZE_MAX;
}

// Decode one code point. Caller guarantees valid UTF-8 at *p.
inline char32_t decode_one(const uint8_t* p, size_t* consumed) {
    if (*p < 0x80) { *consumed = 1; return *p; }
    if ((*p & 0xE0) == 0xC0) {
        *consumed = 2;
        return ((*p & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((*p & 0xF0) == 0xE0) {
        *consumed = 3;
        return ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    *consumed = 4;
    return ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12)
         | ((p[2] & 0x3F) << 6)  | (p[3] & 0x3F);
}

// Encode one code point. Caller guarantees cp is a valid scalar value.
inline size_t encode_one(char32_t cp, uint8_t* out) {
    if (cp < 0x80)      { out[0] = static_cast<uint8_t>(cp); return 1; }
    if (cp < 0x800)     { out[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
                          out[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000)   { out[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
                          out[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
                          out[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); return 3; }
    { out[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
      out[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
      out[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      out[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F)); return 4; }
}

// Returns true if the byte at offset is the start of a code point.
inline bool is_codepoint_boundary(const uint8_t* p, size_t offset) {
    if (offset == 0) return true;
    // Continuation bytes: 10xxxxxx
    return (p[offset] & 0xC0) != 0x80;
}

// Count code points in [p, p+len). O(n).
inline size_t count_code_points(const uint8_t* p, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) count++;  // not continuation byte = start of CP
    }
    return count;
}

// Returns the number of bytes in the code point starting at *p.
// Caller guarantees valid UTF-8.
inline size_t cp_byte_len(const uint8_t* p) {
    if (*p < 0x80) return 1;
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    return 4;
}

} // namespace _

/* ──────────────────────── Utf8Error ──────────────────────── */

class Utf8Error {
public:
    size_t error_pos() const noexcept { return m_error_pos; }

    Vec<uint8_t> into_bytes() && { return std::move(m_bytes); }

private:
    friend class String;
    Utf8Error(Vec<uint8_t> bytes, size_t pos)
        : m_bytes(std::move(bytes)), m_error_pos(pos) {}

    Vec<uint8_t> m_bytes;
    size_t       m_error_pos;
};

/* ──────────────────────── Chars ──────────────────────── */

class Chars {
public:
    char32_t operator*() const noexcept { return m_current; }

    Chars& operator++() noexcept {
        XPP_DEBUG_ASSERT(m_pos < m_end, "chars iterator past end");
        size_t consumed;
        m_current = _::decode_one(m_pos, &consumed);
        m_pos += consumed;
        if (m_pos >= m_end) m_pos = m_end;  // normalise
        return *this;
    }

    bool operator==(const Chars& other) const noexcept {
        return m_pos == other.m_pos && m_end == other.m_end;
    }
    bool operator!=(const Chars& other) const noexcept { return !(*this == other); }

    size_t count() noexcept {
        size_t n = 0;
        const uint8_t* saved = m_pos;
        while (m_pos < m_end) {
            m_pos += _::cp_byte_len(m_pos);
            n++;
        }
        m_pos = saved;
        // restore m_current — decode the saved position
        if (m_pos < m_end) {
            size_t consumed;
            m_current = _::decode_one(m_pos, &consumed);
        }
        return n;
    }

    /* ── Range-for support ── */

    Chars begin() const { return *this; }
    Chars end()   const { return Chars(m_end, m_end); }

private:
    friend class String;
    Chars(const uint8_t* p, const uint8_t* end) noexcept
        : m_pos(p), m_end(end), m_current(0) {
        if (p && p < end) {
            size_t consumed;
            m_current = _::decode_one(p, &consumed);
        }
    }

    const uint8_t* m_pos;
    const uint8_t* m_end;
    char32_t       m_current;
};

/* ──────────────────────── String ──────────────────────── */

class String {
public:
    /* ── Construction ────────────────────────────────────────────── */

    String() = default;

    explicit String(size_t capacity)
        : m_bytes(capacity) {}

    static Result<String, Utf8Error> from_utf8(Vec<uint8_t> bytes) {
        size_t error_pos = _::validate_utf8(bytes.data(), bytes.len());
        if (error_pos != SIZE_MAX) {
            return Result<String, Utf8Error>(err, Utf8Error(std::move(bytes), error_pos));
        }
        String s;
        s.m_bytes = std::move(bytes);
        return Result<String, Utf8Error>(ok, std::move(s));
    }

    static Result<String, Utf8Error> from_utf8(const char* s) {
        XPP_ASSERT(s != nullptr, "from_utf8(NULL)");
        return from_utf8(s, cstr_len(s));
    }

    static Result<String, Utf8Error> from_utf8(const char* s, size_t len) {
        return from_utf8(reinterpret_cast<const uint8_t*>(s), len);
    }

    static String from_utf8_unchecked(Vec<uint8_t> bytes) {
        String s;
        s.m_bytes = std::move(bytes);
        return s;
    }

    /* ── Bytes views ─────────────────────────────────────────────── */

    Span<const uint8_t> as_bytes() const noexcept {
        return Span<const uint8_t>(m_bytes.data(), m_bytes.len());
    }

    Vec<uint8_t> into_bytes() && noexcept { return std::move(m_bytes); }


    /* ── Length ──────────────────────────────────────────────────── */

    size_t len()      const noexcept { return m_bytes.len(); }
    size_t char_len() const noexcept { return _::count_code_points(m_bytes.data(), m_bytes.len()); }
    bool   empty()    const noexcept { return m_bytes.empty(); }

    /* ── Capacity ────────────────────────────────────────────────── */

    size_t capacity()                     const noexcept { return m_bytes.capacity(); }
    void   reserve(size_t additional)                   { m_bytes.reserve(additional); }
    Result<void, AllocError> try_reserve(size_t n)      { return m_bytes.try_reserve(n); }
    void   shrink_to_fit()                              { m_bytes.shrink_to_fit(); }
    Result<void, AllocError> try_shrink_to_fit()        { return m_bytes.try_shrink_to_fit(); }

    /* ── Substring ───────────────────────────────────────────────── */

    String substr(size_t offset, size_t count = SIZE_MAX) const {
        size_t end = (count == SIZE_MAX) ? len() : offset + count;
        XPP_ASSERT(end <= len(), "substr range out of bounds");
        XPP_ASSERT(_::is_codepoint_boundary(m_bytes.data(), offset),
                   "substr: offset not on code point boundary");
        XPP_ASSERT(_::is_codepoint_boundary(m_bytes.data(), end),
                   "substr: end not on code point boundary");

        String s(end - offset);
        for (size_t i = offset; i < end; i++) {
            s.m_bytes.push(m_bytes[i]);
        }
        XPP_ASSERT(s.m_bytes.len() == (end - offset), "substr construction failed");
        return s;
    }

    /* ── Find ────────────────────────────────────────────────────── */

    Option<size_t> find(const String& pattern) const {
        return find_bytes(pattern.as_bytes().data(), pattern.len());
    }
    Option<size_t> find(const char* pattern) const {
        return find_bytes(reinterpret_cast<const uint8_t*>(pattern), cstr_len(pattern));
    }

    Option<size_t> rfind(const String& pattern) const {
        return rfind_bytes(pattern.as_bytes().data(), pattern.len());
    }
    Option<size_t> rfind(const char* pattern) const {
        return rfind_bytes(reinterpret_cast<const uint8_t*>(pattern), cstr_len(pattern));
    }

    bool contains(const String& pattern) const { return find(pattern).is_some(); }
    bool contains(const char* pattern)   const { return find(pattern).is_some(); }

    bool starts_with(const String& prefix) const {
        if (prefix.len() > len()) return false;
        return bytes_eq(m_bytes.data(), prefix.m_bytes.data(), prefix.len());
    }
    bool starts_with(const char* prefix) const {
        size_t plen = cstr_len(prefix);
        if (plen > len()) return false;
        return bytes_eq(m_bytes.data(), reinterpret_cast<const uint8_t*>(prefix), plen);
    }
    bool ends_with(const String& suffix) const {
        if (suffix.len() > len()) return false;
        return bytes_eq(
            m_bytes.data() + len() - suffix.len(),
            suffix.m_bytes.data(),
            suffix.len());
    }
    bool ends_with(const char* suffix) const {
        size_t slen = cstr_len(suffix);
        if (slen > len()) return false;
        return bytes_eq(
            m_bytes.data() + len() - slen,
            reinterpret_cast<const uint8_t*>(suffix), slen);
    }

    /* ── Mutation ────────────────────────────────────────────────── */

    void push(char32_t cp) {
        XPP_ASSERT(cp <= 0x10FFFF, "push: code point exceeds U+10FFFF");
        XPP_ASSERT(cp < 0xD800 || cp > 0xDFFF, "push: surrogate half");
        uint8_t buf[4];
        size_t  n = _::encode_one(cp, buf);
        for (size_t i = 0; i < n; i++) {
            m_bytes.push(buf[i]);
        }
    }

    void push_str(const String& other) {
        for (size_t i = 0; i < other.len(); i++) {
            m_bytes.push(other.m_bytes[i]);
        }
    }

    Result<void, AllocError> try_push_str(const String& other) {
        auto r = m_bytes.try_reserve(other.len());
        if (r.is_err()) return r;
        for (size_t i = 0; i < other.len(); i++) {
            m_bytes.push(other.m_bytes[i]);
        }
        return ok;
    }

    void push_str(const char* s) {
        XPP_ASSERT(s != nullptr, "push_str(NULL)");
        size_t slen = cstr_len(s);
        m_bytes.reserve(slen);
        for (size_t i = 0; i < slen; i++) {
            m_bytes.push(static_cast<uint8_t>(s[i]));
        }
    }

    Option<char32_t> pop() {
        if (m_bytes.len() == 0) return none;

        size_t pos = m_bytes.len() - 1;
        while (pos > 0 && (m_bytes[pos] & 0xC0) == 0x80) {
            pos--;
        }

        size_t consumed;
        char32_t cp = _::decode_one(m_bytes.data() + pos, &consumed);
        m_bytes.truncate(pos);
        return Option<char32_t>(cp);
    }

    void insert(size_t byte_pos, char32_t cp) {
        XPP_ASSERT(byte_pos <= len(), "insert: position out of bounds");
        XPP_ASSERT(byte_pos == len() || _::is_codepoint_boundary(m_bytes.data(), byte_pos),
                   "insert: not a code point boundary");
        XPP_ASSERT(cp <= 0x10FFFF, "insert: code point exceeds U+10FFFF");
        XPP_ASSERT(cp < 0xD800 || cp > 0xDFFF, "insert: surrogate half");

        uint8_t buf[4];
        size_t  n = _::encode_one(cp, buf);

        // Shift tail right by n bytes
        size_t old_len = m_bytes.len();
        m_bytes.resize(old_len + n, 0);
        for (size_t i = old_len; i > byte_pos; i--) {
            m_bytes[i + n - 1] = m_bytes[i - 1];
        }
        for (size_t i = 0; i < n; i++) {
            m_bytes[byte_pos + i] = buf[i];
        }
    }

    void insert_str(size_t byte_pos, const String& s) {
        XPP_ASSERT(byte_pos <= len(), "insert_str: position out of bounds");
        XPP_ASSERT(byte_pos == len() || _::is_codepoint_boundary(m_bytes.data(), byte_pos),
                   "insert_str: not a code point boundary");

        size_t slen = s.len();
        size_t old_len = m_bytes.len();
        m_bytes.resize(old_len + slen, 0);
        for (size_t i = old_len; i > byte_pos; i--) {
            m_bytes[i + slen - 1] = m_bytes[i - 1];
        }
        for (size_t i = 0; i < slen; i++) {
            m_bytes[byte_pos + i] = s.m_bytes[i];
        }
    }

    char32_t remove(size_t byte_pos) {
        XPP_ASSERT(byte_pos < len(), "remove: position out of bounds");
        XPP_ASSERT(_::is_codepoint_boundary(m_bytes.data(), byte_pos),
                   "remove: not a code point boundary");

        size_t consumed;
        char32_t cp = _::decode_one(m_bytes.data() + byte_pos, &consumed);

        // Shift tail left
        for (size_t i = byte_pos + consumed; i < m_bytes.len(); i++) {
            m_bytes[i - consumed] = m_bytes[i];
        }
        m_bytes.truncate(m_bytes.len() - consumed);
        return cp;
    }

    void truncate(size_t new_byte_len) {
        XPP_ASSERT(new_byte_len <= len(), "truncate: new length exceeds current");
        XPP_ASSERT(new_byte_len == 0 || _::is_codepoint_boundary(m_bytes.data(), new_byte_len),
                   "truncate: not a code point boundary");
        m_bytes.truncate(new_byte_len);
    }

    void clear() { m_bytes.clear(); }

    String split_off(size_t byte_pos) {
        XPP_ASSERT(byte_pos <= len(), "split_off: position out of bounds");
        XPP_ASSERT(byte_pos == 0 || byte_pos == len() || _::is_codepoint_boundary(m_bytes.data(), byte_pos),
                   "split_off: not a code point boundary");

        String tail;
        tail.m_bytes = m_bytes.split_off(byte_pos);
        return tail;
    }

    /* ── Replace ─────────────────────────────────────────────────── */

    String replace(const String& from, const String& to) const {
        if (from.len() == 0) return *this;
        String result;
        size_t pos = 0;
        while (pos < len()) {
            auto found = find_bytes_from(from.as_bytes().data(), from.len(), pos);
            if (found.is_none()) break;
            size_t fpos = found.unwrap();
            // Copy bytes before match
            for (size_t i = pos; i < fpos; i++) result.m_bytes.push(m_bytes[i]);
            // Copy replacement
            for (size_t i = 0; i < to.len(); i++) result.m_bytes.push(to.m_bytes[i]);
            pos = fpos + from.len();
        }
        // Copy remaining
        for (size_t i = pos; i < len(); i++) result.m_bytes.push(m_bytes[i]);
        return result;
    }

    String replacen(const String& from, const String& to, size_t n) const {
        if (from.len() == 0 || n == 0) return *this;
        String result;
        size_t pos = 0;
        size_t replaced = 0;
        while (pos < len() && replaced < n) {
            auto found = find_bytes_from(from.as_bytes().data(), from.len(), pos);
            if (found.is_none()) break;
            size_t fpos = found.unwrap();
            for (size_t i = pos; i < fpos; i++) result.m_bytes.push(m_bytes[i]);
            for (size_t i = 0; i < to.len(); i++) result.m_bytes.push(to.m_bytes[i]);
            pos = fpos + from.len();
            replaced++;
        }
        for (size_t i = pos; i < len(); i++) result.m_bytes.push(m_bytes[i]);
        return result;
    }

    String repeat(size_t n) const {
        String result(len() * n);
        for (size_t i = 0; i < n; i++) {
            for (size_t j = 0; j < len(); j++) {
                result.m_bytes.push(m_bytes[j]);
            }
        }
        return result;
    }

    /* ── Trim (ASCII-only) ────────────────────────────────────────── */

    static bool is_ascii_whitespace(uint8_t b) {
        return b == 0x20 || (b >= 0x09 && b <= 0x0D);
    }

    String trim() const {
        size_t start = 0;
        while (start < len() && is_ascii_whitespace(m_bytes[start])) start++;
        size_t end = len();
        while (end > start && is_ascii_whitespace(m_bytes[end - 1])) end--;
        return substr(start, end - start);
    }

    String trim_start() const {
        size_t start = 0;
        while (start < len() && is_ascii_whitespace(m_bytes[start])) start++;
        return substr(start);
    }

    String trim_end() const {
        size_t end = len();
        while (end > 0 && is_ascii_whitespace(m_bytes[end - 1])) end--;
        return substr(0, end);
    }

    /* ── Filter ───────────────────────────────────────────────────── */

    template <class Pred>
    void retain(Pred pred) {
        // Build a new byte buffer from code points that pass the predicate.
        Vec<uint8_t> kept;
        auto it = chars();
        auto ed = it.end();
        while (it != ed) {
            char32_t cp = *it;
            if (pred(cp)) {
                uint8_t buf[4];
                size_t  n = _::encode_one(cp, buf);
                for (size_t i = 0; i < n; i++) kept.push(buf[i]);
            }
            ++it;
        }
        m_bytes = std::move(kept);
    }

    /* ── Iteration ────────────────────────────────────────────────── */

    Chars chars() const noexcept {
        return Chars(m_bytes.data(), m_bytes.data() + m_bytes.len());
    }

    /* ── Comparison ───────────────────────────────────────────────── */

    bool operator==(const String& other) const noexcept {
        if (len() != other.len()) return false;
        return bytes_eq(m_bytes.data(), other.m_bytes.data(), len());
    }
    bool operator!=(const String& other) const noexcept { return !(*this == other); }

    bool operator<(const String& other) const noexcept {
        size_t min_len = len() < other.len() ? len() : other.len();
        for (size_t i = 0; i < min_len; i++) {
            if (m_bytes[i] < other.m_bytes[i]) return true;
            if (m_bytes[i] > other.m_bytes[i]) return false;
        }
        return len() < other.len();
    }

    bool operator==(const char* other) const noexcept {
        size_t olen = cstr_len(other);
        if (len() != olen) return false;
        return bytes_eq(m_bytes.data(), reinterpret_cast<const uint8_t*>(other), len());
    }
    bool operator!=(const char* other) const noexcept { return !(*this == other); }

private:
    // strlen replacement — avoids macOS C++11 C library issues.
    static size_t cstr_len(const char* s) {
        const char* p = s;
        while (*p) p++;
        return static_cast<size_t>(p - s);
    }

    // C-string constructors use this internal overload
    static Result<String, Utf8Error> from_utf8(const uint8_t* data, size_t len) {
        size_t error_pos = _::validate_utf8(data, len);
        if (error_pos != SIZE_MAX) {
            Vec<uint8_t> bytes(len);
            for (size_t i = 0; i < len; i++) bytes.push(data[i]);
            return Result<String, Utf8Error>(err, Utf8Error(std::move(bytes), error_pos));
        }
        String s(len);
        for (size_t i = 0; i < len; i++) {
            s.m_bytes.push(data[i]);
        }
        return Result<String, Utf8Error>(ok, std::move(s));
    }

    // Simple byte equality check — avoids memcmp portability issues.
    static bool bytes_eq(const uint8_t* a, const uint8_t* b, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    // memmem-style byte search
    Option<size_t> find_bytes(const uint8_t* pat, size_t plen) const {
        return find_bytes_from(pat, plen, 0);
    }

    Option<size_t> find_bytes_from(const uint8_t* pat, size_t plen, size_t start) const {
        if (plen == 0) return Option<size_t>(start <= len() ? start : SIZE_MAX);
        if (plen > len() || start > len() - plen) return none;

        const uint8_t* haystack = m_bytes.data();
        // naive O(n*m) — sufficient for L0 string search
        for (size_t i = start; i <= len() - plen; i++) {
            bool match = true;
            for (size_t j = 0; j < plen; j++) {
                if (haystack[i + j] != pat[j]) { match = false; break; }
            }
            if (match) return Option<size_t>(i);
        }
        return none;
    }

    Option<size_t> rfind_bytes(const uint8_t* pat, size_t plen) const {
        if (plen == 0) return Option<size_t>(len());
        if (plen > len()) return none;

        const uint8_t* haystack = m_bytes.data();
        for (size_t i = len() - plen; ; i--) {
            bool match = true;
            for (size_t j = 0; j < plen; j++) {
                if (haystack[i + j] != pat[j]) { match = false; break; }
            }
            if (match) return Option<size_t>(i);
            if (i == 0) break;
        }
        return none;
    }

    Vec<uint8_t> m_bytes;
};

} // namespace xpp

#endif // XPP_STRING_H
