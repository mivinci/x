## 1. Phase 1 — `xpp::Bytes` basic type

- [ ] 1.1 Create `libxpp/xpp/bytes.h` with license header, `#ifndef` guards, and `namespace xpp`
- [ ] 1.2 Implement `Bytes::Impl` struct holding `Vec<uint8_t> buf`; `Bytes` class with `Shared<Impl> m_impl`, `size_t m_offset`, `size_t m_len`
- [ ] 1.3 Implement factories: `empty()`, `from(Vec<uint8_t>)`, `from(String)`, `from(const char*)`, `from(const uint8_t*, size_t)`, `copy(const char*, size_t)`
- [ ] 1.4 Implement copy/move constructors and assignment (copy = refcount++, move = steal)
- [ ] 1.5 Implement observers: `as_span()`, `data()`, `size()`, `empty()`, `begin()`, `end()`
- [ ] 1.6 Implement `slice(offset, len)` and `slice_from(offset)` — O(1), shares `m_impl`, adjusts offset/len
- [ ] 1.7 Implement `to_vec()` — copies into a new `Vec<uint8_t>`
- [ ] 1.8 Implement `to_string()` returning `Result<String>` (delegates to `String::from_utf8`) and `to_string_lossy()` returning `String`
- [ ] 1.9 Write `bytes_test.cpp` covering: empty, from-Vec round-trip, from-String UTF-8, slice zero-copy (verify same data pointer), slice offset/len bounds, to_vec copies, to_string Ok and Err, to_string_lossy replaces invalid bytes
- [ ] 1.10 Add `bytes_test` target to `libxpp/xpp/CMakeLists.txt` with `cxx_std_11`
- [ ] 1.11 Build with ASan (`scripts/test-mac.sh --asan`), run `bytes_test`, fix all failures
- [ ] 1.12 Verify compiles with `-fno-exceptions -fno-rtti -std=c++11`

## 2. Phase 2 — HTTP basic enums and HeaderMap

- [ ] 2.1 Create `libxpp/xpp/http/` directory and stub headers `method.h`, `status.h`, `header.h` with license headers and `#ifndef` guards
- [ ] 2.2 Implement `Method` namespace in `method.h` (`Value` enum: Get/Post/Put/Delete/Patch/Head/Options/Trace/Connect, re-exported constexpr aliases) plus `to_string(Method::Value)` and `Method::from_string(const String&)`
- [ ] 2.3 Implement `StatusCode` namespace in `status.h` (`Value` enum covering 1xx-5xx common codes, re-exported constexpr aliases), plus `is_informational` / `is_success` / `is_redirect` / `is_client_error` / `is_server_error` helpers, `to_string(StatusCode::Value)`, `StatusCode::from_string(const String&)`
- [ ] 2.4 Implement `HeaderMap` in `header.h` with `Vec<String> m_keys` (lowercased) + `Vec<String> m_values` parallel arrays; methods `insert`, `get`, `contains`, `get_all` (returns `Values` range), `erase`, `empty`, `size`, `begin`, `end`, `from_vec`
- [ ] 2.5 Write `method_test.cpp` covering to_string/from_string round-trip for all enum values, unknown string returns None
- [ ] 2.6 Write `status_test.cpp` covering is_success/is_redirect/etc. for representative codes, to_string/from_string round-trip
- [ ] 2.7 Write `header_test.cpp` covering insert + get (case-insensitive), multi-value (Set-Cookie), erase, iteration order, empty/size, from_vec constructor
- [ ] 2.8 Add test targets to CMakeLists, build with ASan, run tests, fix failures

## 3. Phase 3 — `Body`

