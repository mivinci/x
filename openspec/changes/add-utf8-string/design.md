# xpp::String — UTF-8 String (L0 Design)

## Summary

A `String` type that guarantees valid UTF-8 at the type level. Internal storage
is `std::vector<uint8_t>` — a byte buffer with no text semantics. The type
system draws a bright line: `vector<uint8_t>` = opaque bytes, `String` = valid
UTF-8. Purely L0 scope: encode/decode/validate/iterate — no normalization, no
case folding, no grapheme clusters.

## Motivation

`std::string` conflates "text" and "bytes". Rust separates them cleanly:
`Vec<u8>` is raw bytes, `String` is guaranteed UTF-8, `&[u8]` / `&str` are
borrowed views. xpp mirrors this:

1. **Type safety** — `const String&` means "valid UTF-8". Raw bytes use
   `std::vector<uint8_t>`. No ambiguity.
1. **Correct substring** — byte-offset-based but guaranteed to land on code point
   boundaries, never slicing a multi-byte character in half.
1. **Code point iteration** — `chars()` yields `char32_t`, not raw bytes.
1. **Zero dependencies** — UTF-8 decode is a tiny state machine with no lookup
   tables. Entire implementation fits in ~400 lines.

Everything beyond L0 (normalization, case folding, grapheme boundaries) belongs
in a future `libxpp-ext/unicode` extension library.

## Rust ↔ xpp Type Mapping

| Rust | xpp | Notes |
|------|-----|-------|
| `Vec<u8>` | `std::vector<uint8_t>` | Raw bytes, no encoding contract |
| `String` | `String` | Guaranteed valid UTF-8 |
| `&[u8]` | `Span<const uint8_t>` | Borrowed byte view |
| `&str` | (no equivalent yet) | Future: `Str` or `StringRef` |
| `String::from_utf8(Vec<u8>)` | `String::from_utf8(vector<uint8_t>)` | Validates + takes ownership |
| `String::as_bytes() -> &[u8]` | `String::as_bytes() -> Span<const uint8_t>` | Read-only byte view |
| `String::into_bytes() -> Vec<u8>` | `String::into_bytes() -> vector<uint8_t>` | Consume, recover raw bytes |
| `FromUtf8Error` | `Utf8Error` | Owns the original bytes |
| `str::chars()` | `String::chars()` | Code point iterator |

## API Reference

