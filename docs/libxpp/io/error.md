# I/O Error

## Introduction

`xpp::io::Error` is a structured I/O error type, packed into 4 bytes. It mirrors Rust's `std::io::Error` — a categorical `ErrorKind` plus an optional OS errno or libx `xErrno`.

```cpp
#include <xpp/io/error.h>

xpp::io::Error err = xpp::io::Error::from_errno(EADDRINUSE);
if (err.kind() == xpp::io::ErrorKind::AddrInUse) {
    // handle address-in-use
}
printf("%s\n", err.message());  // "Address already in use"
```

## API Reference

### ErrorKind

| Variant | Meaning |
| --------- | --------- |
| `InvalidInput` | Malformed input (bad address string, NULL arg) |
| `HostNotFound` | DNS resolution returned no results |
| `AddrInUse` | EADDRINUSE |
| `AddrNotAvailable` | EADDRNOTAVAIL |
| `PermissionDenied` | EACCES |
| `ConnectionRefused` | ECONNREFUSED |
| `ConnectionReset` | ECONNRESET |
| `BrokenPipe` | EPIPE |
| `TimedOut` | ETIMEDOUT |
| `Other` | Other syscall error |

### Error

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `kind()` | `ErrorKind` | Categorical kind |
| `raw_os_error()` | `int` | OS errno, or 0 if not from a syscall |
| `raw_xerrno()` | `xErrno` | libx error code, or `xErrno_Ok` if not from libx |
| `message()` | `const char*` | Human-readable (strerror or kind description) |
| `from_errno(e)` | `Error` | Construct from OS errno |
| `from_kind(k)` | `Error` | Construct from ErrorKind |
| `from_xerrno(e)` | `Error` | Construct from libx xErrno |

### io::Result\<T\>

```cpp
template <class T> using Result = xpp::Result<T, Error>;
```

Convenience alias — mirrors Rust's `std::io::Result<T>`.

## Encoding

`sizeof(io::Error) == 4` (one `int32_t`). Three sources are distinguishable via bit patterns:

| `m_code` range | Source | Accessor |
| ---------------- | -------- | ---------- |
| `0x00000001 .. 0x3FFFFFFF` | OS errno | `raw_os_error()` |
| `0x40000000 .. 0x7FFFFFFF` | libx xErrno (bit 30 set) | `raw_xerrno()` |
| `0x80000000 .. 0xFFFFFFFF` | Custom ErrorKind (negative) | `kind()` |
| `0x00000000` | Niche (Ok sentinel) | — |

Bit 30 (`0x40000000`) is the xErrno flag. Errno and xErrno values are both small (< 256). Bit 31 (sign) separates custom kinds.

## Usage Examples

### From errno

```cpp
xpp::io::Error err = xpp::io::Error::from_errno(ECONNREFUSED);
err.kind();          // ErrorKind::ConnectionRefused
err.raw_os_error();  // ECONNREFUSED (61 on macOS)
err.message();       // "Connection refused"
```

### From xErrno

```cpp
xErrno libx_err = xErrno_DnsNotFound;
xpp::io::Error err = xpp::io::Error::from_xerrno(libx_err);
err.kind();          // ErrorKind::HostNotFound
err.raw_xerrno();    // xErrno_DnsNotFound
err.raw_os_error();  // 0 (not from a syscall)
```

### From ErrorKind

```cpp
xpp::io::Error err = xpp::io::Error::from_kind(xpp::io::ErrorKind::InvalidInput);
err.kind();          // ErrorKind::InvalidInput
err.raw_os_error();  // 0
err.raw_xerrno();    // xErrno_Ok
```

### With io::Result — `.await()`

```cpp
auto r = xpp::net::UdpSocket::bind("127.0.0.1:9090").await();
if (r.is_err()) {
    auto e = r.unwrap_err();
    if (e.kind() == xpp::io::ErrorKind::AddrInUse) {
        // port already taken
    }
}
```

### With io::Result — `co_await` (C++20)

```cpp
xpp::Promise<void> bind_or_retry(const char *addr) {
    auto r = co_await xpp::net::UdpSocket::bind(addr);
    if (r.is_err()) {
        auto e = r.unwrap_err();
        if (e.kind() == xpp::io::ErrorKind::AddrInUse) {
            co_await xpp::after(1000);
        }
        co_return;
    }
    auto sock = std::move(r).unwrap();
}
```
