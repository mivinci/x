## ADDED Requirements

### Requirement: `xpp::Bytes` basic type

The library SHALL provide a type `xpp::Bytes` representing an immutable, reference-counted byte block. Copies SHALL be O(1) (reference count increment only). Slices SHALL be O(1) (adjust offset and length, sharing the underlying buffer). The type SHALL be default-constructible to an empty state, copyable, movable, and usable as a value type in `xpp::Vec`, `xpp::Option`, and `xpp::sync::mpsc` channels. The type SHALL NOT require heap allocation for the default-constructed (empty) state beyond the `Shared` control block.

#### Scenario: Default-constructed `Bytes` is empty

- **WHEN** a `Bytes` value is default-constructed
- **THEN** `b.empty()` returns `true` and `b.size()` returns `0`

#### Scenario: Copy does not copy the underlying buffer

- **WHEN** a `Bytes` value `a` is constructed from `Bytes::from(Vec<uint8_t>{0x01, 0x02, 0x03})` and `Bytes b = a;` is executed
- **THEN** `b.data() == a.data()` (same pointer, shared buffer) and `b.size() == a.size()`

#### Scenario: Slice shares the buffer

- **WHEN** `Bytes a = Bytes::from(Vec<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x05});` and `Bytes b = a.slice(1, 3);`
- **THEN** `b.size() == 3`, `b.data()[0] == 0x02`, `b.data()[1] == 0x03`, `b.data()[2] == 0x04`, and `b.data() == a.data() + 1` (no copy)

#### Scenario: `to_vec` copies

- **WHEN** `Bytes a = Bytes::from(Vec<uint8_t>{0x01, 0x02, 0x03});` and `Vec<uint8_t> v = a.to_vec();`
- **THEN** `v.size() == 3`, `v[0] == 0x01`, and modifying `v` does not affect `a`

#### Scenario: `to_string` returns Err on invalid UTF-8

- **WHEN** `Bytes a = Bytes::from(Vec<uint8_t>{0xff, 0xfe, 0xfd});` and `auto r = a.to_string();`
- **THEN** `r.is_err()` returns `true`

#### Scenario: `to_string_lossy` replaces invalid bytes

- **WHEN** `Bytes a = Bytes::from(Vec<uint8_t>{0x68, 0x69, 0xff});` (valid "hi" + invalid 0xff) and `String s = a.to_string_lossy();`
- **THEN** `s` contains "hi" followed by the replacement character U+FFFD, and `s.size() > 0`

### Requirement: `xpp::http::Method` enum

The library SHALL provide `xpp::http::Method` (a namespace; the underlying enum is `Method::Value`) with enumerators `Get`, `Post`, `Put`, `Delete`, `Patch`, `Head`, `Options`, `Trace`, `Connect` re-exported as constexpr values (`Method::Get`, ...). The library SHALL provide `to_string(Method::Value)` returning the canonical HTTP method string and `Method::from_string(const String&)` returning `Option<Method::Value>` (None for unknown strings).

#### Scenario: Round-trip all methods

- **WHEN** each enumerator of `Method` is passed to `to_string` and the result back to `Method::from_string`
- **THEN** the resulting `Option<Method>` is `Some` and equals the original enumerator

#### Scenario: Unknown string returns None

- **WHEN** `Method::from_string("FOO")` is called
- **THEN** the result is `None`

### Requirement: `xpp::http::StatusCode` enum

The library SHALL provide `xpp::http::StatusCode` (a namespace; the underlying enum is `StatusCode::Value`) with enumerators for common HTTP status codes (1xx through 5xx, including at minimum `Ok=200`, `Created=201`, `NoContent=204`, `MovedPermanently=301`, `Found=302`, `NotModified=304`, `BadRequest=400`, `Unauthorized=401`, `Forbidden=403`, `NotFound=404`, `MethodNotAllowed=405`, `Conflict=409`, `TooManyRequests=429`, `InternalServerError=500`, `BadGateway=502`, `ServiceUnavailable=503`, `GatewayTimeout=504`) re-exported as constexpr values (`StatusCode::Ok`, ...). The library SHALL provide `is_informational`, `is_success`, `is_redirect`, `is_client_error`, `is_server_error` predicates, plus `to_string(StatusCode::Value)` and `StatusCode::from_string(const String&)`.