```cpp
namespace xpp {

/* ──────────────────────── String ──────────────────────── */

class String {
public:
    /* ── Construction ── */

    String() = default;

    /** Validate a byte buffer as UTF-8. Takes ownership via move.
     *  On failure, returns the original bytes in Utf8Error.
     *
     *  Complexity: O(n) single-pass validation scan.
     *
     *  Usage:
     *    std::vector<uint8_t> buf = read_from_network();
     *    auto s = String::from_utf8(std::move(buf));
     */
    static Result<String, Utf8Error> from_utf8(std::vector<uint8_t> bytes);

    /** Convenience overload for C-string literals. Copies + validates.
     *  s must not be NULL; behaviour is undefined if it is (debug builds
     *  assert s != nullptr). Equivalent to from_utf8(s, strlen(s)).
     */
    static Result<String, Utf8Error> from_utf8(const char* s);

    /** Validate + construct from [data, data+len). */
    static Result<String, Utf8Error> from_utf8(const char* s, size_t len);

    /** Construct without validation. Caller guarantees the bytes are valid UTF-8.
     *  Behaviour is undefined if they aren't.
     */
    static String from_utf8_unchecked(std::vector<uint8_t> bytes);

    /** Copy / move. */
    String(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;

    /* ── Bytes views ── */

    /** Borrowed read-only view of the raw UTF-8 bytes. O(1). */
    Span<const uint8_t> as_bytes() const noexcept;

    /** Consume the String and recover the underlying byte buffer. O(1). */
    std::vector<uint8_t> into_bytes() && noexcept;

    /** Consume the String and return a std::string. The internal byte
     *  buffer is copied into a std::string (O(n)). For zero-copy interop,
     *  callers should use as_bytes() which provides a Span<const uint8_t>
     *  view without allocation.
     *
     *  Useful for interop with std APIs (file paths, logging, HTTP body).
     */
    std::string into_std_string() &&;

    /* ── Length ── */

    /** Byte count. O(1). Equivalent to as_bytes().size(). */
    size_t len() const noexcept;

    /** Code point count. O(n) — scans the entire string.
     *  Avoid calling in hot loops. For repeated counting, cache the result
     *  or build an IndexTable (future).
     */
    size_t char_len() const noexcept;

    /** True if byte length is zero. O(1). */
    bool empty() const noexcept;

    /* ── Substring ── */

    /** Slice [offset, offset+count) in bytes.
     *  XPP_ASSERT that offset and offset+count fall on code point boundaries.
     *  If count == SIZE_MAX, take from offset to end.
     *  XPP_ASSERT that offset + count does not overflow (offset + SIZE_MAX
     *  wraps; the implementation catches this with an explicit end-fixup
     *  before arithmetic).
     */
    String substr(size_t offset, size_t count = SIZE_MAX) const;

    /* ── Find ── */

    /** Byte position of the first occurrence of `pattern`, or None.
     *  `pattern` must be valid UTF-8. O(n*m) — memmem under the hood.
     */
    Option<size_t> find(const String& pattern) const;

    /** Convenience: find by C-string literal. */
    Option<size_t> find(const char* pattern) const;

    Option<size_t> rfind(const String& pattern) const;
    Option<size_t> rfind(const char* pattern) const;

    bool starts_with(const String& prefix) const;
    bool starts_with(const char* prefix) const;
    bool ends_with(const String& suffix) const;
    bool ends_with(const char* suffix) const;

    /* ── Mutation ── */

    /** Append a code point (encoded as 1–4 UTF-8 bytes). O(1) amortised.
     *
     *  XPP_ASSERT that cp is a valid Unicode scalar value:
     *    — Not a surrogate (U+D800–U+DFFF)
     *    — Not exceeding U+10FFFF
     *  Violating this would break the String's valid-UTF-8 invariant.
     */
    void push(char32_t cp);

    /** Append another String's bytes. O(n). */
    void push_str(const String& other);

    /** Remove all bytes. */
    void clear();

    /* ── Comparison ── */

    /** Byte-wise equality. Two String objects are equal iff their UTF-8
     *  representations are identical. Does NOT do Unicode normalisation —
     *  "é" (precomposed U+00E9) and "e\u0301" (decomposed) will NOT compare
     *  equal. This is correct for L0; callers needing normalisation-aware
     *  comparison must normalise first (libxpp-ext).
     */
    bool operator==(const String& other) const noexcept;

    /** Byte-wise ordering (lexicographic on UTF-8). */
    bool operator<(const String& other) const noexcept;

private:
    std::vector<uint8_t> m_bytes;  // invariant: valid UTF-8
};

/* ──────────────────────── Chars ──────────────────────── */

/** Code-point iterator. Input iterator, single-pass (forward only).
 *
 *  Each dereference decodes the current multi-byte sequence into char32_t.
 *  Invalid bytes will never appear because the String invariant guarantees
 *  valid UTF-8. Increment advances past the current code point's byte span.
 *
 *  Usage:
 *    for (char32_t cp : s.chars()) { ... }
 *    auto it = s.chars();
 *    while (it != Chars::end()) { char32_t c = *it; ++it; }
 */
class Chars {
public:
    char32_t operator*() const noexcept;

    /** Advance past the current code point. Must not be called when the
     *  iterator has already reached end(); behaviour is undefined if it is
     *  (debug builds assert m_pos < m_end).
     */
    Chars& operator++() noexcept;

    bool operator==(const Chars& other) const noexcept;
    bool operator!=(const Chars& other) const noexcept;

    /** Count remaining code points from current position to end. O(remaining).
     *
     *  Advances the iterator in-place, then returns to the original position.
     *  After this call the iterator is unchanged. NOT const — iterating is
     *  inherently stateful even though we restore position.
     *
     *  Equivalent to Rust's Chars::count().
     */
    size_t count() noexcept;

    /** Sentinel for range-for. */
    static Chars end() noexcept;

private:
    friend class String;
    Chars(const uint8_t* p, const uint8_t* end) noexcept;
    const uint8_t* m_pos;
    const uint8_t* m_end;
    char32_t m_current;
};

/* ──────────────────────── Utf8Error ──────────────────────── */

/** Returned when from_utf8() validation fails. Owns the original bytes so
 *  the caller can recover — log the error, fall back to a lossy conversion,
 *  or abort.
 */
class Utf8Error {
public:
    /** Byte offset of the first error. */
    size_t error_pos() const noexcept;

    /** Recover the original bytes.
     *  If the bytes were moved from a String constructor, this call
     *  returns them — the caller hasn't lost any data.
     */
    std::vector<uint8_t> into_bytes() &&;

private:
    friend class String;
    Utf8Error(std::vector<uint8_t> bytes, size_t pos);
    std::vector<uint8_t> m_bytes;
    size_t m_error_pos;
};

}  // namespace xpp
```