- [ ] 3.1 Create `libxpp/xpp/http/body.h` with license header and guards
- [ ] 3.2 Implement `Body` class with `enum class Kind { Empty, Once, Channel }`, `m_kind`, `m_once` (Bytes), `m_rx` (mpsc Receiver<Bytes>), `m_pending` (Bytes)
- [ ] 3.3 Implement factories: `empty()`, `from(Bytes)`, `from(Vec<uint8_t>)`, `from(String)`, `from(const char*)`, `from_channel(sync::mpsc::Receiver<Bytes>)`
- [ ] 3.4 Implement `read(void* buf, size_t len) → Promise<ssize_t>`: Empty returns 0; Once copies + slices m_once; Channel drains m_pending first, then `co_await m_rx.recv()`, returns 0 on None
- [ ] 3.5 Implement `bytes() → Promise<Result<Bytes>>` using `io::read_all(*this)` then aggregating into a single `Bytes` (or `Vec<uint8_t>` then `Bytes::from`)
- [ ] 3.6 Implement `text() → Promise<Result<String>>` = `bytes()` then `Bytes::to_string()`
- [ ] 3.7 Implement `is_empty()`, `is_channel()` observers
- [ ] 3.8 Write `body_test.cpp` covering: Empty read returns 0 immediately; Once read returns all bytes then 0; Once read with small buffer slices correctly; Channel receives chunks via mpsc, read returns chunks then 0 on close; Channel read with small buffer slices leftover; bytes() aggregates all; text() UTF-8 decodes; text() returns Err on invalid UTF-8
- [ ] 3.9 Add `body_test` target, build with ASan, run, fix failures

## 4. Phase 4 — `Request` / `Response` + builders

- [ ] 4.1 Create `libxpp/xpp/http/request.h` and `response.h` with license headers and guards
- [ ] 4.2 Implement `Request` class with `m_method`, `m_url`, `m_headers`, `m_body`; accessors `method()`, `url()`, `headers()`, `body()` (borrow), `into_body()` (move), `has_body()`
- [ ] 4.3 Implement `RequestBuilder` with fluent `method`, `url` (3 overloads), `header` (2 overloads), `bearer_auth`, `basic_auth`; terminators `body(Body)`, `body(Bytes)`, `body(Vec<uint8_t>)`, `body(String)`, `body(const char*)`, `body()` (empty) — all return `Result<Request>`; convenience terminators `get(url)`, `post(url)`, `post(url, body)`, etc.
- [ ] 4.4 Implement `Response` class with `m_status`, `m_headers`, `m_body`, `m_final_url`; accessors `status()`, `status_code()`, `headers()`, `header(name)`, `body()`, `into_body()`, `has_body()`, `url()`; convenience `bytes()`, `text()`
- [ ] 4.5 Implement `ResponseBuilder` with `status` (2 overloads), `header`; terminators `body(...)` overloads returning `Response`; static convenience `ok(...)`, `created(...)`, `no_content()`, `bad_request(...)`, `not_found()`, `internal_server_error(...)`
- [ ] 4.6 Write `request_test.cpp` covering: builder produces correct method/url/headers/body; body overloads store correctly; `into_body()` empties the request; URL overloads compile; bearer_auth/basic_auth set correct header
- [ ] 4.7 Write `response_test.cpp` covering: builder sets status/headers/body; static convenience methods return correct status; `bytes()`/`text()` work on Once body; `into_body()` empties the response
- [ ] 4.8 Add test targets, build with ASan, run, fix failures

## 5. Phase 5 — `Error` + `Client` + `ClientBuilder` + `TestServer`