#### Scenario: `is_success` for 2xx codes

- **WHEN** `StatusCode::Ok` and `StatusCode::Created` are passed to `is_success`
- **THEN** both return `true`

#### Scenario: `is_server_error` for 5xx codes

- **WHEN** `StatusCode::InternalServerError` and `StatusCode::BadGateway` are passed to `is_server_error`
- **THEN** both return `true`

#### Scenario: Non-5xx is not server error

- **WHEN** `StatusCode::Ok` is passed to `is_server_error`
- **THEN** the result is `false`

### Requirement: `xpp::http::HeaderMap`

The library SHALL provide `xpp::http::HeaderMap` storing HTTP headers with case-insensitive keys. Lookups SHALL be case-insensitive. The container SHALL support multiple values for the same key (e.g., `Set-Cookie`). The internal representation SHALL be a pair of parallel `Vec<String>` arrays (keys stored lowercased, values original-case) — no node-based container (`std::multimap`, `std::map`).

#### Scenario: Case-insensitive lookup

- **WHEN** `HeaderMap h; h.insert("Content-Type", "application/json");` and `auto v = h.get("content-type");`
- **THEN** `v.is_some()` returns `true` and `v.unwrap()` equals `"application/json"`

#### Scenario: Multiple values for one key

- **WHEN** `HeaderMap h; h.insert("Set-Cookie", "a=1"); h.insert("Set-Cookie", "b=2");` and `auto vs = h.get_all("Set-Cookie");`
- **THEN** iterating `vs` yields two values: `"a=1"` and `"b=2"`

#### Scenario: Erase removes all values for a key

- **WHEN** a `HeaderMap` has two `Set-Cookie` entries and `erase("Set-Cookie")` is called
- **THEN** `contains("Set-Cookie")` returns `false` and the return value of `erase` is `2`

#### Scenario: Iteration order matches insertion order

- **WHEN** three headers are inserted in order `A`, `B`, `C` and the map is iterated
- **THEN** the iteration yields headers in the order `A`, `B`, `C`

### Requirement: `xpp::http::Body` satisfies `AsyncReader`

The library SHALL provide `xpp::http::Body` with three internal states: `Empty`, `Once` (a single `Bytes`), and `Channel` (an `xpp::sync::mpsc::Receiver<Bytes>`). `Body` SHALL satisfy the `xpp::io::AsyncReader` concept by providing `read(void* buf, size_t len) → Promise<ssize_t>`. `read` SHALL return `0` on EOF (Empty exhausted, Once exhausted, or Channel closed). `read` SHALL slice leftover bytes from a previous chunk when `len < chunk.size()` in Channel mode, storing the remainder without copying.

#### Scenario: Empty body read returns 0 immediately

- **WHEN** `Body b = Body::empty();` and `ssize_t n = co_await b.read(buf, 1024);`
- **THEN** `n == 0` (EOF) and no `Promise` suspension occurred

#### Scenario: Once body returns all bytes then EOF

- **WHEN** `Body b = Body::from(Bytes::from("hello"));` and `read(buf, 1024)` is called twice
- **THEN** the first call returns `5` and writes `"hello"` to `buf`; the second call returns `0`

#### Scenario: Once body with small buffer slices correctly

- **WHEN** `Body b = Body::from(Bytes::from("hello"));` and `read(buf, 3)` then `read(buf, 3)` are called
- **THEN** the first call returns `3` and writes `"hel"`; the second returns `2` and writes `"lo"`; a third call returns `0`

#### Scenario: Channel body receives chunks then EOF on close

