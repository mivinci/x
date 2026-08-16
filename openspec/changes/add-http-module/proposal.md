## Why

libxpp currently has no HTTP module. An earlier attempt on branch `add-http-module` (2026-07-08 ~ 07-10, 25 commits, 1220 lines, 87 tests) was abandoned because of six design problems:

1. **Body model oscillated between push and pull** — `client.h` used push (`on_data` → `std::deque<bytes::Bytes>`), `response.h` used pull (`body(R&&reader)`), and `client.h::on_done` re-wrapped the deque into a `read_fn`. The final commit on the branch was `revert: request body pre-buffering`.
2. **Body abstraction was `std::function<ssize_t(char*, size_t)>` (synchronous)**, bypassing the existing `AsyncReader` concept (`read() → Promise<ssize_t>`). `Response::text()` ran a blocking `while` loop calling `fn(buf, 4096)` — this freezes a single-threaded EventLoop.
3. **`HeaderMap` used `std::multimap<std::string, std::string>`** — cache-unfriendly, allocates per lookup. HTTP headers are few (5-20); linear scan beats red-black tree.
4. **No `http::get(url)` convenience function** — the presentation promised `xpp::http::get(url).await()`, but users had to write 4 lines of `Client::builder()...Request::builder()...`.
5. **Server handler `.await()`ed inside the C callback** (`server.h:188`) — parks the EventLoop; any async work inside the handler deadlocks.
6. **Duplicated `bytes::Bytes` and `bytes::Reader` modules** — overlapped with existing `libxpp/xpp/io/` (`BufReader`, `AsyncReader`, `read_all`, `copy`).

This change redesigns the HTTP module from scratch, following the same path as hyper 0.10 → 0.12: bridging a push-based C library (`libx` + libcurl) to a pull-based async API via an mpsc channel, with type naming aligned to `hyper` + `reqwest` so users familiar with Rust's HTTP stack find the same shapes.

## What Changes

Introduce `xpp::http` — a client-side HTTP module for libxpp (server side is out of scope for this change), plus a new top-level `xpp::Bytes` basic type.

### `xpp::Bytes` — refcounted immutable byte block

- New header: `libxpp/xpp/bytes.h`
- O(1) copy (refcount), O(1) slice (offset + len, shares buffer)
- Mirrors Rust's `bytes::Bytes`; complements `xpp::String` / `xpp::Vec` as a basic type
- Internal storage: `Shared<Impl>` (Rc or Arc, compile-time via `-DXPP_MT`)

### `xpp::http` — client-side HTTP

New headers under `libxpp/xpp/http/`:

- `method.h` — `enum class Method` (Get/Post/Put/Delete/Patch/Head/Options/Trace/Connect)
- `status.h` — `enum class StatusCode : uint16_t` with helpers (`is_success`, `is_redirect`, etc.)
- `header.h` — `HeaderMap` backed by `Vec<String>` parallel arrays (keys lowercased), linear-scan lookup
- `body.h` — `Body` as enum `{ Empty, Once, Channel }`; implements `AsyncReader` concept; `Body::from_channel(mpsc::Receiver<Bytes>)` bridges push→pull
- `request.h` — `Request` + `RequestBuilder` (hyper-style builder terminating in `Result<Request>`)
- `response.h` — `Response` + `ResponseBuilder`; `body()` borrows, `into_body()` moves; `bytes()` / `text()` convenience consumers
- `client.h` — `Client` + `ClientBuilder` (timeout, redirect policy, TLS, proxy, auth)
- `error.h` — `Error` with `Kind` enum (Connect, Dns, Timeout, TooManyRedirects, InvalidUrl, Io, Protocol, Tls, Body)
- `http.h` — top-level convenience functions `http::get/post/put/delete_/patch/head` with `String` / `const char*` / `std::string_view` overloads
- `test_server.h` — **test-only** helper, `xpp::http::test::TestServer` runs a minimal HTTP/1.1 responder on loopback for client integration tests; not part of the public API, lives under `xpp/http/` alongside the production headers but guarded by a `test` subnamespace

### Push→Pull bridge

The C library (`libx` `xHttpClient`) pushes data via `on_data(data, len, arg)` callbacks. The xpp io module pulls via `AsyncReader::read(buf, len) → Promise<ssize_t>`. An mpsc channel bridges them:

```
on_data(chunk)  → try_send(chunk) → channel → recv().await() → Body::read() returns
on_done()       → close channel   → next recv() returns None → read() returns 0 (EOF)
```

Channel is bounded (64 chunks). When full, `try_send` returns `Full`, and the C callback returns `-1` to tell `libx` to pause — natural backpressure, no OOM.

## Capabilities

### New Capabilities

- `xpp-bytes`: A refcounted, immutable byte block type `xpp::Bytes` with O(1) copy and O(1) slice. Lives at top-level `xpp::` namespace alongside `xpp::String` and `xpp::Vec`. Backed by `Shared<Impl>` (Rc single-threaded, Arc with `-DXPP_MT`).
- `xpp-http`: A client-side HTTP module for libxpp. Types `Client`, `Request`, `Response`, `Body`, `Method`, `StatusCode`, `HeaderMap`, `Error`, plus top-level convenience functions `http::get/post/put/delete_/patch/head`. Body implements `AsyncReader`, composing with `xpp::io::read_all` and `xpp::io::copy`. Push→pull bridge via `xpp::sync::mpsc` channel (bounded 64). API naming aligned with `hyper` + `reqwest`.

### Modified Capabilities

(none — `openspec/specs/` is currently empty; no existing capabilities to modify)

## Impact

- **New code**:
  - `libxpp/xpp/bytes.h` — new basic type
  - `libxpp/xpp/http/{method,status,header,body,request,response,client,error,http,test_server}.h` — 10 new headers (9 production + 1 test-only)
- **New dependencies**: none — uses existing `xpp::Shared`, `xpp::String`, `xpp::Vec`, `xpp::Option`, `xpp::Result`, `xpp::sync::mpsc`, `xpp::io::AsyncReader`, `xpp::net::TcpListener`, `xpp::promise::Promise`, and `libx/x/http/` (C client API)
- **Wrapped backend**: `libx/x/http/client.h` — already in tree, no changes required
- **Build system**: `libxpp/xpp/CMakeLists.txt` adds the `http/` subtree to the header glob and test targets
- **Tests**: new `bytes_test.cpp`, `http_{method,status,header,body,request,response,client,convenience}_test.cpp`; all C++11-compatible, follow the existing `_test.cpp` + Google Test convention. Client integration tests use `xpp::http::test::TestServer` (local loopback HTTP/1.1 responder) — no external process, no real network egress.
- **No breaking changes**: pure addition. Existing libxpp public API is untouched. No new public C-linkage symbols in `libx`.
- **C++11 constraint preserved**: no `if constexpr`, no `std::variant`/`std::optional`/`std::string_view` in headers that must stay C++11 (convenience overloads using `std::string_view` are guarded by `__cpp_lib_string_view`).
- **Documentation**: new mdBook page under `docs/src/xpp/{bytes,http}.md` covering type model, builder usage, push→pull bridge, and per-method examples
- **Out of scope** (future changes):
  - Server side (`Server`, `Router`, `Handler`)
  - HTTP/2, TLS, WebSocket (libx has C implementations; xpp wrappers are follow-up)
  - `Response::json<T>()` (depends on the `xpp-serde` capability)
  - `Bytes` integration into `xpp::io` as a first-class reader/writer
