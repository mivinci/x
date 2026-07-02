## Context

`xTimerStart` currently has no destruction hook for `arg`. When an event
loop is destroyed with pending timers, `kq_destroy` / `epoll_destroy` /
`poll_destroy` / `wsapoll_destroy` pop each timer off the heap and call
`timer_free` on the struct — but `t->arg` is silently discarded. This
leaks any heap-allocated state the caller attached.

The parallel API `xWorkSubmit` already solves this with its `on_cancel`
parameter (`event.h:251-252`). This change brings `xTimerStart` to
parity.

**Current state:**
- 165 `xTimerStart(...)` call sites across `libx/`, `libdlproxy/`, `libxpp/`
- 38 files affected
- The signature change is breaking — every call site must be updated
- `xTimer_` struct is allocated by `timer_alloc` (already `memset` to 0)
  and freed by `timer_free` (returns to a freelist or `free()`)

## Goals / Non-Goals

**Goals:**
- Provide an `on_cancel` hook on `xTimerStart` invoked when the host
  event loop is destroyed with the timer still pending
- Maintain the existing fire-path semantics: `fn(arg)` runs exactly once
  on fire; `on_cancel` never runs on fire
- Keep `xTimerStop`'s contract unchanged: stop is user-initiated
  cancellation, ownership of `arg` returns to the caller, `on_cancel`
  is NOT invoked
- All existing tests pass on Linux + macOS, openssl + mbedtls, with
  ASan enabled (the existing 4-config CI matrix)

**Non-Goals:**
- Fixing `Promise::after()`'s use-after-free when the Promise is
  destroyed before the timer fires — that's a separate change requiring
  a `TimerPromiseNode` that owns the timer handle
- Adding a "stop callback" for user-initiated stops — explicitly out
  of scope; stop returns arg ownership to the caller
- Changing `xTimer_` struct layout in a way that affects the freelist
  or heap comparator
- Adding a new `xTimerStartFull` / `xTimerStart2` function — the user
  has explicitly asked for an in-place signature change

## Decisions

### D1: In-place signature change to `xTimerStart` (no new function)

**Decision:** Modify `xTimerStart` directly to add `on_cancel` between
`arg` and `timeout_ms`:

```c
xTimer xTimerStart(xTimerFunc fn, void *arg,
                   xTimerFunc on_cancel,
                   uint64_t timeout_ms, uint64_t repeat_ms);
```

**Why:** The user explicitly asked for no new function. Introducing
`xTimerStartFull` would leave two functions doing nearly the same thing
indefinitely, and existing call sites wouldn't benefit from the new
safety guarantee without opt-in churn.

**Alternatives considered:**
- `xTimerStartFull(...)`: rejected — leaves legacy `xTimerStart` forever
- Variadic helper / struct config (`xTimerConf`): rejected — overkill
  for one extra parameter; libx style is positional args

### D2: `on_cancel` placement — between `arg` and `timeout_ms`

**Decision:** Place the new parameter **after `arg`**, before
`timeout_ms`. Not at the end.

**Why:** `timeout_ms` and `repeat_ms` are the "timer configuration" pair
and read together. Putting `on_cancel` at the end would split them from
`fn`/`arg` and break the visual grouping:

```c
// Chosen order
xTimerStart(fn, arg, on_cancel, timeout_ms, repeat_ms);

// Rejected — splits config from callbacks
xTimerStart(fn, arg, timeout_ms, repeat_ms, on_cancel);
```

**Alternatives considered:**
- End of signature: rejected — splits config
- Before `fn`: rejected — `fn` is the primary callback, must come first

### D3: `on_cancel` invoked only on loop-destroy path, not on `xTimerStop`

**Decision:** `on_cancel` is invoked **only** when the host event loop
is destroyed with the timer still pending. `xTimerStop` does NOT invoke
`on_cancel`.

