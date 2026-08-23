# xpp HTTP module — phased implementation plan

Status legend: `[x]` done, `[ ]` pending.

## 1. Phase 1 — `xpp::Bytes` basic type

- [x] 1.1 Create `libxpp/xpp/bytes.h` with license header, `#ifndef` guards, and `namespace xpp`
- [x] 1.2 Implement `Bytes::Impl` struct holding `Vec<uint8_t> buf`; `Bytes` class with `Arc<Impl> m_impl`, `size_t m_offset`, `size_t m_len` (was `Shared<Impl>` until #77 made Shared = Arc)
- [x] 1.3 Implement factories: `empty()`, `from(Vec<uint8_t>)`, `from(String)`, `from(const char*)`, `from(const uint8_t*, size_t)`, `copy(const char*, size_t)`
- [x] 1.4 Implement copy/move constructors and assignment (copy = refcount++, move = steal)
- [x] 1.5 Implement observers: `as_span()`, `data()`, `size()`, `empty()`, `begin()`, `end()`
- [x] 1.6 Implement `slice(offset, len)` and `slice_from(offset)` — O(1), shares `m_impl`, adjusts offset/len
- [x] 1.7 Implement `to_vec()` — copies into a new `Vec<uint8_t>`
- [x] 1.8 Implement `to_string()` returning `Result<String>` (delegates to `String::from_utf8`) and `to_string_lossy()` returning `String`
- [x] 1.9 Write `bytes_test.cpp` covering: empty, from-Vec round-trip, from-String UTF-8, slice zero-copy (verify same data pointer), slice offset/len bounds, to_vec copies, to_string Ok and Err, to_string_lossy replaces invalid bytes
- [x] 1.10 Add `bytes_test` target to `libxpp/xpp/CMakeLists.txt` with `cxx_std_11`
- [x] 1.11 Build with ASan (`scripts/test-mac.sh --asan`), run `bytes_test`, fix all failures
- [x] 1.12 Verify compiles with `-fno-exceptions -fno-rtti -std=c++11`

## 2. Phase 2 — HTTP basic enums and HeaderMap

- [x] 2.1 Create `libxpp/xpp/http/` directory and stub headers `method.h`, `status.h`, `header.h` with license headers and `#ifndef` guards
- [x] 2.2 Implement `Method` namespace in `method.h` (`Value` enum: Get/Post/Put/Delete/Patch/Head/Options/Trace/Connect, re-exported constexpr aliases) plus `to_string(Method::Value)` and `Method::from_string(const String&)`
- [x] 2.3 Implement `StatusCode` namespace in `status.h` (`Value` enum covering 1xx-5xx common codes, re-exported constexpr aliases), plus `is_informational` / `is_success` / `is_redirect` / `is_client_error` / `is_server_error` helpers, `to_string(StatusCode::Value)`, `StatusCode::from_string(const String&)`
- [x] 2.4 Implement `HeaderMap` in `header.h` with `Vec<String> m_keys` (lowercased) + `Vec<String> m_values` parallel arrays; methods `insert`, `get`, `contains`, `get_all` (returns `Values` range), `erase`, `empty`, `size`, `begin`, `end`, `from_vec`
- [x] 2.5 Write `method_test.cpp` covering to_string/from_string round-trip for all enum values, unknown string returns None
- [x] 2.6 Write `status_test.cpp` covering is_success/is_redirect/etc. for representative codes, to_string/from_string round-trip
- [x] 2.7 Write `header_test.cpp` covering insert + get (case-insensitive), multi-value (Set-Cookie), erase, iteration order, empty/size, from_vec constructor
- [x] 2.8 Add test targets to CMakeLists, build with ASan, run tests, fix failures

## 3. Phase 3 — `Body`

- [x] 3.1 Create `libxpp/xpp/http/body.h` with license header and guards
- [x] 3.2 Implement `Body` class with `enum class Kind { Empty, Once, Channel }`, `m_kind`, `m_once` (Bytes), `m_rx` (mpsc Receiver<Bytes>), `m_pending` (Bytes) — note: storage is an `Enum<Bytes, Receiver<Bytes>>` Option, plus backpressure hooks (`on_drain`, shared `xfer_error` flag added in #75)
- [x] 3.3 Implement factories: `empty()`, `from(Bytes)`, `from(Vec<uint8_t>)`, `from(String)`, `from(const char*)`, `from_channel(sync::mpsc::Receiver<Bytes>)` — `from_channel` gained optional `on_drain` + `xfer_error` params (#75)
- [x] 3.4 Implement `read(void* buf, size_t len) → Promise<ssize_t>`: Empty returns 0; Once copies + slices; Channel drains `m_pending` first, then awaits `rx.recv()`, 0 on close / -1 on transfer error
- [x] 3.5 Implement `bytes() → Promise<Result<Bytes>>` using `io::read_all(*this)` then aggregating into a single `Bytes` (or `Vec<uint8_t>` then `Bytes::from`); surfaces `xfer_error`
- [x] 3.6 Implement `text() → Promise<Result<String>>` = `bytes()` then `Bytes::to_string()`
- [x] 3.7 Implement `is_empty()`, `is_channel()` observers
- [x] 3.8 Write `body_test.cpp` covering: Empty read returns 0 immediately; Once read returns all bytes then 0; Once read with small buffer slices correctly; Channel receives chunks via mpsc, read returns chunks then 0 on close; Channel read with small buffer slices leftover; bytes() aggregates all; text() UTF-8 decodes; text() returns Err on invalid UTF-8
- [x] 3.9 Add `body_test` target, build with ASan, run, fix failures

## 4. Phase 4 — `Request` / `Response` + builders

- [x] 4.1 Create `libxpp/xpp/http/request.h` and `response.h` with license headers and guards
- [x] 4.2 Implement `Request` class with `m_method`, `m_url`, `m_headers`, `m_body`; accessors `method()`, `url()`, `headers()`, `body()` (borrow), `into_body()` (move), `has_body()`
- [x] 4.3 Implement `RequestBuilder` with fluent `method`, `url` (3 overloads), `header` (2 overloads), `bearer_auth`, `basic_auth`; terminators `body(Body)`, `body(Bytes)`, `body(Vec<uint8_t>)`, `body(String)`, `body(const char*)`, `body()` (empty) — all return `Result<Request>`. (Convenience terminators `get(url)`/`post(url)` moved to the Client convenience layer — see 5.5.)
- [x] 4.4 Implement `Response` class with `m_status`, `m_headers`, `m_body`, `m_final_url`; accessors `status()`, `status_code()`, `headers()`, `header(name)`, `body()`, `into_body()`, `has_body()`, `url()`; convenience `bytes()`, `text()`
- [x] 4.5 Implement `ResponseBuilder` with `status` (2 overloads), `header`, `url` (added #75); terminators `body(...)` overloads returning `Response`; static convenience `ok(...)`, `created(...)`, `no_content()`, `bad_request(...)`, `not_found()`, `internal_server_error(...)`
- [x] 4.6 Write `request_test.cpp` covering: builder produces correct method/url/headers/body; body overloads store correctly; `into_body()` empties the request; URL overloads compile; bearer_auth/basic_auth set correct header
- [x] 4.7 Write `response_test.cpp` covering: builder sets status/headers/body; static convenience methods return correct status; `bytes()`/`text()` work on Once body; `into_body()` empties the response
- [x] 4.8 Add test targets, build with ASan, run, fix failures

## 5. Phase 5 — `Error` + `Client` + `ClientBuilder` + `TestServer` (PR #75)

- [x] 5.1 Create `libxpp/xpp/http/error.h` with license header and guards
- [x] 5.2 Implement `Error` class with `Kind` enum (Connect/Dns/Timeout/TooManyRedirects/InvalidUrl/Io/Protocol/Tls/Body), `m_kind`, `m_message`, `m_status` (Option<StatusCode>); accessors `kind()`, `message()`, `status()`, `is_connect()`, `is_timeout()`, `is_redirect()`, `is_status_error()`, `to_string()`
- [x] 5.3 Create `libxpp/xpp/http/client.h` with license header and guards
- [x] 5.4 Implement `Client` class wrapping `xHttpClient` from `libx/x/http/client.h`; `send(Request) → Promise<Result<Response>>` via SendAdapter (owns mpsc Sender + PromiseResolver); C callbacks `on_response` (resolve the promise — reqwest semantics; construct Response with Body::from_channel), `on_data` (try_send, full → return 1 pause; resume via the Body's drain hook calling xHttpClientResume), `on_done` (close channel; write shared transfer-error flag on mid-body failure; delete adapter)
- [ ] 5.5 Implement `Client` convenience methods `get(url)`, `post(url)`, `post(url, body)`, `put`, `delete_`, `patch`, `head` with 3 URL overloads each — all delegate to `send(Request::builder()...)`. **Deferred to Phase 6** (do together with the top-level `xpp::http::get` etc.; same delegation pattern).
- [x] 5.6 Implement `ClientBuilder` with `timeout`, `connect_timeout`, `read_timeout`, `header`, `user_agent`, `redirect`, `max_redirects`, `proxy`, `no_proxy`, `tls`, `danger_accept_invalid_certs`, `http1_only`, `http2_prior_knowledge`, `bearer_auth`, `basic_auth`, `build() → Result<Client>` — `read_timeout` now functional (idle-body timer, #75)
- [x] 5.7 Create `libxpp/xpp/http/test_server.h` with license header, guards, and `namespace xpp::http::test`
- [x] 5.8 Implement `TestResponseSpec` struct (`status`, `headers`, `body`, `delay_ms`; plus `echo_request_body`, `redirect_to`, `truncate_body_after`, `mid_body_delay_ms` added in #75)
- [x] 5.9 Implement `TestServer` class: `start(TestResponseSpec)` binds a loopback `xTcpListener` to `127.0.0.1:0` (kernel-assigned port, race-free), reads the port via `getsockname`, and serves each connection event-driven through the conn's xSocket (level-triggered): reads the request up to `\r\n\r\n` + `Content-Length` body, then writes the preset HTTP/1.1 response (partial-write pumping), optionally delayed. **Implementation note:** pure libx C API, no fibers — the original fiber-based design (accept fiber + `co_await xpp::after`) was replaced to avoid fiber/loop deadlocks on Linux shared builds. `port()`, `stop()`, destructor `stop()`.
- [x] 5.10 Write `error_test.cpp` (Layer 1) covering each Kind, is_* predicates, to_string format
- [x] 5.11 Write `client_test.cpp` (Layer 3) using `TestServer`: `Client::builder().build()` succeeds; `send` returns `Ok(Response)` with correct status/headers/body; backpressure (8MB, pause/resume); mid-body disconnect reports error; read-timeout; redirects (final URL + final headers); request body transmitted (echo). (Convenience-method and URL-overload coverage deferred to 6.3 with the top-level functions.)
- [x] 5.12 Add `client_test` and `error_test` targets to CMakeLists, build with ASan, run, fix failures

## 6. Phase 6 — Client convenience methods + top-level functions + docs

- [ ] 6.1 Client convenience methods (`Client::get/post/put/delete_/patch/head`) — see 5.5
- [ ] 6.2 Create `libxpp/xpp/http/http.h` (umbrella) with license header and guards
- [ ] 6.3 Implement top-level `xpp::http::get(url)`, `post(url)`, `post(url, body)`, `put(url)`, `put(url, body)`, `delete_(url)`, `patch(url)`, `patch(url, body)`, `head(url)` with `String`, `const char*`, `std::string_view` (guarded) overloads — each creates a default `Client` and delegates
- [ ] 6.4 Write `http_convenience_test.cpp` (Layer 3) using `TestServer`: each verb round-trips; each URL overload compiles and round-trips; `co_await xpp::http::get("http://127.0.0.1:<port>/")` works end-to-end
- [ ] 6.5 Profile `http::get` cold-start cost; if `curl_global_init` overhead is measurable, add a thread-local default `Client` reused by `http::get` (document in `design.md` open question Q1)
- [ ] 6.6 Add mdBook page `docs/libxpp/bytes.md` documenting the `Bytes` type, copy/slice semantics, when to use `Bytes` vs `Vec<uint8_t>`
- [ ] 6.7 Add mdBook page `docs/libxpp/http.md` documenting the client API, push→pull bridge, body streaming, composition with `io::read_all` / `io::copy`, and examples from `design.md`
- [ ] 6.8 Update `docs/SUMMARY.md` to include the new pages
- [ ] 6.9 Add `http_convenience_test` target, build with ASan, run, fix failures

## 7. Phase 7 — Hardening

- [ ] 7.1 Verify the entire module compiles with `-fno-exceptions -fno-rtti -std=c++11`
- [ ] 7.2 Verify `Bytes` refcount behavior under Arc — thread-safe shared ownership (the `XPP_MT` build option was removed in #77; Arc is always on)
- [ ] 7.3 Stress test: 100 concurrent `http::get` against `TestServer`, verify no leaks (ASan + LSan) — depends on Phase 6
- [ ] 7.4 Stress test: streaming upload of 100MB via `Body::from_channel`, verify backpressure works (channel doesn't grow unbounded) — 8MB covered in client_test; scale up
- [ ] 7.5 Stress test: streaming download of 100MB via `TestServer` preset body, verify no OOM, channel stays bounded
- [ ] 7.6 Verify `Client::drop` while a request is in-flight doesn't crash (cancelation path)
- [ ] 7.7 Review header include graph: no cycles, no transitively pulling libx C headers into user-facing `xpp/` includes (wrap them in `client.h` internals only)
- [ ] 7.8 Final review of all public API doc comments — every public type and method has a `///` comment
