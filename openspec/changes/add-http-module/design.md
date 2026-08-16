## Context

libxpp today has `Promise<T>`, `Option<T>`, `Result<T,E>`, `Vec<T>`, `String`, `Shared<T>`, `sync::mpsc`, `io::AsyncReader`, `io::read_all`, `io::copy`, and `net::{TcpStream,TcpListener}` — Rust-shaped primitives. There is no HTTP module. An earlier attempt on `add-http-module` (abandoned) tried to wrap `libx/x/http/` (which itself wraps libcurl) but mixed push and pull body models, used synchronous `TryRead` abstractions that bypassed `AsyncReader`, and wrote blocking `while` loops in `Response::text()` that freeze the single-threaded EventLoop.

This change adds a new `xpp::http` module that wraps `libx/x/http/client.h` (C API with `on_response` / `on_data` / `on_done` callbacks). The push-based C API is bridged to the pull-based xpp io model via an `xpp::sync::mpsc` channel, exactly the pattern hyper 0.12 adopted when it moved from callback-based to `Stream`-based bodies. Constraints that shape the design:

- **C++11 only** (libxpp is `cxx_std_11`). No `if constexpr`, no fold expressions, no `std::variant`/`std::optional`/`std::string_view` in core headers, no CTAD, no structured bindings.
- **Header-only**. No .cpp files in `libxpp/xpp/`.
- **No RTTI, no exceptions**. Errors flow through `Result<T, Error>`.
- **Rust-inspired naming**: snake_case types/functions, `k_` constants, `_::` for internals, `XPP_` macro prefix. HTTP API naming aligned with `hyper` + `reqwest` so Rust users find the same shapes (`Client`, `Request`, `Response`, `Body`, `Method`, `StatusCode`, `HeaderMap`).
- **No new dependencies**. Reuse `xpp::Shared/Option/Result/Vec/String`, `xpp::sync::mpsc`, `xpp::io::AsyncReader`, `xpp::promise::Promise`, and `libx/x/http/`.

The module is delivered as one coherent slice: `Bytes` (basic type) + 9 http headers. Server side, HTTP/2, TLS, WebSocket, and JSON are explicitly deferred.

## Goals / Non-Goals

**Goals:**

- A `Body` type that satisfies the `AsyncReader` concept, so `io::read_all(resp.body())` and `io::copy(resp.body(), dest)` work out of the box.
- Zero synchronous blocking in the async path — all body consumption goes through `Promise` chains.
- Natural backpressure: channel full → C callback returns `-1` → libx pauses pushing.
- API naming aligned with `hyper` + `reqwest` (one less learning curve for Rust users).
- Top-level `http::get(url)` / `post(url, body)` convenience functions matching `reqwest::get`.
- A `Bytes` type with O(1) copy and O(1) slice, mirroring `bytes::Bytes` — essential for `ChannelReader` to split a chunk without copying.
- `http::get` URL overloads for `String`, `const char*`, and (when available) `std::string_view`.
- C++11 compatibility throughout.

**Non-Goals:**

- Server side (`Server`, `Router`, `Handler`). Follow-up change.
- HTTP/2, TLS, WebSocket. libx has C implementations; xpp wrappers are follow-up changes.
- `Response::json<T>()` — depends on the `xpp-serde` capability.
- Connection pooling, multiplexing, or HTTP/2 stream concurrency tuning.
- Replacing `libx/x/http/` or its API. The C HTTP module stays as-is; the C++ layer is a consumer.
- Zero-copy receive (buffer stays in libx-owned memory). Default is one copy into `Bytes`; a future `Borrowed<Bytes>` wrapper can opt in if profiling demands.
- Performance parity with hyper. Correctness and ergonomics first.

## Decisions

### Decision 1: Bridge push→pull with `xpp::sync::mpsc::channel<Bytes>` (bounded 64)

```cpp
auto [tx, rx] = sync::mpsc::channel<Bytes>(64);
Body body = Body::from_channel(std::move(rx));
// C callback:
//   on_data(data, len) → tx.try_send(Bytes::copy(data, len))
//                        full → return -1 (backpressure)
//   on_done()           → tx.close()  → next rx.recv() returns None → Body::read returns 0
```

**Why:** Same pattern hyper 0.12 adopted (`Body::channel()` returns `(Sender, Body)`). mpsc is already in xpp, integrated with the waker/EventLoop. The bounded buffer gives natural backpressure: when the consumer is slow, `try_send` returns `Full`, the C callback returns `-1`, libx pauses.

