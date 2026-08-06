# xpp::String — UTF-8 String (L0 Design)

## Summary

A `String` type that guarantees valid UTF-8 at the type level. Internal storage
is `xpp::Vec<uint8_t>` — a byte buffer built on xpp's allocator protocol, with
no text semantics. The type system draws a bright line: `Vec<uint8_t>` = opaque
bytes, `String` = valid UTF-8. Purely L0 scope: encode/decode/validate/iterate —
no normalization, no case folding, no grapheme clusters.

String is built on **our own `Vec`**, not `std::vector`. This means:

- **Dual OOM API**: `push()` asserts on OOM; `try_push()` returns `Result`.
  `push_str()` / `try_push_str()` follow the same pattern.
- **`Option` returns**: `pop()` returns `Option<char32_t>` — matches Vec's
  `Option<T>` semantics.
- **`split_off()`**: Direct reuse of Vec's `split_off()`, bound to code point
  boundaries.
- **`shrink_to_fit()`**: Vec provides both `shrink_to_fit()` and
  `try_shrink_to_fit()`.
- **`retain()`**: Vec already has `retain(Func)` — String wraps it with a
  code-point-aware predicate or simply byte-level filtering.

## Motivation

`std::string` conflates "text" and "bytes". Rust separates them cleanly:
`Vec<u8>` is raw bytes, `String` is guaranteed UTF-8, `&[u8]` / `&str` are
borrowed views. xpp mirrors this — and with `Vec<uint8_t>` now in the standard
library, the type alignment with Rust is near-perfect:

1. **Type safety** — `const String&` means "valid UTF-8". Raw bytes use
   `Vec<uint8_t>`. No ambiguity.
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
| `Vec<u8>` | `Vec<uint8_t>` | Raw bytes via xpp allocator protocol |
| `String` | `String` | Guaranteed valid UTF-8, backed by `Vec<uint8_t>` |
| `&[u8]` | `Span<const uint8_t>` | Borrowed byte view |
| `&str` | (no equivalent yet) | Future: `Str` or `StringRef` |
| `String::from_utf8(Vec<u8>)` | `String::from_utf8(Vec<uint8_t>)` | Validates + takes ownership |
| `String::as_bytes() -> &[u8]` | `String::as_bytes() -> Span<const uint8_t>` | Read-only byte view |
| `String::into_bytes() -> Vec<u8>` | `String::into_bytes() -> Vec<uint8_t>` | Consume, recover raw bytes. O(1). |
| `FromUtf8Error` | `Utf8Error` | Owns the original `Vec<uint8_t>` |
| `str::chars()` | `String::chars()` | Code point iterator |

## API Reference

