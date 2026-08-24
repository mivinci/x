# Client

[← HTTP](README.md)

Async HTTP client wrapping libx's `xHttpClient` (libcurl). Promise-based, hyper/reqwest-shaped.

## Construction

```cpp
auto client = xpp::http::Client::builder()
  .timeout(5000)          // total request timeout, ms (default: no timeout?)
  .connect_timeout(3000)  // TCP/TLS connect timeout
  .read_timeout(5000)     // read timeout
  .redirect(3)            // follow up to 3 redirects
  .user_agent("xpp/1.0")
  .proxy("http://proxy:8080")   // optional
  .header("Accept-Encoding", "identity")
  .build()
  .unwrap();
```

Builder options: `timeout`, `connect_timeout`, `read_timeout`, `redirect`, `max_redirects`, `user_agent`, `proxy`, `no_proxy`, `header` (default request headers).

## Making Requests

```cpp
// Convenience methods — URL as String, const char*, or std::string_view:
auto r  = client.get("https://example.com/a").await();        // Promise<Result<Response>>
auto p  = client.post("https://example.com/upload", "payload").await();
auto d  = client.del("https://example.com/items/7").await();
auto h  = client.head("https://example.com").await();

// Full control via Request:
auto req = xpp::http::Request::builder()
  .method(xpp::http::Method::Post)
  .url("https://example.com/api")
  .header("Content-Type", "application/json")
  .body(R"({"k":"v"})")
  .build()
  .unwrap();
auto r = client.send(std::move(req)).await();
```

Each call returns `Promise<Result<Response, http::Error>>`:

- `Ok(Response)` — headers arrived. The body is **streamed** via an mpsc channel with backpressure; read it with `bytes()` / `text()` / `body().read()`. A transfer failure mid-body surfaces as a read error, not a truncated EOF.
- `Err(http::Error)` — transport failure before headers (connect/DNS/timeout), **or** a 4xx/5xx status (a `Protocol` error whose `status()` carries the code).

## Reading the Response

```cpp
auto resp = r.unwrap();
uint16_t code = resp.status_code();
auto headers  = resp.headers();          // HeaderMap (case-insensitive keys)
auto final    = resp.final_url();        // Some(url) after redirects

// Whole body at once:
auto bytes = resp.bytes().await().unwrap();   // Promise<Result<Bytes>>
auto text  = resp.text().await().unwrap();    // Promise<Result<String>> (UTF-8)

// Or stream it:
auto body = resp.into_body();
char buf[4096];
ssize_t n;
while ((n = body.read(buf, sizeof(buf)).await()) > 0) { /* process */ }
```

`Response` is move-only (hyper style). `bytes()`/`text()` consume the body; `read()` follows the `AsyncReader` concept so `io::read_all` / `io::copy` work directly.

## Request Bodies

```cpp
// Once bodies — bytes / Vec<uint8_t> / String / const char*:
.builder().body("text payload")
.builder().body(xpp::Bytes::copy(data, len))
```

Channel (streaming) request bodies are not yet supported for upload — `into_once_bytes()` returns empty for a channel body. This is a documented limitation.

## Error Types

`http::Error` carries a `Kind` (`Connect`, `Dns`, `Timeout`, `Protocol`, `Body`, `Io`, …) and a message. `Protocol` errors additionally carry the HTTP status (`error.status()`). See [error.h](../../../libxpp/xpp/http/error.h) for the full list.

## Related

- [Body](body.md) — reading and streaming
- [Server](server.md) — the other half
- [libx HTTP Client (C API)](../../../libx/http/client.md)
