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
  return Response::ok(id + " / " + post);   // "42 / 7" for /users/42/posts/7
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
// Sync (either standard):
.route("GET /ok", [](Request req) { return Response::ok("fine"); })
```

**C++20 — coroutine:**

```cpp
// Async — reads the request body, then responds:
.route("POST /echo", [](Request req) -> Promise<Result<Response>> {
  auto body = co_await req.into_body().bytes();
  return Response::ok(body.unwrap());
})
```

**C++11 — `.then()` chain:**

```cpp
// Async — same route, promise composition instead of a coroutine:
.route("POST /echo", [](Request req) -> Promise<Result<Response>> {
  return req.into_body().bytes().then([](Result<Bytes> b) {
    return Response::ok(b.unwrap());
  });
})

// Timed work chains the same way:
.route("GET /slow", [](Request req) -> Promise<Result<Response>> {
  return xpp::after(50).then([]() { return Response::ok("done"); });
})
```

Handler errors: returning `Err` answers **500**. Unmatched routes answer **404** (libx). A request body that overflows the channel answers **413**.

## Request Body

The request `Body` is a channel fed by libx's `on_data` callback — streamed with backpressure, no full-buffering:

**C++20 — coroutine:**

```cpp
.route("POST /sum", [](Request req) -> Promise<Result<Response>> {
  auto bytes = co_await req.into_body().bytes();   // or .text(), or read() in a loop
  auto n = bytes_to_sum(bytes);
  return Response::ok(std::to_string(n));
})
```

**C++11 — `.then()` chain:**

```cpp
.route("POST /sum", [](Request req) -> Promise<Result<Response>> {
  return req.into_body().bytes().then([](Result<Bytes> b) {
    return Response::ok(std::to_string(bytes_to_sum(b.unwrap())));
  });
})
```

The channel has a fixed capacity (256 chunks); when full, libx pauses the connection and resumes once the consumer drains.

## Streaming Responses

Return a **channel-backed body** and the server streams it out via `xHttpCtxWrite` (close-delimited in HTTP/1; nghttp2 streams in H2):

**C++20 — coroutine producer** (pass the lambda directly to `spawn` — see the lifetime note below):

```cpp
#include <xpp/http/body.h>
#include <xpp/sync/mpsc.h>

.route("GET /countdown", [](Request req) -> Result<Response> {
  auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(4);
  xpp::spawn([tx]() mutable -> Promise<void> {
    for (int i = 3; i > 0; --i) co_await tx.send(xpp::Bytes::copy(std::to_string(i).c_str(), 1));
    tx.close();
    co_return;
  });
  auto body = Body::from_channel(std::move(rx));
  return Response::ok(std::move(body));
})
```

**C++11 — recursive `.then()` producer:**

```cpp
// A struct whose operator() sends one chunk and chains itself for the
// next — the .then()-era equivalent of a coroutine loop.
struct Countdown {
  xpp::sync::mpsc::Sender<xpp::Bytes> tx;
  int i = 3;

  xpp::Promise<void> operator()() {
    if (i == 0) {
      tx.close();                  // EOF for the reader
      return xpp::resolve();
    }
    return tx.send(xpp::Bytes::copy(std::to_string(i).c_str(), 1)).then([this]() {
      --i;
      return (*this)();
    });
  }
};

.route("GET /countdown", [](Request req) -> Result<Response> {
  auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(4);
  xpp::spawn(Countdown{tx});       // defer node keeps a heap copy alive
                                   // for the whole chain
  auto body = Body::from_channel(std::move(rx));
  return Response::ok(std::move(body));
})
```

The stream ends when the channel closes. The write loop drops remaining chunks if the server is destroyed mid-stream or the connection dies.

> **Coroutine-lambda lifetime**: a lambda coroutine's frame stores the closure *pointer*, not a copy. Pass the lambda **directly** to `xpp::spawn(...)` (the defer node keeps a heap copy for the chain's lifetime), use a named coroutine function, or keep the closure alive where it is declared. `auto make = [&]{...}; spawn(make());` on a dying stack frame crashes. See `issues/coro-nested-spawn-capture-lambda-crash.md`.

## Router & Middleware (tower/axum-aligned)

Routing, path parameters, 404/405, and middleware live in the composable
`Router` (`<xpp/http/router.h>`). A Router is itself a handler — hand one to
`.router(...)`, nest it under a prefix, or call it directly in tests:

```cpp
Router r;
r.route("GET /users/:id", [](Request req, String id) { return Response::ok(id); })
  .route("/health", [](Request) { return Response::ok("fine"); });

Router api;
api.route("/users/:id", handler);
r.nest("/api", std::move(api));   // strips "/api" — sub-router is prefix-unaware

auto server = Server::builder()
                .router(std::move(r))
                .bind("127.0.0.1", 8080)
                .build()
                .unwrap();
```

`ServerBuilder::route()` registers into the builder's internal Router;
`layer()` adds middleware to it.

### Middleware

A middleware is `Handler -> Handler` where the unified Handler is
`Request -> Promise<Result<Response>>` (hyper's `Service::call` shape).
Registration order follows tower's ServiceBuilder: **the first layer
registered is the outermost**.

**C++20 — coroutine layer:**

```cpp
auto logging = [](Router::HandlerFn next) -> Router::HandlerFn {
  return [next](Request req) -> Promise<Result<Response>> {
    XLOG_INFO("-> {} {}", to_string(req.method()), req.url());
    auto r = co_await next(std::move(req));
    co_return r;
  };
};
```

**C++11 — `.then()` layer:**

```cpp
auto tag = [](Router::HandlerFn next) -> Router::HandlerFn {
  return [next](Request req) -> Promise<Result<Response>> {
    return next(std::move(req)).then([](Result<Response> r) {
      return xpp::resolve(std::move(r));
    });
  };
};
```

Layers run after matching — path parameters are readable via
`req.param("id")` — and can short-circuit (return without calling `next`).
`nest()` freezes the sub-router and bakes its layers into its routes
(sub layers inner, outer router's layers outer).

### Fallback & status answers

- No route matches the path → the `fallback(h)` handler, or **404** by default
- Path matches a pattern but the method doesn't → **405**
- A Router with no routes at all still answers 404

Standalone unit-testing: call the Router directly — no sockets:

```cpp
Router r;
r.route("GET /a", [](Request) { return Response::ok("a"); });
auto resp = r(Request::builder().method(Method::Get).url("/a").body().unwrap()).await();
```

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