- **WHEN** a `mpsc::channel<Bytes>(256)` is created, `Body::from_channel(rx)` is constructed, the sender pushes `Bytes::from("abc")` then `Bytes::from("def")` then closes, and `read(buf, 1024)` is called three times
- **THEN** the first call returns `3` and writes `"abc"`; the second returns `3` and writes `"def"`; the third returns `0`

#### Scenario: Channel body slices leftover without copy

- **WHEN** a Channel `Body` receives a 16-byte chunk and `read(buf, 4)` is called
- **THEN** the call returns `4`, writes the first 4 bytes, and the next `read(buf, 16)` returns `12` and writes the remaining 12 bytes (sliced from the same underlying `Bytes` buffer)

### Requirement: `Body::bytes()` and `Body::text()`

The library SHALL provide `Body::bytes() → Promise<Result<Bytes>>` that aggregates all chunks into a single `Bytes`, and `Body::text() → Promise<Result<String>>` that calls `bytes()` then decodes as UTF-8. If the underlying channel closes with an error (connection reset), `bytes()` SHALL return `Err`. If the aggregated bytes are not valid UTF-8, `text()` SHALL return `Err`.

#### Scenario: `bytes()` aggregates multiple chunks

- **WHEN** a Channel `Body` receives `Bytes::from("abc")`, `Bytes::from("def")`, then closes, and `bytes()` is called
- **THEN** the result is `Ok` and the `Bytes` equals `"abcdef"`

#### Scenario: `text()` decodes UTF-8

- **WHEN** a Channel `Body` receives `Bytes::from("hello")` then closes, and `text()` is called
- **THEN** the result is `Ok` and the `String` equals `"hello"`

#### Scenario: `text()` returns Err on invalid UTF-8

- **WHEN** a Channel `Body` receives `Bytes::from(Vec<uint8_t>{0xff, 0xfe})` then closes, and `text()` is called
- **THEN** the result is `Err`

### Requirement: `Body` factories

The library SHALL provide `Body::empty()`, `Body::from(Bytes)`, `Body::from(Vec<uint8_t>)`, `Body::from(String)`, `Body::from(const char*)`, and `Body::from_channel(sync::mpsc::Receiver<Bytes>)`. The `Vec`, `String`, and `const char*` overloads SHALL internally convert to `Bytes` and delegate to `Body::from(Bytes)`.

#### Scenario: `Body::from(const char*)` constructs an Once body

- **WHEN** `Body b = Body::from("hello");` and `b.read(buf, 1024)` is called
- **THEN** the call returns `5` and writes `"hello"`

#### Scenario: `Body::from_channel` constructs a Channel body

- **WHEN** a `mpsc::Receiver<Bytes>` is passed to `Body::from_channel` and `is_channel()` is called on the result
- **THEN** `is_channel()` returns `true`

### Requirement: `xpp::http::Request` and `RequestBuilder`

The library SHALL provide `xpp::http::Request` with accessors `method()`, `url()`, `headers()`, `body()` (borrow), `into_body()` (move), `has_body()`. The library SHALL provide `Request::builder()` returning a `RequestBuilder` with fluent configurators `method`, `url` (3 overloads: `String`, `const char*`, `std::string_view` guarded), `header` (2 overloads), `bearer_auth`, `basic_auth`. Termination methods `body(...)` (overloads: `Body`, `Bytes`, `Vec<uint8_t>`, `String`, `const char*`, empty) SHALL return `Result<Request>`. The library SHALL NOT provide method+url convenience terminators (like `get(url)` or `post(url, body)`) on `RequestBuilder` — callers set `method()` + `url()` + `body()` explicitly. Method+url convenience functions live on `Client` (`Client::get(url)` etc. — see the Client requirement).

#### Scenario: Builder produces a Request with all fields set

- **WHEN** `Request::builder().method(Method::Post).url("https://x.com").header("A", "1").body("payload").unwrap()` is executed
- **THEN** the resulting `Request` has `method() == Method::Post`, `url() == "https://x.com"`, `headers().get("a")` returns `Some("1")` (case-insensitive), and `body().read(buf, 1024)` returns `"payload"`