**Why:** Stop is user-initiated; the caller knows why they're stopping
and may want to reuse `arg` (e.g. restart the timer with different
parameters, transfer `arg` to another timer). If `on_cancel` ran on
stop, it would force the caller to re-allocate `arg` every time they
wanted to stop+restart, breaking the natural C idiom of "stop, mutate
state, start again with the same state pointer".

This diverges from `xWorkSubmit`'s `on_cancel`, which DOES fire on
user-initiated `xWorkCancel`. The divergence is intentional: timers
have a common restart pattern that work items do not.

**Alternatives considered:**
- Semantic B (stop triggers `on_cancel`): rejected — breaks the
  stop+restart idiom. The discussion in explore mode confirmed this
  with a concrete UAF scenario.

### D4: `on_cancel` and `fn` share the same typedef

**Decision:** Reuse `xTimerFunc` (the existing `void (*)(void *)`
typedef) for `on_cancel` rather than introducing a new typedef.

**Why:**
- Same signature (`void (*)(void *)`)
- Avoids API surface bloat
- Matches `xWorkCancelFunc` style which is also `void (*)(void *, void *)`
  matching `xWorkDoneFunc` (with the result pointer)

If we wanted different semantics later (e.g. pass a "reason" enum), we
could introduce a new typedef in a future change. For now, identical
signatures suffice.

**Alternatives considered:**
- New `xTimerCancelFunc` typedef: rejected — adds names without value

### D5: Backend destroy paths iterate heap and invoke `on_cancel`

**Decision:** Each backend's `*_destroy` function (`kq_destroy`,
`epoll_destroy`, `poll_destroy`, `wsapoll_destroy`) gains a shared
helper `timer_heap_destroy(loop)` that:

```c
while (xHeapSize(loop->timer_heap) > 0) {
    struct xTimer_ *t = (struct xTimer_ *)xHeapPop(loop->timer_heap);
    if (t->on_cancel) t->on_cancel(t->arg);
    timer_free(loop, t);
}
```

This replaces the existing duplicated loop in each backend (currently
4 copies of the same 3-line pattern). The helper lives in
`event_private.h` as a `static inline` so all backends share it.

**Why:** DRY — currently the destroy loop is duplicated 4 times across
backends. Adding `on_cancel` invocation in 4 places is error-prone;
centralizing in a shared helper is a one-line change per backend
(replace the loop with `timer_heap_destroy(loop)`).

**Alternatives considered:**
- Inline the `on_cancel` call in each backend: rejected — 4 copies of
  the same logic

### D6: `on_cancel` is a `xTimerFunc` parameter, may be NULL

**Decision:** `on_cancel` may be NULL. The destroy path and any
invocation site must NULL-check before calling.

**Why:** Most call sites (~95%) don't need cleanup — `arg` points to
stack-local state, or to a struct owned elsewhere. Forcing every caller
to provide a no-op function would massively churn the codebase for no
benefit. The NULL case is the natural "no cleanup needed" default.

This also makes the migration mechanical: every existing call site
just gains `, NULL` before the existing `, timeout_ms`.

### D7: Migration via mechanical sed, then targeted improvements

**Decision:** The 165 call sites are updated in two passes:

1. **Mechanical pass** — A scripted `sed` update inserts `, NULL`
   before the `timeout_ms` argument at every existing call site. This
   preserves current behavior (no cleanup) with zero semantic change.

2. **Targeted pass** — A small number of call sites (initial estimate:
   5-10) where `arg` is heap-allocated and owned by the timer are
   updated to provide a real `on_cancel`. These are identified by
   auditing each call site for the pattern `arg = malloc(...)` /
   `arg = new T(...)` where the free/delete only happens in `fn`.

**Why:** A two-pass approach keeps the mechanical churn separate from
the semantic changes. The mechanical pass is verifiable by diff (only
`, NULL` insertions). The targeted pass is small and reviewable.

**Alternatives considered:**
- Manual update of each site: rejected — 165 sites, too error-prone
- New function + gradual migration: rejected — user asked for in-place
  change

