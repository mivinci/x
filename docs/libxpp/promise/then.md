# then() — Chaining & Auto-Flatten

[← Promise\<T\>](README.md)

`then()` is the primary chaining mechanism for `Promise<T>`. It transforms a resolved value into a new `Promise`, with automatic flattening.

## Basic chaining

```cpp
xpp::resolve(10)
    .then([](int x) { return x * 2; })   // Promise<int>
    .then([](int x) { return x + 1; })   // Promise<int>
    .then([](int x) {                     // Promise<void>
        printf("result: %d\n", x);
    });
// final chain: Promise<void>
```

Each `.then()` adds one link to the chain. The chain only starts running when the root promise (here `resolve(10)`) is polled by the event loop.

## Auto-Flatten

If a `.then()` callback returns a `Promise<U>`, the result is flattened to `Promise<U>` — not `Promise<Promise<U>>`. This is the same behavior as Rust's `Future::and_then` and JavaScript's `Promise.then`.

```cpp
Promise<User> fetch_user(int id) { /* ... */ }
Promise<Order> fetch_order(User &u) { /* ... */ }

Promise<Order> order = resolve(42)
    .then([](int id) { return fetch_user(id); })   // → Promise<User> (flattened)
    .then([](User u) { return fetch_order(u); });   // → Promise<Order> (flattened)
```

Without auto-flatten, the result would be `Promise<Promise<User>>` — requiring `.then().then()` to unwrap. With it, you write a flat chain.

## Type transformations

```cpp
Promise<std::string> msg = resolve(10)
    .then([](int x)  { return x * 2; })            // Promise<int>
    .then([](int x)  { return std::to_string(x); }); // Promise<std::string>
```

Each `.then(fn)` turns `Promise<T>` into `Promise<decltype(fn(T))>`. The compiler tracks types through the entire chain.

## Void handling

```cpp
Promise<> p = resolve(10)
    .then([](int x) { printf("%d\n", x); });
// p is Promise<void>
```

A `then()` that returns void (or `Promise<void>`) turns the rest of the chain into `Promise<void>`. This is convenient for fire-and-forget side effects at the end of a chain.

## then() is non-mutating

Each `.then()` call returns a **new** `Promise` — the original is untouched. This means you can fork a chain into multiple consumers:

```cpp
auto root     = fetch_value();
auto doubled  = root.then([](int x) { return x * 2; });
auto tripled  = root.then([](int x) { return x * 3; });
// root is unchanged; doubled and tripled are independent forks
```

## Error handling

There's no built-in `catch` method. Errors are propagated through the chain as regular values using `Result<T, E>`:

```cpp
Promise<Result<int, MyError>> compute = resolve(42)
    .then([](int x) -> Result<int, MyError> {
        if (x == 0) return err(MyError::DivideByZero);
        return ok(100 / x);
    })
    .then([](Result<int, MyError> r) {
        return r.is_ok() ? r.unwrap() * 2 : 0;
    });
```

## Arenas: allocation model

Each `.then()` chain shares a 256-byte bump allocator (arena). Promise nodes are allocated from this arena, not individually heap-allocated. This means a 10-link chain is a single `malloc` (the arena) rather than 10 separate allocations. Nodes that overflow the arena fall back to heap transparently. The arena is freed when the chain's root promise is destroyed.

## Driving the chain

Chains are lazy ��� nothing runs until polled. Use `.await()` to drive the chain to completion on the current thread:

```cpp
int result = resolve(10)
    .then([](int x) { return x * 2; })
    .await();
// result == 20
```

## then() vs co_await

These two are equivalent:

```cpp
// C++11: then()
Promise<int> p = fetch_value()
    .then([](int x) { return x * 2; });

// C++20: co_await
Promise<int> p = async_compute();  // inside: int x = co_await fetch_value(); co_return x * 2;
```

Both produce the same `Promise<int>` backed by the same `poll()` mechanism.