#### Scenario: `into_body` moves the body out

- **WHEN** a `Request` with a non-empty body has `into_body()` called
- **THEN** the returned `Body` contains the original bytes, and subsequent `has_body()` on the `Request` returns `false`

#### Scenario: URL overloads compile

- **WHEN** `Request::builder().url(String("https://x.com"))`, `Request::builder().url("https://x.com")`, and (when `__cpp_lib_string_view` is defined) `Request::builder().url(std::string_view("https://x.com"))` are compiled
- **THEN** all three compile successfully

### Requirement: `xpp::http::Response` and `ResponseBuilder`

The library SHALL provide `xpp::http::Response` with accessors `status()`, `status_code()`, `headers()`, `header(name)`, `body()` (borrow), `into_body()` (move), `has_body()`, `url()` (final URL after redirects). The library SHALL provide convenience methods `bytes() → Promise<Result<Bytes>>` and `text() → Promise<Result<String>>` that delegate to `into_body().bytes()` and `into_body().text()`. The library SHALL provide `Response::builder()` returning a `ResponseBuilder` with `status` (2 overloads), `header`, and `body(...)` terminators returning `Response`. Static convenience methods `ok`, `created`, `no_content`, `bad_request`, `not_found`, `internal_server_error` SHALL return pre-configured `Response` values.

#### Scenario: Builder produces a Response

- **WHEN** `Response::builder().status(StatusCode::NotFound).header("X", "1").body("err").` is executed
- **THEN** the resulting `Response` has `status() == StatusCode::NotFound`, `headers().get("x")` returns `Some("1")`, and `body().read(buf, 1024)` returns `"err"`

#### Scenario: `bytes()` consumes the body

- **WHEN** a `Response` with body `"hello"` has `bytes()` called
- **THEN** the returned `Promise` resolves to `Ok(Bytes)` and the `Bytes` equals `"hello"`

#### Scenario: Static `ok(String)` returns 200 with body

- **WHEN** `Response::ok("hello")` is called
- **THEN** the result has `status() == StatusCode::Ok` and body contains `"hello"`

### Requirement: `xpp::http::Client` and `ClientBuilder`

The library SHALL provide `xpp::http::Client` wrapping `libx/x/http/`'s `xHttpClient`. The library SHALL provide `Client::send(Request) → Promise<Result<Response>>` as the generic entry point, resolving as soon as the response headers arrive (reqwest semantics): `Ok(Response)` with a live streamed `Body`, or `Err` for transport failures before headers and for 4xx/5xx statuses. A body transfer that fails mid-body SHALL surface the error on the body read (`bytes()` returns `Err`), not a silent truncated EOF. The library SHALL provide convenience methods `get(url)`, `post(url)`, `post(url, body)`, `put(url)`, `put(url, body)`, `del(url)`, `patch(url)`, `patch(url, body)`, `head(url)` with 3 URL overloads each — all internally constructing a `Request` and delegating to `send`. The library SHALL provide `Client::builder()` returning a `ClientBuilder` with configuration methods: `timeout`, `connect_timeout`, `read_timeout`, `header`, `user_agent`, `redirect` (accepting `RedirectPolicy`), `max_redirects`, `proxy`, `no_proxy`, `tls`, `danger_accept_invalid_certs`, `http1_only`, `http2_prior_knowledge`, `bearer_auth`, `basic_auth`. Termination method `build()` SHALL return `Result<Client>`.

#### Scenario: `send` returns a Response from a local HTTP server

- **WHEN** a local TCP listener speaks HTTP/1.1 and `Client::builder().build().unwrap().send(Request::builder().get("http://127.0.0.1:<port>/").unwrap())` is executed
- **THEN** the returned `Promise` resolves to `Ok(Response)` with `status() == StatusCode::Ok`

#### Scenario: Convenience `get(url)` delegates to `send`

- **WHEN** `Client::builder().build().unwrap().get("http://127.0.0.1:<port>/")` is executed against a local listener
- **THEN** the returned `Promise` resolves to `Ok(Response)` with `status() == StatusCode::Ok`

