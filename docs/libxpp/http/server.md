# Server

[← HTTP](README.md)

Async HTTP server wrapping libx's `xHttpServer` (HTTP/1.1 + HTTP/2). Handlers run on the event loop via `xpp::spawn` — waker-driven, no per-request fiber stack.

## Construction

```cpp
auto server = xpp::http::Server::builder()
  .route("GET /users/:id", handler)
  .route("/health", handler)            // any method
  .idle_timeout(30000)                  // ms, 0 = no timeout (default 60000)
  .bind("127.0.0.1", 8080)              // port 0 = kernel-assigned
  .build()
  .unwrap();

auto running = server.serve();          // listens synchronously
uint16_t port = server.port();          // actual port (bind(0))
// ...
server.stop();                          // resolve serve() → connections drained
running.await();
```

`serve()` returns `Promise<Result<void>>` that resolves when `stop()` is called. Listening is synchronous, so `port()` is immediately valid.

## Routes & Path Parameters

Pattern is `"METHOD /path"` (or `/path` for any method). `:name` segments become handler **arguments**, injected in pattern order, as `xpp::String`:

```cpp
.route("GET /users/:id/posts/:post", [](Request req, String id, String post) {
  return ResponseBuilder::ok(id + " / " + post);   // "42 / 7" for /users/42/posts/7
})
```

The handler parameter count is checked at registration (`XPP_ASSERT`).

## Handler Signatures

A handler takes `Request` **by value** (hyper style — move-in) plus the injected parameters, and returns one of:

| Return type | Meaning |
| --- | --- |
| `Response` | synchronous response |
| `Result<Response>` | synchronous, fallible (Err → 500) |
| `Promise<Result<Response>>` | async handler (`.then()` chain, or a `co_await` coroutine) |

```cpp
// Sync:
.route("GET /ok", [](Request req) { return ResponseBuilder::ok("fine"); })

// Async (C++20 coroutine) — reads the request body, then responds:
.route("POST /echo", [](Request req) -> Promise<Result<Response>> {
  auto body = co_await req.into_body().bytes();
  return ResponseBuilder::ok(body.unwrap());
})

// Async (C++11 .then() chain):
.route("GET /slow", [](Request req) -> Promise<Result<Response>> {
  return xpp::after(50).then([]() { return ResponseBuilder::ok("done"); });
})
```

Handler errors: returning `Err` answers **500**. Unmatched routes answer **404** (libx). A request body that overflows the channel answers **413**.

## Request Body

The request `Body` is a channel fed by libx's `on_data` callback — streamed with backpressure, no full-buffering:

```cpp
.route("POST /sum", [](Request req) -> Promise<Result<Response>> {
  auto bytes = co_await req.into_body().bytes();   // or .text(), or read() in a loop
  auto n = bytes_to_sum(bytes);
  return ResponseBuilder::ok(std::to_string(n));
})
```

The channel has a fixed capacity (256 chunks); when full, libx pauses the connection and resumes once the consumer drains.

## Streaming Responses

Return a **channel-backed body** and the server streams it out via `xHttpCtxWrite` (close-delimited in HTTP/1; nghttp2 streams in H2):

```cpp
#include <xpp/http/body.h>
#include <xpp/sync/mpsc.h>

.route("GET /countdown", [](Request req) -> Result<Response> {
  auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(4);
  // Producer — spawn a coroutine lambda (closure copied to the heap):
  xpp::spawn([tx]() mutable -> Promise<void> {
    for (int i = 3; i > 0; --i) co_await tx.send(xpp::Bytes::copy(std::to_string(i).c_str(), 1));
    tx.close();
    co_return;
  });
  auto body = Body::from_channel(std::move(rx));
  return ResponseBuilder().status(StatusCode::Ok).body(std::move(body));
})
```

The stream ends when the channel closes. The write loop drops remaining chunks if the server is destroyed mid-stream or the connection dies.

> **Coroutine-lambda lifetime**: a lambda coroutine's frame stores the closure *pointer*, not a copy. Pass the lambda **directly** to `xpp::spawn(...)` (the defer node keeps a heap copy for the chain's lifetime), use a named coroutine function, or keep the closure alive where it is declared. `auto make = [&]{...}; spawn(make());` on a dying stack frame crashes. See `issues/coro-nested-spawn-capture-lambda-crash.md`.

## Concurrency

Requests on the same route run concurrently — per-request state is stored via `xHttpCtxSetUser` (user pointer delivered to `on_data`/`on_done`), so streaming bodies of simultaneous requests stay separate. Test `ConcurrentBodiesStaySeparate` covers this.

## Lifecycle & Safety

- `Server` is move-only; the destructor tears down the C server.
- **In-flight handlers**: a handler may complete *after* the `Server` is destroyed. The spawn chain captures a `ServerLifetime` Arc and drops the response write instead of touching the freed `ctx` (test `DestroyWithInflightHandlerDoesNotCrash`).
- `serve()` blocks nothing — run the loop as usual; `stop()` resolves it.

## Related

- [Body](body.md) — request/response body, channels, streaming
- [Client](client.md)
- [libx HTTP Server (C API)](../../../libx/http/server.md)