```cpp
namespace xpp {

/* ──────────────────────── String ──────────────────────── */

class String {
public:
    /* ── Construction ── */

    String() = default;

    /** Pre-allocate byte capacity. O(1). Asserts on OOM.
     *  Equivalent to Rust's String::with_capacity(). */
    explicit String(size_t capacity);

    /** Validate a byte buffer as UTF-8. Takes ownership via move.
     *
     *  Complexity: O(n) single-pass validation scan.
     *
     *  Usage:
     *    Vec<uint8_t> buf = read_from_network();
     *    auto s = String::from_utf8(std::move(buf));
     */
    static Result<String, Utf8Error> from_utf8(Vec<uint8_t> bytes);

    /** Convenience overload for C-string literals. Copies + validates.
     *  s must not be NULL; debug builds assert s != nullptr.
     *  Equivalent to from_utf8(s, strlen(s)).
     */
    static Result<String, Utf8Error> from_utf8(const char* s);

    /** Validate + construct from [data, data+len). */
    static Result<String, Utf8Error> from_utf8(const char* s, size_t len);

    /** Construct without validation. Caller guarantees the bytes are valid UTF-8.
     *  Behaviour is undefined if they aren't.
     */
    static String from_utf8_unchecked(Vec<uint8_t> bytes);

    /** Copy / move.
     *  Copy clones the underlying Vec<uint8_t> (deep copy of all bytes).
     *  Move transfers ownership, leaving source empty.
     */
    String(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(const String&) = default;
    String& operator=(String&&) noexcept = default;

    /* ── Bytes views ── */

    /** Borrowed read-only view of the raw UTF-8 bytes. O(1). */
    Span<const uint8_t> as_bytes() const noexcept;

    /** Consume the String and recover the underlying Vec<uint8_t>. O(1).
     *  The returned Vec owns the byte buffer; the String is left empty. */
    Vec<uint8_t> into_bytes() && noexcept;

    /** Consume the String and return a std::string. O(n) — Vec<uint8_t>
     *  and std::string have different layouts (std::string uses SSO),
     *  so a copy is required. The original Vec's heap buffer is released.
     *
     *  Use as_bytes() for zero-copy interop; use this when you need an
     *  owning std::string for an API that requires one. */
    std::string into_std_string() &&;

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

    /* ── Capacity ── */

    /** Allocated byte capacity. O(1). */
    size_t capacity() const noexcept;

    /** Reserve capacity for at least `additional` more bytes. Asserts on OOM. */
    void reserve(size_t additional);

    /** Explicit OOM variant. */
    Result<void, AllocError> try_reserve(size_t additional);

    /** Release excess capacity. Asserts on OOM (realloc failure). */
    void shrink_to_fit();

    /** Explicit OOM variant. */
    Result<void, AllocError> try_shrink_to_fit();

    /* ── Substring ── */

    /** Slice [offset, offset+count) in bytes.
     *  XPP_ASSERT that offset and offset+count fall on code point boundaries.
     *  If count == SIZE_MAX, take from offset to end.
     */
    String substr(size_t offset, size_t count = SIZE_MAX) const;

    /* ── Find ── */

    /** Byte position of the first occurrence of `pattern`, or None. O(n*m). */
    Option<size_t> find(const String& pattern) const;

    /** Convenience: find by C-string literal. */
    Option<size_t> find(const char* pattern) const;

    Option<size_t> rfind(const String& pattern) const;
    Option<size_t> rfind(const char* pattern) const;

    /** Returns true if find(pattern).is_some(). */
    bool contains(const String& pattern) const;
    bool contains(const char* pattern) const;

    bool starts_with(const String& prefix) const;
    bool starts_with(const char* prefix) const;
    bool ends_with(const String& suffix) const;
    bool ends_with(const char* suffix) const;

    /* ── Mutation ── */

    /** Append a code point (encoded as 1–4 UTF-8 bytes). O(1) amortised.
     *  XPP_ASSERT that cp is a valid Unicode scalar value:
     *    — Not a surrogate (U+D800–U+DFFF)
     *    — Not exceeding U+10FFFF
     */
    void push(char32_t cp);

    /** Append another String's bytes. O(n). Asserts on OOM. */
    void push_str(const String& other);

    /** Explicit OOM variant of push_str(). */
    Result<void, AllocError> try_push_str(const String& other);

    /** Append a C-string literal. Asserts on OOM. */
    void push_str(const char* s);

    /** Pop the last code point. O(1) — decodes and removes the last
     *  code point's byte span. Returns None if empty. */
    Option<char32_t> pop();

    /** Insert a code point at a byte position. O(n).
     *  XPP_ASSERT byte_pos is on a code point boundary. */
    void insert(size_t byte_pos, char32_t cp);

    /** Insert a String at a byte position. O(n).
     *  XPP_ASSERT byte_pos is on a code point boundary. */
    void insert_str(size_t byte_pos, const String& s);

    /** Remove and return the code point at byte_pos. O(n).
     *  XPP_ASSERT byte_pos is on a code point boundary. */
    char32_t remove(size_t byte_pos);

    /** Truncate to new_byte_len bytes. O(1).
     *  XPP_ASSERT new_byte_len is on a code point boundary. */
    void truncate(size_t new_byte_len);

    /** Remove all bytes. O(1) for the length; capacity is preserved. */
    void clear();

    /** Split at a code point boundary. Returns the tail String,
     *  leaving this with [0, byte_pos). O(tail length) — reuses
     *  Vec::split_off(), which copies the tail elements. */
    String split_off(size_t byte_pos);

    /** Replace all occurrences of `from` with `to`. Returns a new String. O(n). */
    String replace(const String& from, const String& to) const;

    /** Replace first `n` occurrences. Returns a new String. O(n). */
    String replacen(const String& from, const String& to, size_t n) const;

    /** Repeat this String `n` times. Returns a new String. O(n*m). */
    String repeat(size_t n) const;

    /* ── Trim (ASCII-only) ── */

    /** Remove leading and trailing ASCII whitespace (0x09-0x0D, 0x20). */
    String trim() const;
    String trim_start() const;
    String trim_end() const;

    /* ── Filter (code-point level) ── */

    /** Keep code points for which pred(cp) returns true. O(n).
     *  Internally uses Vec<uint8_t>::retain() on a byte-level predicate
     *  or rebuilds the buffer.
     */
    template <class Pred>
    void retain(Pred pred);

    /* ── Iteration ── */

    Chars chars() const noexcept;

    /* ── Comparison ── */

    /** Byte-wise equality. Does NOT do Unicode normalisation —
     *  "é" (precomposed U+00E9) and "e\u0301" (decomposed) will NOT compare
     *  equal. Callers needing normalisation-aware comparison must normalise
     *  first (libxpp-ext).
     */
    bool operator==(const String& other) const noexcept;
    bool operator!=(const String& other) const noexcept;

    /** Byte-wise lexicographic ordering on UTF-8. */
    bool operator<(const String& other) const noexcept;

    /* ── C-string interop ── */

    bool operator==(const char* other) const noexcept;
    bool operator!=(const char* other) const noexcept;

private:
    Vec<uint8_t> m_bytes;  // invariant: valid UTF-8
};

/* ──────────────────────── Chars ──────────────────────── */

/** Code-point iterator. Input iterator, single-pass (forward only).
 *
 *  Each dereference decodes the current multi-byte sequence into char32_t.
 *  Invalid bytes will never appear because the String invariant guarantees
 *  valid UTF-8. Increment advances past the current code point's byte span.
 *
 *  Usage:
 *    for (char32_t cp : s.chars()) { ... }             // range-for
 *    auto it = s.chars();
 *    auto end = it.end();
 *    while (it != end) { char32_t c = *it; ++it; }
 *
 *  Note: begin()/end() are instance methods for range-for support.
 *  Manual loops capture end() once — Chars::end() returns a sentinel
 *  with m_pos == m_end pointing to the real buffer end, which is NOT
 *  nullptr. A static factory returning (nullptr, nullptr) would never
 *  match a real iterator's end state (m_pos == m_end == data+len).
 */
class Chars {
public:
    char32_t operator*() const noexcept;

    /** Advance past the current code point. Must not be called when the
     *  iterator has already reached end(); debug asserts m_pos < m_end. */
    Chars& operator++() noexcept;

    bool operator==(const Chars& other) const noexcept;
    bool operator!=(const Chars& other) const noexcept;

    /** Count remaining code points from current position to end. O(remaining).
     *  Iterates in-place then restores position. After this call the iterator
     *  state is unchanged. */
    size_t count() noexcept;

    /* ── Range-for support ── */

    /** Copy of current position. Range-for calls this to start iteration. */
    Chars begin() const { return *this; }

    /** Sentinel at the end of the byte range. Range-for calls this
     *  to get the termination condition. */
    Chars end() const { return Chars(m_end, m_end); }

private:
    friend class String;
    Chars(const uint8_t* p, const uint8_t* end) noexcept;
    const uint8_t* m_pos;
    const uint8_t* m_end;
    char32_t m_current;
};

/* ──────────────────────── Utf8Error ──────────────────────── */

/** Returned when from_utf8() validation fails. Owns the original bytes so
 *  the caller can recover — log the error, fall back to lossy conversion,
 *  or abort.
 */
class Utf8Error {
public:
    /** Byte offset of the first error. */
    size_t error_pos() const noexcept;

    /** Recover the original bytes. Moves the Vec<uint8_t> out. */
    Vec<uint8_t> into_bytes() &&;

private:
    friend class String;
    Utf8Error(Vec<uint8_t> bytes, size_t pos);
    Vec<uint8_t> m_bytes;
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

### Delegating to Vec — what's free

Since internal storage is `Vec<uint8_t>`, many methods are one-liner delegations:

```cpp
size_t String::len()       const noexcept { return m_bytes.len(); }
size_t String::capacity()  const noexcept { return m_bytes.capacity(); }
bool   String::empty()     const noexcept { return m_bytes.empty(); }
void   String::clear()                   { m_bytes.clear(); }
void   String::reserve(size_t n)         { m_bytes.reserve(n); }
void   String::shrink_to_fit()           { m_bytes.shrink_to_fit(); }
String String::split_off(size_t pos)     {
    XPP_ASSERT(is_codepoint_boundary(m_bytes.data(), pos), "not a boundary");
    String tail; tail.m_bytes = m_bytes.split_off(pos);
    return tail;
}
```

### `into_std_string()` implementation

```cpp
std::string String::into_std_string() && {
    return std::string(
        reinterpret_cast<const char*>(m_bytes.data()),
        m_bytes.len()
    );
    // m_bytes is destroyed after this (moved-from String)
}
```

The `Vec<uint8_t>` is released normally by the destructor. The `std::string`
constructor copies the bytes — O(n). This is unavoidable because `Vec<uint8_t>`
and `std::string` have different memory layouts (SSO).

### `pop()` — decode last code point

```cpp
Option<char32_t> String::pop() {
    if (m_bytes.len() == 0) return none;

    // Walk backwards to find the start of the last code point.
    // Continuation bytes have the form 10xxxxxx.
    size_t pos = m_bytes.len() - 1;
    while (pos > 0 && (m_bytes[pos] & 0xC0) == 0x80) {
        pos--;
    }

    size_t consumed;
    char32_t cp = decode_one(m_bytes.data() + pos, &consumed);
    m_bytes.truncate(pos);
    return Option<char32_t>(cp);
}
```

## Size and Layout

```
sizeof(String)    == sizeof(Vec<uint8_t>) == 24
                     (CompressedPair<RawStorage, GlobalAllocator>: 3 pointers)