#### Scenario: `ClientBuilder` configuration is honored

- **WHEN** `Client::builder().timeout(1ms).build().unwrap().get("http://127.0.0.1:<port>/slow")` is executed against a local listener that delays 100ms before responding
- **THEN** the returned `Promise` resolves to `Err` with `Error::kind() == Error::Kind::Timeout`

### Requirement: `xpp::http::Error`

The library SHALL provide `xpp::http::Error` with a `Kind` enum (enumerators `Connect`, `Dns`, `Timeout`, `TooManyRedirects`, `InvalidUrl`, `Io`, `Protocol`, `Tls`, `Body`). Accessors `kind()`, `message()`, `status()` (returning `Option<StatusCode>` for HTTP error responses). Predicates `is_connect()`, `is_timeout()`, `is_redirect()`, `is_status_error()`. A `to_string()` method SHALL produce a human-readable description.

#### Scenario: `is_timeout` predicate

- **WHEN** an `Error` is constructed with `Kind::Timeout`
- **THEN** `is_timeout()` returns `true` and `is_connect()` returns `false`

#### Scenario: `status()` is Some for HTTP error responses

- **WHEN** an `Error` is constructed with `Kind::Protocol` and an associated `StatusCode::BadRequest`
- **THEN** `status()` returns `Some(StatusCode::BadRequest)`

### Requirement: Top-level convenience functions `http::get/post/...` (DEFERRED)

**Deferred.** Top-level free functions (`xpp::http::get(url)` etc.) were
prototyped in Phase 6 but not shipped: a default Client has no
configuration, and in C++ its lifetime must span the whole transfer
(unlike Rust, where the Future owns the Client) — a per-call temporary
Client would be destroyed right after submit and cancel the request. The
`Client::get/post/...` convenience methods provide the same one-liner
ergonomics without global state. Revisit if a global default Client (with
connection pooling) is wanted.
#### Scenario: `http::get` one-liner against local server

- **WHEN** `co_await xpp::http::get("http://127.0.0.1:<port>/")` is executed against a local listener
- **THEN** the returned `Promise` resolves to `Ok(Response)` with `status() == StatusCode::Ok`

#### Scenario: URL overloads compile

- **WHEN** `http::get(String("..."))`, `http::get("...")`, and (when `__cpp_lib_string_view` is defined) `http::get(std::string_view("..."))` are compiled
- **THEN** all three compile successfully

### Requirement: Push→Pull bridge via mpsc channel

The `Client::send` implementation SHALL bridge `libx/x/http/`'s push-based C callbacks (`on_response`, `on_data`, `on_done`) to the pull-based `Body` via an `xpp::sync::mpsc::channel<Bytes>` with bounded capacity 256. When the channel is full, the `on_data` callback SHALL return `1` (pause) — libx buffers the chunk and pauses the transfer via `curl_easy_pause`; the `Body`'s drain hook SHALL call `xHttpClientResume` when the consumer frees a slot. When `on_done` is invoked, the channel SHALL be closed, causing subsequent `Body::read()` calls to return `0` (EOF) — or `-1` followed by an `Err` from `bytes()` if the shared transfer-error flag was set (mid-body failure).

#### Scenario: Backpressure pauses the sender

- **WHEN** a `Client::send` is in-flight, the consumer is not calling `Body::read`, and `libx` has pushed 256 chunks
- **THEN** the next `on_data` callback returns `1`, `libx` pauses the transfer (buffering the chunk without loss), and `xHttpClientResume` unpauses once the consumer drains the channel

#### Scenario: Mid-body failure surfaces on the body read

- **WHEN** response headers arrived (`send()` resolved `Ok`) but the connection is reset before the full body is received
- **THEN** `bytes()` returns `Err` (not a truncated `Ok`)

#### Scenario: Channel close triggers EOF