- [ ] 5.1 Create `libxpp/xpp/http/error.h` with license header and guards
- [ ] 5.2 Implement `Error` class with `Kind` enum (Connect/Dns/Timeout/TooManyRedirects/InvalidUrl/Io/Protocol/Tls/Body), `m_kind`, `m_message`, `m_status` (Option<StatusCode>); accessors `kind()`, `message()`, `status()`, `is_connect()`, `is_timeout()`, `is_redirect()`, `is_status_error()`, `to_string()`
- [ ] 5.3 Create `libxpp/xpp/http/client.h` with license header and guards
- [ ] 5.4 Implement `Client` class wrapping `xHttpClient` from `libx/x/http/client.h`; `send(Request) → Promise<Result<Response>>` via SendAdapter (owns mpsc Sender + PromiseResolver); C callbacks `on_response` (resolve the promise — reqwest semantics; construct Response with Body::from_channel), `on_data` (try_send, full → return 1 pause; resume via the Body's drain hook calling xHttpClientResume), `on_done` (close channel; write shared transfer-error flag on mid-body failure; delete adapter)
- [ ] 5.5 Implement `Client` convenience methods `get(url)`, `post(url)`, `post(url, body)`, `put`, `delete_`, `patch`, `head` with 3 URL overloads each — all delegate to `send(Request::builder()...)`
- [ ] 5.6 Implement `ClientBuilder` with `timeout`, `connect_timeout`, `read_timeout`, `header`, `user_agent`, `redirect`, `max_redirects`, `proxy`, `no_proxy`, `tls`, `danger_accept_invalid_certs`, `http1_only`, `http2_prior_knowledge`, `bearer_auth`, `basic_auth`, `build() → Result<Client>`
- [ ] 5.7 Create `libxpp/xpp/http/test_server.h` with license header, guards, and `namespace xpp::http::test`
- [ ] 5.8 Implement `TestResponseSpec` struct (`status`, `headers`, `body`, `delay_ms` as `uint64_t`)
- [ ] 5.9 Implement `TestServer` class: `start(TestResponseSpec)` binds a loopback `TcpListener` to `127.0.0.1:0` (kernel-assigned port, NOT `get_free_port()` — race-free), reads the real port from `TcpListener::local_addr()`, spawns an accept `xpp::fiber` that reads each request up to `\r\n\r\n` and drains any `Content-Length` body bytes, optionally `co_await xpp::after(delay_ms)`, then writes a preset HTTP/1.1 response (status line + headers + body + `Connection: close`). `port()` returns the bound port. `stop()` closes the listener. Destructor calls `stop()`.
- [ ] 5.10 Write `error_test.cpp` (Layer 1) covering each Kind, is_* predicates, to_string format
- [ ] 5.11 Write `client_test.cpp` (Layer 3) using `TestServer`: `Client::builder().build()` succeeds; `send` returns `Ok(Response)` with correct status/headers/body against a preset `TestServer`; convenience methods (`get`/`post`/...) construct correct Request (verified end-to-end via TestServer response); URL overloads (`String`/`const char*`/`std::string_view`) all compile and round-trip; `timeout` configuration triggers `Error::is_timeout()` against a delayed TestServer response
- [ ] 5.12 Add `client_test` and `error_test` targets to CMakeLists, build with ASan, run, fix failures

## 6. Phase 6 — Top-level convenience functions + docs

- [ ] 6.1 Create `libxpp/xpp/http/http.h` with license header and guards
- [ ] 6.2 Implement `xpp::http::get(url)`, `post(url)`, `post(url, body)`, `put(url)`, `put(url, body)`, `delete_(url)`, `patch(url)`, `patch(url, body)`, `head(url)` with `String`, `const char*`, `std::string_view` (guarded) overloads — each creates a default `Client` and delegates
- [ ] 6.3 Write `http_convenience_test.cpp` (Layer 3) using `TestServer`: each verb (`get`/`post`/`put`/`delete_`/`patch`/`head`) round-trips against a TestServer; each URL overload compiles and round-trips; the one-liner `co_await xpp::http::get("http://127.0.0.1:<port>/")` works end-to-end
- [ ] 6.4 Profile `http::get` cold-start cost; if `curl_global_init` overhead is measurable, add a thread-local default `Client` reused by `http::get` (document in `design.md` open question Q1)
- [ ] 6.5 Add mdBook page `docs/src/xpp/bytes.md` documenting the `Bytes` type, copy/slice semantics, when to use `Bytes` vs `Vec<uint8_t>`
- [ ] 6.6 Add mdBook page `docs/src/xpp/http.md` documenting the client API, push→pull bridge, body streaming, composition with `io::read_all` / `io::copy`, and examples from `design.md`
- [ ] 6.7 Update `docs/src/xpp/SUMMARY.md` to include the new pages
- [ ] 6.8 Add `http_convenience_test` target, build with ASan, run, fix failures

## 7. Phase 7 — Hardening

- [ ] 7.1 Verify the entire module compiles with `-fno-exceptions -fno-rtti -std=c++11`
- [ ] 7.2 Verify `Bytes` refcount behavior under `-DXPP_MT` (Arc path) — thread-safe shared ownership
- [ ] 7.3 Stress test: 100 concurrent `http::get` against `TestServer`, verify no leaks (ASan + LSan)
- [ ] 7.4 Stress test: streaming upload of 100MB via `Body::from_channel`, verify backpressure works (channel doesn't grow unbounded)
- [ ] 7.5 Stress test: streaming download of 100MB via `TestServer` preset body, verify no OOM, channel stays bounded
- [ ] 7.6 Verify `Client::drop` while a request is in-flight doesn't crash (cancelation path)
- [ ] 7.7 Review header include graph: no cycles, no transitively pulling libx C headers into user-facing `xpp/` includes (wrap them in `client.h` internals only)
- [ ] 7.8 Final review of all public API doc comments — every public type and method has a `///` comment