sizeof(Chars)     == 24 (two pointers + cached char32_t)
sizeof(Utf8Error) == 32 (Vec<uint8_t> 24B + error_pos 8B)
```

With the default `GlobalAllocator`, `String` is exactly 24 bytes on 64-bit —
zero overhead beyond the `Vec` itself. A custom stateful allocator grows
`sizeof(String)` by `sizeof(Allocator)`.

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
| `s.chars()` on empty | `it == it.end()` immediately (m_pos == m_end == nullptr) |
| `s.char_len()` on empty | 0 |
| `s.pop()` on empty | `Option<char32_t>` = None |
| `s.pop()` on `"a"` | Returns `Some('a')`, string becomes empty |
| `"é" == "e\u0301"` | false (byte-wise comparison, NFC not applied) |

## Rust `String` Method Coverage

This section maps every method on Rust's `std::string::String` to xpp's
equivalent (or explains why it's deferred). Legend:

| Tag | Meaning |
|-----|---------|
| ✅ L0 | Implemented in xpp `String` |
| ⚠️ L0 | Implementable but deferred (edge cases, iterator complexity) |
| ❌ L1 | Needs Unicode tables — belongs in `libxpp-ext/unicode` |

---

### 1. Construction

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `String::new()` | `String()` default | ✅ L0 | Empty, zero allocation |
| `String::with_capacity(n)` | `String(size_t n)` | ✅ L0 | `Vec<uint8_t>(n)` constructor — allocates capacity |
| `String::from_utf8(Vec<u8>)` | `String::from_utf8(Vec<uint8_t>)` | ✅ L0 | Validate + take ownership |
| `String::from_utf8_lossy(&[u8])` | `String::from_utf8_lossy(Span<const uint8_t>)` | ⚠️ L0 | Replace bad bytes with U+FFFD. Deferred. |
| `String::from_utf16(&[u16])` | — | ❌ L1 | Needs UTF-16 codec |
| `From<&str>` / `to_string()` | `from_utf8(const char*)` + copy ctor | ✅ L0 | Already covered |

### 2. Conversion / Views

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `as_bytes() -> &[u8]` | `as_bytes() -> Span<const uint8_t>` | ✅ L0 | O(1), no copy |
| `as_str() -> &str` | (no Str type yet) | ⚠️ L0 | Requires `Str` borrowed type |
| `into_bytes() -> Vec<u8>` | `into_bytes() -> Vec<uint8_t>` | ✅ L0 | Consuming, O(1) move |
| — | `into_std_string() -> std::string` | ✅ L0 | Consuming, O(n) copy. std interop. |
| `into_boxed_str()` | — | ❌ | No `Box<str>` equivalent |
| `from_raw_parts` / `into_raw_parts` | — | ❌ | Vec doesn't expose raw parts (deliberately) |

### 3. Capacity

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `capacity()` | `capacity()` | ✅ L0 | Delegates to `Vec::capacity()` |
| `reserve(n)` | `reserve(size_t n)` | ✅ L0 | Delegates to `Vec::reserve()` (asserts on OOM) |
| `try_reserve(n)` | `try_reserve(size_t n)` | ✅ L0 | Delegates to `Vec::try_reserve()` → `Result` |
| `reserve_exact(n)` | `reserve(n)` | ✅ L0 | Vec's `reserve()` already allocates exact |
| `shrink_to_fit()` | `shrink_to_fit()` | ✅ L0 | Delegates to `Vec::shrink_to_fit()` |
| `shrink_to(n)` | — | ❌ | Vec doesn't support shrink-to-arbitrary |

### 4. Push / Pop / Insert / Remove

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `push(ch: char)` | `push(char32_t cp)` | ✅ L0 | Encodes 1-4 bytes via `encode_one()` |
| `push_str(s: &str)` | `push_str(const String&)` | ✅ L0 | Byte append, asserts on OOM |
| — | `try_push_str(const String&)` | ✅ L0 | Explicit OOM handling |
| `push_str(s)` | `push_str(const char*)` | ✅ L0 | C-string convenience |
| `pop() -> Option<char>` | `pop() -> Option<char32_t>` | ✅ L0 | Decode last CP, truncate bytes |
| `insert(idx, ch)` | `insert(size_t byte_pos, char32_t cp)` | ✅ L0 | Assert on CP boundary |
| `insert_str(idx, s)` | `insert_str(size_t byte_pos, const String&)` | ✅ L0 | Same boundary check |
| `remove(idx) -> char` | `remove(size_t byte_pos) -> char32_t` | ✅ L0 | Decode CP at pos, erase byte span |
| `retain(pred)` | `retain(Pred)` | ✅ L0 | Code-point-level filter |
| `truncate(new_len)` | `truncate(size_t new_byte_len)` | ✅ L0 | CP boundary assert |
| `clear()` | `clear()` | ✅ L0 | Delegates to `Vec::clear()` |

> **Note:** `push()`, `insert()`, and `push_str()` assert on OOM. Use
> `try_push_str()` for explicit error handling. `push(char32_t)` is bounded
> to 4 bytes — the OOM surface is minimal, so only `push_str` gets a `try_*`
> variant for now.

### 5. Substring / Split

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `[range]` slicing | `substr(offset, count)` | ✅ L0 | CP boundary assert |
| `split_off(at) -> String` | `split_off(size_t byte_pos) -> String` | ✅ L0 | Reuses `Vec::split_off()` |
| `split(pat)` / `splitn()` | `Split` iterator | ⚠️ L0 | Iterator type adds complexity. Deferred. |
| `rsplit(pat)` / `rsplitn()` | same | ⚠️ L0 | Same |
| `lines()` | `Lines` iterator | ⚠️ L0 | Trivial (\n split). Deferred. |
| `drain(range)` | — | ❌ | Requires mutable view into String |

### 6. Search / Contains

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `find(pat) -> Option<usize>` | `find(const String&) -> Option<size_t>` | ✅ L0 | Byte-level `memmem` |
| `rfind(pat)` | `rfind(const String&)` | ✅ L0 | Reverse scan |
| `contains(pat) -> bool` | `contains(const String&) -> bool` | ✅ L0 | `find(p).is_some()` |
| `starts_with(pat)` | `starts_with(const String&)` | ✅ L0 | Prefix match |
| `ends_with(pat)` | `ends_with(const String&)` | ✅ L0 | Suffix match |
| `match_indices(pat)` | — | ⚠️ L0 | Iterator, deferred |
| `matches(pat)` / `rmatches(pat)` | — | ⚠️ L0 | Same |

### 7. Trim (ASCII-only)

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `trim() -> &str` | `trim() -> String` | ✅ L0 | ASCII whitespace (0x09-0x0D, 0x20) |
| `trim_start()` | `trim_start()` | ✅ L0 | Same |
| `trim_end()` | `trim_end()` | ✅ L0 | Same |
| `trim_matches(pat)` | — | ⚠️ L0 | Generic predicate-based, deferred |

### 8. Replace

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `replace(from, to)` | `replace(from, to) -> String` | ✅ L0 | All occurrences, returns new String |
| `replacen(from, to, n)` | `replacen(from, to, n)` | ✅ L0 | Capped |

### 9. Repeat

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `repeat(n) -> String` | `repeat(size_t n) -> String` | ✅ L0 | `reserve(len() * n)` + looped append |

### 10. Comparison

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `==` / `!=` | `operator==` / `operator!=` | ✅ L0 | Byte-wise |
| `<` / `>` | `operator<` | ✅ L0 | Lexicographic on bytes |
| `eq_ignore_ascii_case` | — | ⚠️ L0 | Trivial, deferred |

### 11. Iteration

| Rust | xpp equivalent | Tag | Notes |
|------|---------------|-----|-------|
| `chars() -> Chars` | `chars() -> Chars` | ✅ L0 | Code point iterator |
| `Chars::count()` | `Chars::count()` | ✅ L0 | Count remaining CPs |
| `char_indices() -> CharIndices` | — | ⚠️ L0 | `(byte_offset, char32_t)` pairs. Deferred. |
| `bytes() -> Bytes` | `as_bytes()` or range-for on `Span` | ✅ L0 | Span is iterable |
| `split_whitespace()` | — | ❌ L1 | Unicode whitespace table needed |

### 12. Case (all L1)

| Rust | xpp | Tag | Why |
|------|-----|-----|-----|
| `to_lowercase()` | — | ❌ L1 | Unicode case folding table |
| `to_uppercase()` | — | ❌ L1 | Same |
| `to_ascii_lowercase()` | — | ⚠️ L0 | Trivial byte transform. Deferred. |
| `to_ascii_uppercase()` | — | ⚠️ L0 | Same |

---

### Summary Count

| Category | Count |
|----------|-------|
| ✅ L0 — implement now | **35** methods |
| ⚠️ L0 — defer to L0.1 | **12** methods (iterators, lossy, ascii_case) |
| ❌ L1 — `libxpp-ext/unicode` | **9** methods (case, normalize, grapheme, utf16) |

### L0 Implementation Order

```
Phase 1 (core, ~350 lines):
  new, from_utf8, from_utf8_unchecked, as_bytes, into_bytes, into_std_string,
  len, empty, char_len, capacity, reserve, try_reserve,
  shrink_to_fit, try_shrink_to_fit, chars, clear, push, push_str,
  substr, find, rfind, contains, starts_with, ends_with,
  operator==, operator!=, operator<