- **WHEN** `libx` invokes `on_done` and the consumer calls `Body::read(buf, 1024)` after all buffered chunks are drained
- **THEN** `Body::read` returns `0` (EOF)

### Requirement: No synchronous blocking in async path

The `Body::read`, `Body::bytes`, `Body::text`, `Response::bytes`, `Response::text`, `Client::send`, and the `Client` convenience methods (`get/post/...`) SHALL return `Promise` and SHALL NOT perform any blocking syscall or blocking spin-wait on the calling thread. Internal suspension SHALL go through `xpp::sync::mpsc::Receiver::recv()` which integrates with the EventLoop waker.

#### Scenario: `Body::read` suspends when channel is empty

- **WHEN** a Channel `Body` has no buffered chunks and the sender has not yet closed, and `Body::read(buf, 1024)` is called
- **THEN** the returned `Promise` is `Pending`, the calling fiber is suspended, and the EventLoop is free to process other work

### Requirement: `xpp::http::test::TestServer` for client integration tests

The library SHALL provide a test-only `xpp::http::test::TestServer` class in `libxpp/xpp/http/test_server.h` (under `namespace xpp::http::test`, NOT part of the public API). `TestServer::start(TestResponseSpec)` SHALL bind a `xpp::net::TcpListener` on loopback port 0 (kernel-assigned, race-free — NOT using `get_free_port()`), spawn an accept fiber on the current `EventLoop` that reads each incoming HTTP request up to the blank-line terminator (`\r\n\r\n`) and drains any `Content-Length` body bytes, optionally sleeps for `TestResponseSpec::delay_ms` milliseconds via `co_await xpp::after(delay_ms)`, then writes back a preset HTTP/1.1 response constructed from `TestResponseSpec::status`, `headers`, and `body`, then closes the connection. `TestServer::port()` SHALL return the actual bound port (read from `TcpListener::local_addr()` after bind). `TestServer::stop()` SHALL close the listener and join the accept fiber; the destructor SHALL call `stop()`. `TestServer` SHALL NOT support concurrent connections, dynamic routing, or body streaming — it is a static-response test fixture, not a general HTTP server.

#### Scenario: `TestServer` responds to a `Client::send` request

- **WHEN** a `TestServer` is started with `TestResponseSpec{StatusCode::Ok, {{"Content-Type","text/plain"}}, Bytes::from("hello"), 0}` and `Client::builder().build().unwrap().get(("http://127.0.0.1:" + port + "/").c_str()).await()` is executed
- **THEN** the returned `Promise` resolves to `Ok(Response)` with `status() == StatusCode::Ok`, `headers().get("content-type")` returning `Some("text/plain")`, and `bytes()` resolving to `Ok(Bytes)` equal to `"hello"`

#### Scenario: `TestServer::delay_ms` triggers client timeout

- **WHEN** a `TestServer` is started with `TestResponseSpec{StatusCode::Ok, {}, Bytes::empty(), 100}` (100ms pre-response delay) and a `Client` configured with `timeout(1)` (1ms) calls `get` against it
- **THEN** the returned `Promise` resolves to `Err` with `Error::is_timeout()` returning `true`

#### Scenario: `TestServer` is not part of the public API

- **WHEN** a user includes `xpp/http/client.h` (and friends) but not `xpp/http/test_server.h`
- **THEN** the names `xpp::http::test::TestServer` and `xpp::http::test::TestResponseSpec` are not visible, and the build does not pull in the libx C server/network headers transitively

#### Scenario: `TestServer` destructor cleans up the listener

- **WHEN** a `TestServer` instance goes out of scope without an explicit `stop()` call
- **THEN** the bound listening socket is closed and the accept fiber has terminated before the destructor returns

#### Scenario: `TestServer` binds to port 0 (no `get_free_port()`)

- **WHEN** `TestServer::start(spec)` is called
- **THEN** the underlying `TcpListener` is bound to `127.0.0.1:0` (kernel-assigned port) and `port()` returns the actual port assigned by the kernel (read from `TcpListener::local_addr()`), with no race window between port selection and bind
