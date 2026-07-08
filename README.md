# <p align="center"><img src="docs/logo.png" alt="x" width="160"></p>

<p align="center">
  Foundation libraries for programming in C/C++ —<br>
  event loop, async I/O, channels, TLS, fibers, and more.
</p>

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>(8);

// Consumer fiber — drains the channel, prints sum
auto consumer = xpp::fiber([rx = std::move(rx)]() mutable {
  int sum = 0;
  while (true) {
    auto val = rx.recv().await();      // suspends until data arrives
    if (val.is_none()) break;          // channel closed — all done
    sum += val.unwrap();
  }
  printf("sum: %d\n", sum);            // 210
});

// Two producer fibers send concurrently over the same channel
auto p1 = xpp::fiber([tx]() {
  for (int i =  1; i <= 10; i++) tx.send(i).await();
});
auto p2 = xpp::fiber([tx]() {
  for (int i = 11; i <= 20; i++) tx.send(i).await();
});

// Wait for producers, then signal consumer
xpp::all(std::move(p1), std::move(p2))
    .then([tx = std::move(tx)]() mutable { tx.close(); })
    .await();

consumer.await();
```

`.await()` suspends the fiber so the event loop keeps churning — and it works in C++11.  
Prefer `co_await`?  Prefer `.then()` chains?  Same `Promise<T>`, your call.

## Libraries

| Library | Language | Description |
|---------|----------|-------------|
| **libxpp** | C++11 | Stackful fibers, `Promise<T>`, channels, I/O combinators, smart pointers |
| **libx** | C99 | Event loop, async I/O, HTTP/2, WebSocket, WebRTC, P2P, DNS, filesystem |

## Quick Start

```bash
# macOS
brew install googletest llhttp nghttp2 libusrsctp

# Linux
sudo apt install cmake libgtest-dev libnghttp2-dev libssl-dev libusrsctp-dev
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
| `XPP_MT` | `ON` | Multi-threading support (`Shared` → `Arc`, atomic channels) |
| `XPP_FIBER` | `ON` | Stackful fiber scheduling for `Promise::await()` |
| `CMAKE_CXX_STANDARD` | `20` | C++ standard (`11` for pure callback/fiber, `20` for `co_await`) |

## Documentation

See [GitHub Pages](https://mivinci.github.io/x/) for module-level API reference.

## License

MIT
