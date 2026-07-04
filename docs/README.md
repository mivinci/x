# x

Foundation libraries for C and C++ — event loop, async I/O, and Rust-style C++ bindings.

## Libraries

| Library | Language | Description |
| --------- | ---------- | ------------- |
| **[libx](libx/README.md)** | C99 | Event loop, timers, async I/O, HTTP, WebSocket, WebRTC, DNS |
| **[libxpp](libxpp/promise.md)** | C++11 | Rust-style bindings on top of libx: `Promise<T>`, `Timer`, `Own<T>`, `Option<T>` |

## Quick start

```bash
# macOS
brew install googletest llhttp nghttp2 libusrsctp

# Linux
sudo apt-get install cmake libgtest-dev libnghttp2-dev libssl-dev libusrsctp-dev
```

```bash
cmake -B build -G Ninja && cmake --build build -j
cd build && ctest --output-on-failure
```

## License

MIT