## Internal Implementation

### Validation

Single-pass scan with a tiny state machine — no tables:

```cpp
// Returns offset of first error, or SIZE_MAX if valid.
size_t validate_utf8(const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] < 0x80) continue;
        size_t start = i;
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
```

Checks performed:
- Continuation bytes must be `10xxxxxx` (`(byte & 0xC0) == 0x80`)
- Overlong encodings (e.g. U+002F encoded as 2 bytes)
- Surrogate halves (U+D800–U+DFFF) — forbidden in UTF-8
- Beyond U+10FFFF — outside Unicode range

Total validator ~50 lines, still zero tables, still single-pass.

### Decode one code point

```cpp
char32_t decode_one(const uint8_t* p, size_t* consumed) {
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
```

### Encode one code point

```cpp
size_t encode_one(char32_t cp, uint8_t* out) {
    if (cp < 0x80)      { out[0] = cp; return 1; }
    if (cp < 0x800)     { out[0] = 0xC0|(cp>>6); out[1] = 0x80|(cp&0x3F); return 2; }
    if (cp < 0x10000)   { out[0] = 0xE0|(cp>>12); out[1] = 0x80|((cp>>6)&0x3F);
                          out[2] = 0x80|(cp&0x3F); return 3; }
    { out[0] = 0xF0|(cp>>18); out[1] = 0x80|((cp>>12)&0x3F);
      out[2] = 0x80|((cp>>6)&0x3F); out[3] = 0x80|(cp&0x3F); return 4; }
}
```

### Substring boundary check

```cpp
// Returns true if byte at `offset` is the start of a code point.
bool is_codepoint_boundary(const uint8_t* p, size_t offset) {
    if (offset == 0) return true;
    // Continuation bytes: 10xxxxxx
    return (p[offset] & 0xC0) != 0x80;
}
```

## Size and Layout

```
sizeof(String)    == sizeof(std::vector<uint8_t>) == 24 (ptr + size + capacity)
sizeof(Chars)     == 24 (two pointers + cached char32_t)
sizeof(Utf8Error) == 32 (vector 24B + error_pos 8B)
```

No hidden allocations beyond the byte vector's heap buffer.

## Edge Cases

| Input | Behaviour |
|-------|-----------|
| `from_utf8("")` | Ok(String with empty bytes) |
| `from_utf8("hello\0world")` | Ok — U+0000 is valid UTF-8 (0x00) |
| `from_utf8("\xC0\x80")` | Err — overlong encoding of U+0000 |
| `from_utf8("\xED\xA0\x80")` | Err — surrogate half U+D800 |
| `from_utf8("\xF4\x90\x80\x80")` | Err — exceeds U+10FFFF |
| `s.substr(1, 3)` on `"你好"` | Assert fails — offset 1 is continuation byte |
| `s.substr(0, 3)` on `"你好"` | Returns `"你"` |
| `s.chars()` on empty | iterator == Chars::end() immediately |
| `s.char_len()` on empty | 0 |
| `"é" == "e\u0301"` | false (byte-wise comparison, NFC not applied) |

