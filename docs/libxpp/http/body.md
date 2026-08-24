# Body

[← HTTP](README.md)

`xpp::http::Body` is the request/response body value — move-only, hyper-style. It satisfies the `AsyncReader` concept, so `io::read_all` / `io::copy` work on it directly.

## Kinds

A `Body` is one of three kinds:

| Kind | Construction | Read behavior |
| --- | --- | --- |
| `Empty` | `Body::empty()` | immediate EOF (`read()` returns 0) |
| `Once` | `Body::from(bytes / Vec<uint8_t> / String / const char*)` | one-shot bytes |
| `Channel` | `Body::from_channel(mpsc::Receiver<Bytes>)` | streamed — `read()` suspends when the channel is empty, resumes on wake |

```cpp
Body a = Body::empty();
Body b = Body::from(xpp::Bytes::copy("hi", 2));
Body c = Body::from(std::string("text"));

auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(64);
Body s = Body::from_channel(std::move(rx));
tx.send(xpp::Bytes::copy("chunk", 5));   // feeds the stream
tx.close();                              // EOF for the reader
```

## Reading

```cpp
// Whole body:
auto bytes = body.bytes().await().unwrap();   // Promise<Result<Bytes>>
auto text  = body.text().await().unwrap();    // Promise<Result<String>> (UTF-8)

// Streaming (AsyncReader):
char buf[4096];
ssize_t n;
while ((n = body.read(buf, sizeof(buf)).await()) > 0) { /* process */ }
// n == 0 at EOF
```

For a channel body, `read()` returns a **Pending** promise when the channel is empty; the reading coroutine/fiber suspends until a chunk arrives or the sender closes. This is waker-driven — nothing busy-polls.

> **Lifetime**: `read()` does not extend the Body's lifetime. Keep the `Body` in a named variable while awaiting (unlike `bytes()`/`text()`, which move it into an `Arc` internally).

## Observers

```cpp
bool empty   = body.is_empty();     // Empty kind or exhausted
bool channel = body.is_channel();   // backed by a stream
xpp::Bytes once = body.into_once_bytes();  // Once/Empty only (channel → empty)
```

## Streaming Request Bodies

The server-side request body is a channel fed by libx's `on_data` callback — `req.into_body()` gives you a channel `Body` that streams with backpressure:

```cpp
.route("POST /echo", [](Request req) -> Promise<Result<Response>> {
  auto body = co_await req.into_body().bytes();  // full body, streamed
  return ResponseBuilder::ok(body);
})
```

The server channel has fixed capacity; when full, libx pauses the connection and resumes once you drain it (natural backpressure).

Client-side request **upload** only supports `Once` bodies today — `into_once_bytes()` returns empty for a channel body (documented limitation).

## Streaming Response Bodies

Return a channel `Body` from a handler and the server streams it:

```cpp
.route("GET /stream", [](Request req) -> Result<Response> {
  auto [tx, rx] = xpp::sync::mpsc::channel<xpp::Bytes>(4);
  xpp::spawn([tx]() mutable -> Promise<void> {
    co_await tx.send(xpp::Bytes::copy("part1", 5));
    co_await tx.send(xpp::Bytes::copy("-part2", 6));
    tx.close();
    co_return;
  });
  return ResponseBuilder().status(StatusCode::Ok)
                          .body(Body::from_channel(std::move(rx)));
})
```

Each chunk is written via `xHttpCtxWrite`; closing the channel ends the stream (`xHttpCtxEndStream`). HTTP/1 streams are close-delimited (`Connection: close`); H2 uses nghttp2 streams.

## Related

- [Client](client.md) — response bodies
- [Server](server.md) — request bodies & streaming responses
- [Channels](../channels/README.md) — mpsc
- [I/O](../io/README.md) — `io::read_all`, `io::copy`
