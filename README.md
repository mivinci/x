# x

Foundation libraries for programming in C/C++.

## Libraries

| Library | Description |
|---------|-------------|
| **libx** | Event loop, async I/O, HTTP/2, WebSocket, WebRTC — all in C99 |
| **libxpp** | Rust-style C++ bindings: Promise\<T\>, coroutines, channels, async I/O |

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