Phase 2 (mutation, ~200 lines):
  pop, truncate, insert, insert_str, remove, split_off,
  replace, replacen, repeat, retain

Phase 3 (utility, ~120 lines):
  trim, trim_start, trim_end, try_push_str, operator==(const char*)

Phase 4 (deferred L0.1, ~200 lines):
  from_utf8_lossy, char_indices,
  split/splitn/rsplitn/lines iterators,
  eq_ignore_ascii_case, to_ascii_lowercase, to_ascii_uppercase
```

Total: ~870 lines of implementation + ~500 lines of tests across all phases.

## File Placement

```
libxpp/xpp/fmt.h       — XPP_HAS_FMTLIB detection (included by string.h)
libxpp/xpp/string.h    — String + Chars + Utf8Error + fmt formatter
libxpp/xpp/string_test.cpp
```

Dependencies: `xpp/vec.h`, `xpp/result.h`, `xpp/option.h`, `xpp/span.h`.

No dependency on `<vector>` or `<string>` for the core type — only
`into_std_string()` brings in `<string>`.

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

`xpp/string.h` conditionally provides the `fmt::formatter` specialisation:

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

## C++11 Compatibility

The entire design uses only C++11 features:
- Range-for loop through `Chars` iterator.
- No `std::string_view` — views use `Span<const uint8_t>`.
- No `constexpr` — decode functions are plain functions.
- `Vec<uint8_t>` provides all mutating operations with both assert-on-OOM and
  `try_*` variants, beyond what C++11 `std::vector` offers.

## Key Changes from std::vector-based Design

| Aspect | Old (std::vector) | New (Vec<uint8_t>) |
|--------|-------------------|---------------------|
| Internal storage | `std::vector<uint8_t>` | `Vec<uint8_t>` |
| `from_utf8()` | Takes `std::vector<uint8_t>` | Takes `Vec<uint8_t>` |
| `into_bytes()` | Returns `std::vector<uint8_t>` | Returns `Vec<uint8_t>` |
| Utf8Error storage | `std::vector<uint8_t>` | `Vec<uint8_t>` |
| Capacity API | `reserve()` only (no try) | `reserve()` + `try_reserve()` |
| OOM handling | Throw or abort | Dual API: assert / Result |
| `split_off()` | Manual reimplementation | Delegates to `Vec::split_off()` |
| `clear()` | `vector::clear()` | `Vec::clear()` (preserves capacity) |
| `retain()` | `erase(remove_if(...))` | `Vec::retain(Pred)` |
| Dependencies | `<vector>` | `xpp/vec.h` |
| Allocator | `std::allocator` | xpp allocator protocol (pluggable) |

## Open Questions

1. **`std::hash` specialisation?** Yes — hash the underlying bytes via
   `Vec<uint8_t>`'s `as_span()`.
2. **`operator<<` for logging?** Minimal — `os.write((const char*)s.as_bytes().data(), s.len())`.
3. **`from_utf8_lossy`?** Future — replace invalid sequences with U+FFFD.
   Candidate for L0.1.
4. **Allocator template parameter?** Not in L0. `String` always uses
   `GlobalAllocator`. Custom allocators can be added as a template parameter
   (`String<Alloc>`) in a follow-up if needed, matching `Vec<T, Alloc>`.
