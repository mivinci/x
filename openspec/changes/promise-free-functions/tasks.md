## 1. Free functions

- [ ] 1.1 Add `resolve(T v)` → `Promise<T>` in `promise.h` (deduces T)
- [ ] 1.2 Add `after(uint64_t ms)` → `Promise<void>` in `promise.h`
- [ ] 1.3 Add `defer(Func fn)` → `Promise<T>` in `promise.h` (deduces T from return type)
- [ ] 1.4 Add `work(Func fn)` → `Promise<T>` in `promise.h` (deduces T from return type, uses `_::WorkAdapter`)
- [ ] 1.5 Add `adapt<T, Adapter>(args...)` → `Promise<T>` in `promise.h` (both explicit)

## 2. Remove static methods

- [ ] 2.1 Remove `Promise<T>::resolve()` declaration + definition
- [ ] 2.2 Remove `Promise<void>::after()` declaration + definition
- [ ] 2.3 Remove `Promise<T>::defer()` declaration + definition
- [ ] 2.4 Remove `Promise<T>::work()` declaration + definition
- [ ] 2.5 Remove `Promise<T>::adapt()` declaration + definition
- [ ] 2.6 Remove `Promise<void>::resolve()` (void overload)

## 3. Update tests

- [ ] 3.1 Migrate `Promise<T>::resolve(v)` → `resolve(v)` in all test files
- [ ] 3.2 Migrate `Promise<void>::after(ms)` → `after(ms)` in all test files
- [ ] 3.3 Migrate `Promise<T>::defer(fn)` → `defer(fn)` in all test files
- [ ] 3.4 Migrate `Promise<T>::work(fn)` → `work(fn)` in all test files
- [ ] 3.5 Migrate `Promise<T>::adapt<Adapter>(args)` → `adapt<T, Adapter>(args)` in all test files
- [ ] 3.6 Verify all tests pass

## 4. Update docs

- [ ] 4.1 Update `docs/libxpp/promise/README.md` — API table uses free functions
- [ ] 4.2 Update `docs/libxpp/promise/deferred.md` — examples use free functions
- [ ] 4.3 Update `docs/libxpp/promise/timers.md` — examples use free functions
- [ ] 4.4 Update `docs/libxpp/promise/combinators.md` — examples use free functions
- [ ] 4.5 Update `docs/libxpp/promise/adapter.md` — examples use free functions
- [ ] 4.6 Update `docs/libxpp/promise/coroutine.md` — examples use free functions