## Risks / Trade-offs

- **[Risk] Signature breakage breaks downstream consumers**
  - Mitigation: libx builds as a static archive; consumers must
    recompile. The change is flagged as BREAKING in proposal.md.
    In-tree consumers (libdlproxy, libxpp) are updated in the same
    change.

- **[Risk] `on_cancel` may invoke user code that touches freed state**
  - Mitigation: `on_cancel` runs before `timer_free`, so `t` is still
    valid inside `on_cancel`. The loop itself is being destroyed, so
    `on_cancel` must not call `xTimerStart` or any loop API. This
    restriction is documented in the API contract.

- **[Risk] `on_cancel` semantics (loop-destroy only) surprise users
  who expect stop to trigger it**
  - Mitigation: Documented prominently in `event.h`. The divergence
    from `xWorkSubmit` is called out with rationale.

- **[Risk] Mechanical sed pass introduces bugs in edge cases**
  (multi-line calls, macro arguments, comments)
  - Mitigation: After the sed pass, run `cmake --build` + `ctest` on
    Linux and macOS with ASan. Any site missed by sed will be a
    compile error; any site misreplaced will fail tests.

- **[Trade-off] 165 call sites need updating for a feature most won't
  use immediately**
  - Accepted: The alternative (new function) leaves two APIs forever.
    A clean break is preferred.

## Migration Plan

1. **libx layer (signature + struct + destroy paths)**
   - Update `event.h` signature
   - Update `event_timer.c` (`submit_timer` stores `on_cancel`)
   - Update `event_private.h` (struct field + shared `timer_heap_destroy` helper)
   - Update 4 backends to call the shared helper

2. **libx internal callers (sed pass + targeted review)**
   - Run scripted `xTimerStart(a, b, c, d)` → `xTimerStart(a, b, NULL, c, d)`
     on all `*.c` and `*.cpp` files under `libx/`, `libdlproxy/`, `libxpp/`
   - Audit each production caller (`.c` files) for heap-allocated `arg`
     that should provide a real `on_cancel`
   - Update `libdlproxy/dlproxy/dlproxy.c:153` (one site, `arg = t` is
     the dlproxy state — likely needs real on_cancel)

3. **Tests**
   - All test files updated mechanically
   - New test in `event_timer_lifecycle_test.cpp`:
     `TimerDestroyFiresOnCancel` — creates a timer, destroys the loop
     without firing, asserts `on_cancel` was invoked
   - New test: `TimerStopDoesNotFireOnCancel` — creates a timer, stops
     it, asserts `on_cancel` was NOT invoked
   - New test: `TimerFireDoesNotFireOnCancel` — creates a timer, lets
     it fire, asserts `on_cancel` was NOT invoked

4. **Docs**
   - Update `libx/x/base/event.h` API contract
   - Update `libx/x/base/EVENT.md` reference
   - Update `docs/libx/base/event.md` (signature table + examples)
   - Update `docs/libxpp/event.md`, `docs/libxpp/promise.md` examples
   - Update `AGENTS.md` if it references the signature (check)
   - `book/` is auto-generated from `docs/` — no manual edits

5. **CI verification**
   - Run `scripts/test-mac.sh -t openssl -j $(sysctl -n hw.ncpu) --asan`
   - Run `scripts/test-mac.sh -t mbedtls -j $(sysctl -n hw.ncpu) --asan`
   - Run `bash scripts/test-linux.sh -t openssl -j $(nproc) --asan`
     (in Docker via `.container/run-ci.sh`)
   - All 4 configs must pass with zero leaks reported by ASan

## Open Questions

- Should `xTimerStart`'s `on_cancel=NULL` behavior be documented as
  "the caller manages arg lifetime entirely" (current implicit contract)
  or should libx eventually mandate `on_cancel` for any heap-allocated
  arg? — **Defer to follow-up.** For now, NULL is allowed and the
  contract is "if you provide on_cancel, it runs on loop destroy".
