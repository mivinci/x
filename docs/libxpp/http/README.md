# xpp::http

HTTP client and server for libxpp — `Client` / `Server`, async-first, wrapping libx's `xHttpClient` / `xHttpServer` C APIs.

- [Client](client.md)
- [Server](server.md)
- [Body](body.md)

## Introduction

`xpp::http` provides a hyper/reqwest-shaped HTTP stack: a `Client` with `get`/`post`/`put`/`del`/`patch`/`head` conveniences, and a `Server` with template-injected path parameters and async handlers. Everything is Promise-based — no blocking I/O in the async path.

Header-only, C++11-compatible, with C++20 coroutine sugar (`co_await` / `co_return`) when `XPP_HAS_COROUTINES` is defined. Errors flow through `Result<T, http::Error>` — no exceptions.

## Client at a Glance

**C++20 — coroutines:**

```cpp
#include <xpp/http/client.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto client = xpp::http::Client::builder().timeout(5000).build().unwrap();

xpp::Promise<void> fetch() {
  auto r = co_await client.get("https://example.com/api");
  if (r.is_err()) { /* transport failure or 4xx/5xx */ co_return; }
  auto resp = std::move(r).unwrap();
  auto body = co_await resp.bytes();
  // ...
  co_return;
}
// Drive it — either parks the caller (running the loop) or fire-and-forget:
fetch(client).await();
xpp::spawn(fetch(client));
```

**C++11 — `.await()`** (parks the caller, driving the event loop):

```cpp
#include <xpp/http/client.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto client = xpp::http::Client::builder().timeout(5000).build().unwrap();

auto r = client.get("https://example.com/api").await();
if (r.is_err()) { /* transport failure or 4xx/5xx */ }
auto resp = r.unwrap();
auto body = resp.bytes().await().unwrap();
```

The response `Body` is streamed through an mpsc channel with backpressure — read it via `bytes()`, `text()`, or `body().read()`. 4xx/5xx statuses surface as `Err(Error)` (a `Protocol` error carrying the status code).

## Server at a Glance

```cpp
#include <xpp/http/server.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto server = xpp::http::Server::builder()
  .route("GET /users/:id", [](xpp::http::Request req, xpp::String id) {
    return xpp::http::ResponseBuilder::ok(id);
  })
  .bind("127.0.0.1", 8080)   // port 0 = kernel-assigned
  .build()
  .unwrap();

auto running = server.serve();   // listens synchronously; resolves on stop()
// ... on another fiber/loop iteration:
server.stop();
```

Async handlers come in two flavors — the same `POST /echo` route:

**C++20 — coroutine:**

```cpp
.route("POST /echo", [](xpp::http::Request req) -> xpp::Promise<xpp::http::Result<xpp::http::Response>> {
  auto body = co_await req.into_body().bytes();
  return xpp::http::ResponseBuilder::ok(body.unwrap());
})
```

**C++11 — `.then()` chain:**

```cpp
.route("POST /echo", [](xpp::http::Request req) -> xpp::Promise<xpp::http::Result<xpp::http::Response>> {
  return req.into_body().bytes().then([](xpp::http::Result<xpp::Bytes> b) {
    return xpp::http::ResponseBuilder::ok(b.unwrap());
  });
})
```

Handlers run on the event loop via `xpp::spawn` (waker-driven, no per-request fiber). Path parameters (`:name`) are injected as handler arguments in pattern order. A channel-backed response body is streamed out (`xHttpCtxWrite` / close-delimited in HTTP/1).

## Body Model

`xpp::http::Body` has three kinds:

| Kind | Producer | Consumer |
| --- | --- | --- |
| `Empty` | `Body::empty()` | EOF immediately |
| `Once` | `Body::from(bytes / string / Vec)` | one-shot bytes |
| `Channel` | `Body::from_channel(mpsc::Receiver<Bytes>)` | streamed, waker-driven |

See [Body](body.md) for reading, backpressure, and streaming responses.

## Error Handling

- Transport failures (connect/DNS/timeout) → `Err(Error{Kind::Connect|Dns|Timeout|…})`
- 4xx/5xx responses → `Err(Error{Kind::Protocol, status})`
- Server handler returning `Err` → 500 to the client
- Unmatched route → 404
- Request body overflow (channel full) → 413

## Related

- [libx HTTP (C API)](../../libx/http/README.md)
- [Promise / coroutines](../promise/README.md)
- [Channels](../channels/README.md)
