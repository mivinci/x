# <p align="center"><img src="docs/logo.png" alt="x" width="160"></p>

<p align="center">
  Foundation libraries for programming in C/C++ —<br>
  event loop, async I/O, channels, TLS, fibers, and more.
</p>

```cpp
// libxpp — Rust-style async in C++: no callback hell, no manual state machines.

xpp::fiber([]() {
  auto listener = xpp::net::TcpListener::bind("0.0.0.0:8080").await().unwrap();
  auto [conn, addr] = listener.accept().await();

  char buf[1024];
  while (true) {
    ssize_t n = conn.read(buf, sizeof(buf)).await();
    if (n <= 0) break;
    conn.write(buf, n).await();
  }
}).then([]() { printf("server done\n"); });

// .await() suspends the fiber — the event loop keeps churning.
// Works in C++11.  Also supports co_await (C++20) and .then() chains.
// Same Promise<T> every time.  Choose your style, not a different library.
```

## Libraries

| Library | Language | Description |
|---------|----------|-------------|
| **libx** | C99 | Event loop, async I/O, HTTP/2, WebSocket, WebRTC, P2P, DNS, filesystem |
| **libxpp** | C++11 | Stackful fibers, `Promise<T>`, channels, I/O combinators, smart pointers |

## Quick Start

```bash
# macOS
brew install googletest llhttp nghttp2 libusrsctp

# Linux
sudo apt-get install cmake libgtest-dev libnghttp2-dev libssl-dev libusrsctp-dev
```

```bash
cmake -B build && cmake --build build -j
cd build && ctest
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `X_BUILD_STATIC` | `OFF` | Build static libraries |
| `X_BUILD_TESTS` | `ON` | Build tests |
| `X_TLS_BACKEND` | `auto` | TLS backend: `auto`, `openssl`, `mbedtls`, `none` |
| `X_DEBUG_LEVEL` | `0` | Debug log verbosity (0–3) |

## Documentation

See [GitHub Pages](https://mivinci.github.io/x/) for module-level API reference.

## License

MIT
