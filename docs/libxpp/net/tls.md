# TLS

## Introduction

`xpp::net::TlsConfig` and `TlsContext` provide RAII TLS configuration. Pass a `TlsContext` to `TcpStream::connect()` to enable TLS — the handshake is transparent.

```cpp
#include <xpp/net/tls.h>

xpp::net::TlsContext ctx(xpp::net::TlsConfig::client());
auto conn = xpp::net::TcpStream::connect("example.com:443", ctx).wait();
```

## API Reference

### TlsConfig

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `client()` | `TlsConfig` | Client defaults (system CA, verify on) |
| `client_insecure()` | `TlsConfig` | Client that skips peer verification |
| `server(cert, key)` | `TlsConfig` | Server config with cert + key paths |
| `server(cert, key, ca)` | `TlsConfig` | Server config with CA (for mTLS) |
| `with_cert(path)` | `TlsConfig&` | Builder: set cert path |
| `with_key(path)` | `TlsConfig&` | Builder: set key path |
| `with_ca(path)` | `TlsConfig&` | Builder: set CA path |
| `with_key_password(pw)` | `TlsConfig&` | Builder: set key password |
| `with_alpn(protocols)` | `TlsConfig&` | Builder: set ALPN list |
| `with_skip_verify(bool)` | `TlsConfig&` | Builder: toggle verification |
| `raw()` | `const xTlsConf*` | Underlying libx config |

Each `with_*` builder has two overloads, selected by ref-qualifier:
- `with_*(...) &`  → returns `TlsConfig&`, modifies in-place (lvalue chain)
- `with_*(...) &&` → returns `TlsConfig&&`, enables move (rvalue chain)

```cpp
// Rvalue chain: temporary factory → && overloads → move at end
auto conf = TlsConfig::client().with_cert(...).with_key(...);

// Lvalue chain: named variable → & overloads → modify in-place
TlsConfig conf = TlsConfig::client();
conf.with_ca("/custom/ca.pem").with_alpn({"h2", "http/1.1"});
```

### TlsContext

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `TlsContext(conf)` | `TlsContext` | Create context from TlsConfig |
| `TlsContext(xTlsConf*)` | `TlsContext` | Create from raw libx config |
| `reload(conf)` | `int` | Hot-reload certificates (0 = success) |
| `raw()` | `xTlsCtx` | Underlying libx context |
| `is_valid()` | `bool` | Construction succeeded |
| `operator bool()` | `bool` | Same as `is_valid()` |

## How it works

`TlsConfig` is a builder that owns the string storage (cert path, key path, CA path, key password, ALPN protocols). It wraps `xTlsConf` (a POD of pointers) — the strings stay valid for the config's lifetime.

`TlsContext` calls `xTlsCtxCreate` in its constructor and `xTlsCtxDestroy` in its destructor. The mode (client or server) is determined automatically by libx: if both `cert` and `key` are set, server mode; otherwise client mode.

When passed to `TcpStream::connect()`, the `xTlsCtx` handle is set in `xTcpConnectConf::tls_ctx`. libx's `xTcpConnect` does the TLS handshake transparently — the resulting `TcpStream` encrypts/decrypts via `recv`/`send` as usual.

## Usage Examples

### Client with system CA

```cpp
xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
auto conn = xpp::net::TcpStream::connect("example.com:443", tls).wait();
conn.write("GET / HTTP/1.0\r\n\r\n", 18).wait();
```

### Server with certificate

```cpp
xpp::net::TlsContext tls(xpp::net::TlsConfig::server("cert.pem", "key.pem"));
// Pass to TcpListener::bind via xTcpListenerConf (not yet wrapped)
```

### Builder pattern

```cpp
xpp::net::TlsConfig conf = xpp::net::TlsConfig::client().with_ca("/custom/ca.pem").with_alpn({"h2", "http/1.1"});
xpp::net::TlsContext ctx(conf);
```

### mTLS (mutual TLS)

```cpp
// Server verifies client certificates
xpp::net::TlsContext server_tls(
    xpp::net::TlsConfig::server("server.pem", "server.key", "ca.pem"));

// Client presents its certificate
xpp::net::TlsConfig client_conf = xpp::net::TlsConfig::client().with_cert("client.pem").with_key("client.key");
xpp::net::TlsContext client_tls(client_conf);
```

## Coroutine Examples

### HTTPS client

```cpp
xpp::Promise<void> https_fetch() {
    xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
    auto conn = co_await xpp::net::TcpStream::connect("example.com:443", tls);

    co_await conn.write("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n", 40);

    char buf[4096];
    ssize_t n = co_await conn.read(buf, sizeof(buf));
    printf("%.*s\n", (int)n, buf);
}
```

### Connect with error handling

```cpp
xpp::Promise<void> connect_or_fallback() {
    xpp::net::TlsContext tls(xpp::net::TlsConfig::client());
    auto conn = co_await xpp::net::TcpStream::connect("example.com:443", tls);
    if (!conn.is_open()) {
        // connect failed — try fallback
        co_return;
    }
    co_await conn.write("hello", 5);
}
```
