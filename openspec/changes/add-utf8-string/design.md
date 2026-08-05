# xpp::String — UTF-8 String (L0 Design)

## Summary

A `String` type that guarantees valid UTF-8 at the type level. Built on top of
`std::string`, adding encoding safety + code-point-aware operations with zero
runtime data tables. Purely L0 scope: encode/decode/validate/iterate — no
normalization, no case folding, no grapheme clusters.

## Motivation

`std::string` is a byte sequence with no encoding contract. `String` bridges
the gap:

1. **Type safety** — `const String&` in a function signature means "this is valid
   UTF-8, you don't need to check".
1. **Correct substring** — byte-offset-based but guaranteed to land on code point
   boundaries, never slicing a multi-byte character in half.
1. **Code point iteration** — `chars()` yields `char32_t`, not raw bytes.
1. **Zero dependencies** — UTF-8 decode is a tiny state machine with no lookup
   tables. Entire implementation fits in ~400 lines.

Everything beyond L0 (normalization, case folding, grapheme boundaries) belongs
in a future `libxpp-ext/unicode` extension library.

## API Reference

```cpp
namespace xpp {

/* ──────────────────────── String ──────────────────────── */

class String {
public:
    /* ── Construction ── */

    String() = default;

    /** Validate the byte sequence. On success, ownership transfers via move.
     *  On failure, the original bytes are returned in Utf8Error.
     *
     *  Complexity: O(n) single-pass validation scan.
     */
    static Result<String, Utf8Error> from_utf8(std::string bytes);

    /** Construct without validation. Caller guarantees the bytes are valid UTF-8.
     *  Behaviour is undefined if they aren't.
     */
    static String from_utf8_unchecked(std::string bytes);

    /** Copy / move. */
    String(const String&) = default;
    String(String&&) = default;
    String& operator=(const String&) = default;
    String& operator=(String&&) = default;

    /* ── Borrowing ── */

    /** The raw UTF-8 bytes. Always valid. */
    const std::string& bytes() const noexcept;

    /* ── Length ── */

    /** Byte count. O(1). */
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
     *  If count == SIZE_MAX, take until end.
     */
    String substr(size_t offset, size_t count = SIZE_MAX) const;

    /* ── Find ── */

    /** Byte position of the first occurrence of `pattern`, or None.
     *  O(n * m) — memmem under the hood.
     */
    Option<size_t> find(const char* pattern) const;

    /** Byte position of the last occurrence, or None. */
    Option<size_t> rfind(const char* pattern) const;

    bool starts_with(const char* prefix) const;
    bool ends_with(const char* suffix) const;

    /* ── Mutation ── */

    /** Append a code point (encoded as 1–4 UTF-8 bytes). O(1) amortised. */
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
    std::string m_bytes;  // invariant: valid UTF-8
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
    Chars& operator++() noexcept;

    bool operator==(const Chars& other) const noexcept;
    bool operator!=(const Chars& other) const noexcept;

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
    std::string into_bytes() &&;

private:
    friend class String;
    Utf8Error(std::string bytes, size_t pos);
    std::string m_bytes;
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
        if ((p[i] & 0xE0) == 0xC0) { i += 1; if (i >= len) return i - 1; }
        else if ((p[i] & 0xF0) == 0xE0) { i += 2; if (i >= len) return i - 2; }
        else if ((p[i] & 0xF8) == 0xF0) { i += 3; if (i >= len) return i - 3; }
        else return i;  // invalid leading byte
    }
    return SIZE_MAX;
}
```

Additional checks for overlong encodings and surrogate halves (~10 lines) are
added in the full implementation. Total decoder ~40 lines.

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
sizeof(String)   == sizeof(std::string) == 24 (pointer + size + capacity)
sizeof(Chars)    == 24 (two pointers + cached char32_t)
sizeof(Utf8Error)== 32 (string 24B + error_pos 8B)
```

No hidden allocations beyond `std::string`'s heap buffer.

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

## What's NOT in L0

- `operator[](size_t)` — no random code point access
- `to_lowercase()` / `to_uppercase()` — needs Unicode case folding table
- NFC / NFD normalization — needs decomposition tables
- `find(char32_t)` — use `find("\xE4\xBD\xA0")` (UTF-8 literal) instead
- `replace(char32_t, char32_t)` — compose from `find` + `substr` + `push`
- Grapheme cluster iteration — needs UCD grapheme break property table

All of the above are natural candidates for `libxpp-ext/unicode`.

## File Placement

```
libxpp/xpp/string.h    — String + Chars + Utf8Error + internal helpers
libxpp/xpp/string_test.cpp
```

No new dependencies beyond `std::string`, `xpp/result.h`, `xpp/option.h`.

## C++11 Compatibility

The entire design uses only C++11 features:
- Range-for loop requires `begin()` / `end()`, which `Chars` provides via
  the `String::chars()` factory + `operator!=` overload.
- No `std::string_view` — `find` / `starts_with` accept `const char*`.
- No `constexpr` — decode functions are plain functions.
- Inline namespace `_::` holds helper functions to keep the header light.

## Open Questions

1. **`std::hash` specialisation?** Yes — forward to `std::hash<std::string>` on
   `bytes()`. Byte-identical strings are hash-identical. NFC mismatch issue
   is acknowledged but belongs to the caller.
2. **`operator<<` for logging?** Yes — `os << s.bytes()` is enough. An explicit
   `operator<<` that prints the raw bytes avoids encoding confusion.
3. **`from_utf8_lossy(async_fd::read()` integration?** Natural: `auto buf = co_await io::read_all(reader); auto s = String::from_utf8(std::move(buf));`
