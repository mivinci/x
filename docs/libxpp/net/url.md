# URL

## Introduction

`xpp::net::Url` is an RAII wrapper around libx's `xUrl`. Parsing returns `Result<Url, UrlParseError>` — explicit error handling, no exceptions.

```cpp
#include <xpp/net/url.h>

auto r = xpp::net::Url::parse("https://example.com:8080/path?q=1");
if (r.is_ok()) {
    auto u = std::move(r).unwrap();
    u.scheme();   // "https"
    u.host();     // "example.com"
    u.port_num(); // 8080
    u.path();     // "/path"
}
```

## API Reference

### Url

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `parse(raw)` | `Result<Url, UrlParseError>` | Parse a URL string |
| `parse(std::string)` | `Result<Url, UrlParseError>` | Parse from std::string |
| `scheme()` | `std::string` | e.g. "https" |
| `host()` | `std::string` | e.g. "example.com" |
| `port_num()` | `uint16_t` | Explicit or scheme default (http=80, https=443) |
| `path()` | `std::string` | e.g. "/api" |
| `query()` | `std::string` | e.g. "q=1" |
| `fragment()` | `std::string` | e.g. "section1" |
| `userinfo()` | `std::string` | e.g. "user:pass" |
| `raw()` | `const xUrl&` | Underlying libx handle |

### UrlParseError

| Variant | Meaning |
| --------- | --------- |
| `Empty` | Input was NULL or empty |
| `InvalidFormat` | Not a valid URL (missing scheme, host, or malformed) |

## How it works

`Url::parse()` calls `xUrlParse` which makes an internal copy of the input string. All accessor fields point into this copy. `~Url()` calls `xUrlFree` to release it.

The error type is a dedicated enum (`UrlParseError`) rather than the generic `xErrno` — matching the pattern of `AddrParseError` in `addr.h`.

## Usage Examples

### Parse and inspect

```cpp
auto r = xpp::net::Url::parse("http://localhost:3000/api");
if (r.is_ok()) {
    auto u = std::move(r).unwrap();
    EXPECT_EQ(u.scheme(), "http");
    EXPECT_EQ(u.host(), "localhost");
    EXPECT_EQ(u.port_num(), 3000);
    EXPECT_EQ(u.path(), "/api");
}
```

### Default port

`port_num()` returns the explicit port if present, otherwise the scheme default:

```cpp
xpp::net::Url::parse("http://localhost/api").unwrap().port_num();   // 80
xpp::net::Url::parse("https://localhost/api").unwrap().port_num();  // 443
```

### Error handling

```cpp
auto r = xpp::net::Url::parse("not a url");
if (r.is_err()) {
    auto e = r.unwrap_err();  // UrlParseError::InvalidFormat
    printf("parse failed: %s\n", xpp::net::url_error_message(e));
}
```