## Rust `String` Method Coverage

This section maps every method on Rust's `std::string::String` to xpp's
equivalent (or explains why it's deferred). Legend:

| Tag | Meaning |
|-----|---------|
| ✅ L0 | Implemented in xpp `String` — no Unicode tables needed |
| ⚠️ L0 | Implementable but deferred (edge cases, iterator complexity) |
| ❌ L1 | Needs Unicode tables — belongs in `libxpp-ext/unicode` |

---

### 1. Construction

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `String::new()` | `String()` default constructor | ✅ L0 | Empty string, zero allocation |
| `String::with_capacity(n)` | `String::with_capacity(size_t n)` | ✅ L0 | Forward to `vector.reserve(n)` |
| `String::from_utf8(Vec<u8>)` | `String::from_utf8(vector<uint8_t>)` | ✅ L0 | Validate + take ownership |
| `String::from_utf8_lossy(&[u8])` | `String::from_utf8_lossy(Span<const uint8_t>)` | ⚠️ L0 | Replace bad bytes with U+FFFD. Trivial impl (reuses validator). Deferred to L0.1. |
| `String::from_utf16(&[u16])` | — | ❌ L1 | Needs UTF-16 codec. Rarely needed in C ecosystem. |
| `from_utf16_lossy` | — | ❌ L1 | Same reason |
| `From<&str>` / `to_string()` | `from_utf8(const char*)` + copy ctor | ✅ L0 | Already covered |

### 2. Conversion / Views

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `as_bytes() -> &[u8]` | `as_bytes() -> Span<const uint8_t>` | ✅ L0 | O(1), no copy |
| `as_str() -> &str` | (no Str type yet) | ⚠️ L0 | Requires `Str` borrowed type. Deferred. |
| `into_bytes() -> Vec<u8>` | `into_bytes() -> vector<uint8_t>` | ✅ L0 | Consuming, O(1) |
| `into_std_string()` | `into_std_string() -> std::string` | ✅ L0 | `reinterpret_cast` of internal bytes. O(1). std interop (paths, HTTP, logging). |
| `into_boxed_str()` | — | ❌ | No `Box<str>` equivalent in C++ |
| `from_raw_parts` / `into_raw_parts` | — | ❌ | Unsafe internals, not for public API |

### 3. Capacity

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `capacity()` | `capacity()` | ✅ L0 | Forward to `vector.capacity()` |
| `reserve(n)` | `reserve(size_t n)` | ✅ L0 | Forward to `vector.reserve()` |
| `reserve_exact(n)` | `reserve_exact(size_t n)` | ✅ L0 | Forward to `vector.reserve()` (C++11 doesn't have `shrink_to_fit` guarantee anyway) |
| `shrink_to_fit()` | `shrink_to_fit()` | ✅ L0 | Forward to `vector.shrink_to_fit()` |
| `shrink_to(n)` | — | ❌ | C++11 `vector` doesn't support this |

### 4. Push / Pop / Insert / Remove

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `push(ch: char)` | `push(char32_t cp)` | ✅ L0 | Encodes 1-4 bytes via `encode_one()` |
| `push_str(s: &str)` | `push_str(const String&)` | ✅ L0 | Byte append |
| `pop() -> Option<char>` | `pop() -> Option<char32_t>` | ✅ L0 | Decode last code point, truncate bytes |
| `insert(idx, ch)` | `insert(size_t byte_pos, char32_t cp)` | ✅ L0 | `XPP_ASSERT` byte_pos is on code point boundary |
| `insert_str(idx, s)` | `insert_str(size_t byte_pos, const String&)` | ✅ L0 | Same boundary check |
| `remove(idx) -> char` | `remove(size_t byte_pos) -> char32_t` | ✅ L0 | Decode CP at pos, erase its byte span |
| `retain(pred)` | `retain(Func)` | ⚠️ L0 | Iterator-based filter. Deferred — needs callback API design. |
| `truncate(new_len)` | `truncate(size_t new_byte_len)` | ✅ L0 | Cut at byte boundary. `XPP_ASSERT` it's a CP boundary. |
| `clear()` | `clear()` | ✅ L0 | Zero length |

### 5. Substring / Split

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `[range]` slicing | `substr(offset, count)` | ✅ L0 | Already in design |
| `split_off(at) -> String` | `split_off(size_t byte_pos) -> String` | ✅ L0 | Split at boundary, return tail |
| `split(pat)` / `splitn()` | `Split` iterator returning `String` | ⚠️ L0 | Useful but iterator type adds complexity. Deferred. |
| `rsplit(pat)` / `rsplitn()` | same | ⚠️ L0 | Same |
| `lines()` | `lines() -> Lines` iterator | ⚠️ L0 | Trivial (\n split). Deferred with Split. |
| `drain(range)` | — | ❌ | Requires mutable view into String, complex. |

### 6. Search / Contains

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `find(pat) -> Option<usize>` | `find(const String&) -> Option<size_t>` | ✅ L0 | Already in design |
| `rfind(pat)` | `rfind(const String&)` | ✅ L0 | Already in design |
| `contains(pat) -> bool` | `contains(const String&) -> bool` | ✅ L0 | `find(p).is_some()` |
| `starts_with(pat)` | `starts_with(const String&)` | ✅ L0 | Already in design |
| `ends_with(pat)` | `ends_with(const String&)` | ✅ L0 | Already in design |
| `match_indices(pat)` | — | ⚠️ L0 | Iterator, deferred with Split |
| `matches(pat)` / `rmatches(pat)` | — | ⚠️ L0 | Same |

### 7. Trim

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `trim() -> &str` | `trim() -> String` (ASCII-only) | ✅ L0 | Trims ASCII spaces (`0x09-0x0D`, `0x20`). Unicode whitespace trim needs L1 tables. |
| `trim_start()` | `trim_start()` | ✅ L0 | Same ASCII subset |
| `trim_end()` | `trim_end()` | ✅ L0 | Same |
| `trim_matches(pat)` | — | ⚠️ L0 | Generic predicate-based, deferred |

### 8. Replace

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `replace(from, to)` | `replace(const String& from, const String& to) -> String` | ✅ L0 | All occurrences, byte-level. Pure scanning + building a new String. |
| `replacen(from, to, n)` | `replacen(p, t, n)` | ✅ L0 | Same, capped |

### 9. Repeat

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `repeat(n) -> String` | `repeat(size_t n) -> String` | ✅ L0 | `result.reserve(len() * n)` + looped append |

### 10. Comparison

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `==` / `!=` | `operator==` / `operator!=` | ✅ L0 | Byte-wise |
| `<` / `>` | `operator<` | ✅ L0 | Lexicographic on bytes |
| `eq_ignore_ascii_case` | `eq_ignore_ascii_case` | ⚠️ L0 | Trivial (lowercase ASCII bytes in compare). Deferred. |

### 11. Iteration

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `chars() -> Chars` | `chars() -> Chars` | ✅ L0 | Code point iterator, already in design |
| `Chars::count()` | `Chars::count()` | ✅ L0 | Count remaining code points. O(remaining). Copies iterator position, does not modify String. |
| `char_indices() -> CharIndices` | `char_indices() -> CharIndices` | ⚠️ L0 | `(byte_offset, char32_t)` pairs. Trivial to add. Deferred. |
| `bytes() -> Bytes` | Use `as_bytes()` or range-for on `Span` | ✅ L0 | `Span<const uint8_t>` is iterable. No separate Bytes type needed. |
| `split_whitespace()` | — | ❌ L1 | Unicode whitespace table needed |

### 12. Case (all L1)

| Rust | xpp | Tag | Why |
|------|-----|-----|-----|
| `to_lowercase()` | — | ❌ L1 | Unicode case folding table |
| `to_uppercase()` | — | ❌ L1 | Same |
| `to_ascii_lowercase()` | — | ⚠️ L0 | Trivial byte transform. Deferred for now. |
| `to_ascii_uppercase()` | — | ⚠️ L0 | Same |

---

### Summary Count

| Category | Count |
|----------|-------|
| ✅ L0 — implement now | **30** methods |
| ⚠️ L0 — defer to L0.1 | **14** methods (iterators, lossy, retain, ascii_case) |
| ❌ L1 — `libxpp-ext/unicode` | **9** methods (case, normalize, grapheme, utf16) |

### L0 Implementation Order

```
Phase 1 (core, ~400 lines):
  new, from_utf8, from_utf8_unchecked, as_bytes, into_bytes,
  len, empty, char_len, chars, clear, push, push_str,
  substr, find, rfind, starts_with, ends_with, contains,
  operator==, operator<

Phase 2 (mutation, ~150 lines):
  pop, truncate, insert, insert_str, remove, split_off,
  capacity, reserve, shrink_to_fit, with_capacity

Phase 3 (utility, ~150 lines):
  replace, replacen, repeat,
  trim, trim_start, trim_end

Phase 4 (deferred L0.1, ~250 lines):
  from_utf8_lossy, retain, char_indices,
  split/splitn/rsplitn/lines iterators,
  eq_ignore_ascii_case, to_ascii_lowercase, to_ascii_uppercase
```

Total: ~950 lines of implementation + ~400 lines of tests across all phases.

## File Placement

```
libxpp/xpp/fmt.h       — XPP_HAS_FMTLIB detection (included by string.h)
libxpp/xpp/string.h    — String + Chars + Utf8Error + fmt formatter (if available)
libxpp/xpp/string_test.cpp
```

Dependencies: `std::vector<uint8_t>`, `xpp/result.h`, `xpp/option.h`, `xpp/span.h`.

## fmtlib Integration

`xpp/fmt.h` detects whether the host project has `{fmt}` (via `__has_include` or
a `XPP_FMT_CORE` user override) and exposes one macro:

```cpp
// xpp/fmt.h
#pragma once

#if defined(XPP_FMT_CORE) || __has_include(<fmt/core.h>)
  #define XPP_HAS_FMTLIB 1
  #include <fmt/core.h>
#endif
```

`xpp/string.h` conditionally provides the `fmt::formatter` specialisation
so that every user of `#include <xpp/string.h>` gets fmt support
automatically when `{fmt}` is available:

```cpp
// xpp/string.h (excerpt)
#include "fmt.h"

#ifdef XPP_HAS_FMTLIB
template <>
struct fmt::formatter<xpp::String> : fmt::formatter<fmt::string_view> {
    template <typename FormatContext>
    auto format(const xpp::String& s, FormatContext& ctx) const {
        auto sv = fmt::string_view(
            reinterpret_cast<const char*>(s.as_bytes().data()),
            s.len()
        );
        return fmt::formatter<fmt::string_view>::format(sv, ctx);
    }
};
#endif /* XPP_HAS_FMTLIB */
```

**Usage:**
```cpp
#include <xpp/string.h>

auto s = String::from_utf8("hello世界").unwrap();
fmt::print("{}\n", s);      // "hello世界"
fmt::print("{:>10}\n", s);  // " hello世界"
```

No separate `#include <xpp/fmt.h>` needed — `string.h` pulls it in.
When `{fmt}` is absent the header compiles harmlessly.

## C++11 Compatibility

The entire design uses only C++11 features:
- Range-for loop through `Chars` iterator + `begin()`/`end()` adapter.
- No `std::string_view` — views use `Span<const uint8_t>`.
- No `constexpr` — decode functions are plain functions.
- Inline namespace `_::` holds helper functions to keep the header light.

## Open Questions

1. **`std::hash` specialisation?** Yes — hash the underlying bytes
   (`std::vector<uint8_t>` doesn't have a built-in `std::hash`, so we provide
   one). Byte-identical strings are hash-identical.
2. **`operator<<` for logging?** Minimal — `os.write((const char*)s.as_bytes().data(), s.len())`.
3. **`from_utf8_lossy`?** Future — replace invalid sequences with U+FFFD.
   Needs only the validator already present. Candidate for L0.1.