**Alternative considered:** unbounded queue (Go-style). Rejected because under pressure the queue grows without bound, OOM risk on small devices.

**Alternative considered:** direct `Future<Bytes>` per chunk, no channel. Rejected because the consumer can't easily express "give me the next chunk whenever" without a stream-like type, and a stream type is just a channel with one slot.

**Capacity 64:** at ~4KB per chunk, 256KB buffer — large enough to absorb a typical HTTP response without stalling, small enough not to OOM embedded targets. Tunable later via `ClientBuilder::body_buffer_chunks(n)`.

### Decision 2: `Body` is a tagged enum `{ Empty, Once, Channel }`, not a virtual class

```cpp
class Body {
  enum class Kind { Empty, Once, Channel };
  Kind m_kind = Kind::Empty;
  Bytes m_once;                      // for Once
  sync::mpsc::Receiver<Bytes> m_rx;   // for Channel
  Bytes m_pending;                   // Channel: leftover slice from previous chunk
};
```

**Why:** Avoids heap allocation + vtable per Body. Matches hyper's `enum Kind { Once, Chan, Wrap }`. `Empty` is the default-constructed state (no allocation at all).

**Alternative considered:** `std::function<Promise<ssize_t>(void*, size_t)>` (the old branch's approach). Rejected because it (a) requires heap allocation per Body, (b) hides what the Body actually is, (c) tempts users to write blocking lambdas.

### Decision 3: `Body::read()` returns `Promise<ssize_t>`, returns 0 on EOF

```cpp
Promise<ssize_t> Body::read(void* buf, size_t len);
// returns:  > 0  bytes written to buf
//            0    EOF (Empty exhausted, Once exhausted, or channel closed)
//           < 0   error (future; for now errors surface via Body::bytes() returning Result)
```

**Why:** Matches `AsyncReader::read` signature, so `Body` composes with `io::read_all`, `io::copy`, `BufReader`. Zero is EOF — the convention shared by POSIX `read(2)` and Rust's `AsyncRead`.

### Decision 4: Introduce `xpp::Bytes` as a new basic type

```cpp
class Bytes {
  struct Impl { Vec<uint8_t> buf; };
  Shared<Impl> m_impl;      // Rc or Arc (compile-time via -DXPP_MT)
  size_t       m_offset = 0;
  size_t       m_len    = 0;
};
// sizeof(Bytes) = 24 on 64-bit
```

**Why:**
- `Body::read(buf, len)` with `len < chunk.size()` needs to remember the leftover. With `Vec<uint8_t>` that's a second allocation or an unsafe pointer; with `Bytes` it's `slice_from(n)` — O(1), refcount-shared, no copy.
- Multiple consumers of the same response body (e.g., logging + processing) can each hold a `Bytes` without copying.
- Aligns with `bytes::Bytes` in Rust, which hyper and reqwest use for the same reason.

**Alternative considered:** just use `Vec<uint8_t>`, copy on slice. Rejected because `ChannelReader` slicing would copy on every `read()` call — one chunk per `recv()`, sliced N times.

**Lives at `xpp::Bytes`** (top-level), not `xpp::bytes::Bytes`. Reasons:
- It's a basic type used as frequently as `String` and `Vec`.
- `String` lives at `xpp::String`, not `xpp::string::String` — consistency.
- File path is `libxpp/xpp/bytes.h` (single header, not a directory), to avoid confusion with the old branch's `libxpp/xpp/bytes/` module.

### Decision 5: `HeaderMap` backed by `Vec<String>` parallel arrays

```cpp
class HeaderMap {
  Vec<String> m_keys;    // stored lowercased for case-insensitive lookup
  Vec<String> m_values;
};
```

**Why:** HTTP headers per message are few (5-20). Linear scan of 20 strings beats a red-black tree (`std::multimap`) due to cache locality. Parallel arrays (SoA) beat `Vec<std::pair<String,String>>` (AoS) because the keys array is contiguous and the hot path (lookup by key) only touches keys.

**Alternative considered:** `std::multimap` (old branch). Rejected — per-node heap allocation, cache-unfriendly, allocates on every `get` to lowercase the key.

**Alternative considered:** small-vector optimization (inline first 8). Deferred — the overhead of `Vec<String>` heap allocation for 5-20 entries is negligible vs. the network RTT.

### Decision 6: `Client::send` (not `request`) and top-level `http::get` (not `Client::get` only)

```cpp
namespace xpp::http {
  Promise<Result<Response>> get(String url);   // creates a default Client internally
}

class Client {
  Promise<Result<Response>> send(Request req);  // generic entry
  Promise<Result<Response>> get(String url);    // convenience, calls send
};
```

**Why:** `send` is more verb-like than `request` (which is also a noun), and aligns with `reqwest::Client::send`. Top-level `http::get` aligns with `reqwest::get` — one-liner for the common case, no `Client` setup required.

### Decision 7: `Response::body()` borrows, `into_body()` moves

```cpp
Body&  body();          // borrow
Body   into_body();     // move out
```

**Why:** Direct mirror of hyper's API. C++ has no borrow checker, but the borrow/move split makes ownership explicit: `body()` for inspecting headers + streaming reads without taking ownership; `into_body()` when the caller will consume the body fully (e.g., `resp.bytes()`).

### Decision 8: `RequestBuilder::body()` returns `Result<Request>`, not `Request`

```cpp
Result<Request> body(Body body);
Result<Request> body(Bytes bytes);
```

**Why:** Aligns with hyper. Even though current `Body::from` overloads can't fail, future body sources (file-backed, stream-wrapped) may fail to construct. Returning `Result` now keeps the API stable for future additions.

### Decision 9: URL parameter overloads — `String`, `const char*`, `std::string_view`

```cpp
Promise<Result<Response>> get(String url);
Promise<Result<Response>> get(const char* url);
Promise<Result<Response>> get(std::string_view url);  // guarded by __cpp_lib_string_view
```

**Why:** String literals (`"https://..."`) hit `const char*`; `xpp::String` moves efficiently; `std::string_view` avoids copies when the caller has a `std::string`. The `std::string_view` overload is guarded so C++11 builds still compile.

### Decision 10: `Body::bytes()` returns `Result<Bytes>`, `text()` returns `Result<String>`

```cpp
Promise<Result<Bytes>>  bytes();   // I/O may fail (connection reset)
Promise<Result<String>> text();    // bytes() + UTF-8 validation
```

**Why:** Body consumption involves I/O (the channel may close early with a connection error). UTF-8 validation can fail for `text()`. Aligns with reqwest's `bytes()` / `text()` returning `Result`.

### Decision 11: `Bytes::to_string()` returns `Result<String>`, `to_string_lossy()` returns `String`

**Why:** Mirrors Rust's `String::from_utf8` (returns `Result`) and `String::from_utf8_lossy` (returns `Cow<str>`). Consistent with xpp's existing `String::from_utf8`.

### Decision 12: `Shared<Impl>` for `Bytes` — compile-time Rc/Arc

**Why:** xpp's `Shared<T>` is `Rc<T>` by default and `Arc<T>` when `-DXPP_MT` is defined. HTTP body chunks live on the EventLoop thread in the common case (Rc is enough), but multi-EventLoop topologies need Arc. Reusing `Shared` keeps `Bytes` consistent with other xpp types and defers the threading decision to build time.

## Open Questions

### Q1: libcurl global init cost

`http::get(url)` creates a default `Client` each call. If `xHttpClientCreate` triggers `curl_global_init` (process-wide, not thread-safe before init), this cost is paid per call. **To verify by profiling.** If measurable, add a thread-local default `Client` reused by `http::get`.

### Q2: Channel capacity tuning

64 chunks × ~4KB ≈ 256KB. The right number depends on workload (small JSON responses vs. large file downloads). **Initial value 64, plan to add `ClientBuilder::body_buffer_chunks(n)` if profiling shows backpressure stalls.**

### Q3: Server side

Out of scope for this change. The push→pull bridge pattern will apply symmetrically: `on_request` spawns a handler coroutine (doesn't `.await()` in the C callback); `on_data` pushes into the request body channel; handler returns `Promise<Result<Response>>`, completion callback writes status/headers back to `xHttpCtx` and the response body is pulled via `on_read`.

### Q4: H2 / TLS / WebSocket

libx has `proto_h2.c`, `tls.h`, `ws*.c` — none wrapped in xpp yet. Follow-up changes.

### Q5: JSON

`Response::json<T>()` depends on `xpp-serde` (in flight on another branch). Will be added as a separate change once serde lands.

## File Layout

```
libxpp/xpp/
├── bytes.h              ← NEW: xpp::Bytes basic type
└── http/
    ├── method.h         ← NEW: enum class Method
    ├── status.h         ← NEW: enum class StatusCode : uint16_t
    ├── header.h         ← NEW: HeaderMap (Vec<String> parallel arrays)
    ├── body.h           ← NEW: Body { Empty, Once, Channel }
    ├── request.h        ← NEW: Request + RequestBuilder
    ├── response.h       ← NEW: Response + ResponseBuilder
    ├── client.h         ← NEW: Client + ClientBuilder
    ├── error.h          ← NEW: Error
    └── http.h           ← NEW: top-level http::get/post/...
```

10 new headers total. The old branch had 13 files (including a redundant `bytes/` module and `test_server.h`) — both removed.

## API Surface (Summary)

See `specs/xpp-http/spec.md` for the formal requirements. The full C++ API is:

- `xpp::Bytes` — refcounted byte block (`empty`, `from`, `copy`, `slice`, `slice_from`, `to_vec`, `to_string`, `to_string_lossy`, `as_span`, `data`, `size`, `begin`, `end`)
- `xpp::http::Method` — enum
- `xpp::http::StatusCode` — enum : uint16_t + helpers
- `xpp::http::HeaderMap` — `insert`, `get`, `contains`, `get_all`, `erase`, iterators
- `xpp::http::Body` — `empty`, `from`, `from_channel`, `read`, `bytes`, `text`, `is_empty`, `is_channel`
- `xpp::http::Request` + `RequestBuilder` — `method`, `url`, `header`, `bearer_auth`, `basic_auth`, `body` (overloads), `get`/`post`/... convenience terminators
- `xpp::http::Response` + `ResponseBuilder` — `status`, `headers`, `body`, `into_body`, `bytes`, `text`, `url`; builder `status`, `header`, `body`; static `ok`/`created`/`no_content`/`bad_request`/`not_found`/`internal_server_error`
- `xpp::http::Client` + `ClientBuilder` — `send`, `get`/`post`/...; builder `timeout`, `connect_timeout`, `read_timeout`, `header`, `user_agent`, `redirect`, `max_redirects`, `proxy`, `no_proxy`, `tls`, `danger_accept_invalid_certs`, `http1_only`, `http2_prior_knowledge`, `bearer_auth`, `basic_auth`, `build`
- `xpp::http::Error` — `kind`, `message`, `status`, `is_connect`, `is_timeout`, `is_redirect`, `is_status_error`, `to_string`
- `xpp::http::RedirectPolicy` — `FollowUpTo10`, `FollowAll`, `None`
- Top-level `xpp::http::get/post/put/delete_/patch/head` with `String` / `const char*` / `std::string_view` overloads

## Usage Examples

### One-shot GET

```cpp
auto resp = co_await xpp::http::get("https://example.com").unwrap();
auto text = co_await resp.text().unwrap();
```

### Customized POST

```cpp
auto client = xpp::http::Client::builder()
    .timeout(5s)
    .header("Authorization", "Bearer xxx")
    .build()
    .unwrap();

auto req = xpp::http::Request::builder()
    .method(xpp::http::Method::Post)
    .url("https://api.example.com/upload")
    .header("Content-Type", "application/json")
    .body(R"({"key":"value"})")
    .unwrap();

auto resp = co_await client.send(req).unwrap();
```

### Streaming download

```cpp
auto resp = co_await xpp::http::get("https://large-file.example.com/big.bin").unwrap();
auto& body = resp.body();
char buf[4096];
while (true) {
    ssize_t n = co_await body.read(buf, sizeof(buf));
    if (n <= 0) break;
    process_chunk(buf, n);
}
```

### Compose with `io` module

```cpp
auto resp = co_await xpp::http::get("https://example.com").unwrap();
auto all  = co_await xpp::io::read_all(resp.body());   // Body satisfies AsyncReader
auto file = co_await xpp::fs::File::create("out.bin").unwrap();
co_await xpp::io::copy(resp.body(), file);
```

### Streaming upload

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(64);
xpp::spawn([tx]() -> xpp::Promise<void> {
    for (int i = 0; i < 100; i++) {
        char buf[1024];
        size_t n = generate_chunk(buf);
        co_await tx.send(xpp::Bytes::copy(buf, n));
    }
    tx.close();
});

auto req = xpp::http::Request::builder()
    .post("https://upload.example.com/stream")
    .body(xpp::http::Body::from_channel(rx))
    .unwrap();
auto resp = co_await client.send(req).unwrap();
```
