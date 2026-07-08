# Error Handling

[← Promise\<T\>](README.md)

Promise chains don't have built-in exceptions or a `catch` method. Instead, errors are propagated as regular values using `Result<T, E>`. The type system tracks error states through the entire chain.

## The pattern: Result<T, E> in chains — `.await()`

```cpp
enum class DbError { ConnectionFailed, NotFound, Timeout };

auto result = fetch_user(42).await();
if (result.is_ok()) {
    auto &user = result.unwrap();
} else {
    switch (result.unwrap_err()) {
        case DbError::ConnectionFailed: /* retry */ break;
        case DbError::NotFound: /* handle */ break;
    }
}
```

## The pattern: Result<T, E> in chains — `co_await` (C++20)

```cpp
Promise<Result<User, DbError>> fetch_user(int id) {
    auto conn = co_await db_connect();
    if (!conn) co_return err(DbError::ConnectionFailed);
    auto user = co_await conn.query(id);
    if (!user) co_return err(DbError::NotFound);
    co_return ok(std::move(user));
}

auto result = fetch_user(42).await();
if (result.is_ok()) {
    auto &user = result.unwrap();
} else {
    switch (result.unwrap_err()) {
        case DbError::ConnectionFailed: /* retry */ break;
        case DbError::NotFound: /* handle */ break;
    }
}
```

## What if I don't use Result?

If the async operation itself can't fail (e.g., a timer, a pure computation), just return `Promise<T>` directly:

```cpp
// .await()
auto val = always_works().await();

// co_await (C++20)
Promise<int> always_works() { co_return 42; }
auto val = always_works().await();
```

## Error recovery in chains — `.await()`

```cpp
auto user = fetch_user(42)
    .then([](Result<User, DbError> r) -> User {
        if (r.is_ok()) return r.unwrap();
        return User::anonymous();
    })
    .await();
```

## Error recovery in chains — `co_await` (C++20)

```cpp
Promise<User> get_or_default(int id) {
    return fetch_user(id)
        .then([](Result<User, DbError> r) -> User {
            if (r.is_ok()) return r.unwrap();
            return User::anonymous();
        });
}
```

## try_next: sequential fallback

`try_next` tries multiple async operations in order, returning the first `Ok` result:

```cpp
Promise<Result<User, DbError>> user = xpp::try_next(42, {
    [](int id) { return fetch_from_primary(id); },
    [](int id) { return fetch_from_replica(id); },
    [](int id) { return err(DbError::NotFound); }
});
```

If `primary` succeeds → return its result. If it fails → try `replica`. If all fail → the last error.

## Error types

libxpp provides `io::Error` for I/O errors — a niche-optimized 4-byte type that distinguishes between POSIX `errno` and libx `xErrno`:

```cpp
// .await()
auto file_r = xpp::fs::File::open("config.json").await();

// co_await (C++20)
Promise<Result<std::string, io::Error>> read_file() {
    auto file = co_await File::open("config.json");
    if (!file) co_return err(file.unwrap_err());
    auto data = co_await file->read_all();
    co_return ok(std::move(data));
}
```

For domain-specific errors, define your own error enum — `Result` works with any error type.

## Comparison with other approaches

| | libxpp | Rust | JavaScript |
| --- | --- | --- | --- |
| Error type | `Result<T, E>` | `Result<T, E>` | `throw` / `catch` |
| Propagation | explicit in chain | `?` operator | implicit |
| Type safety | compile-time (template) | compile-time | runtime |
