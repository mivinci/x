## Context

`xEventLoopCreate()` currently takes no arguments. `xEventLoopCreateWithGroup(xTaskGroup group)` is the only parameterized creation path, and it only sets the default offload task group. There is no way to associate a human-readable name with a loop, making it hard to identify which thread runs which loop in `ps`, `htop`, lldb, or crash dumps.

Thread naming APIs are available on all platforms:
- Linux: `pthread_setname_np(pthread_self(), name)` — 15 chars + NUL
- macOS: `pthread_setname_np(name)` — 15 chars + NUL  
- Windows: `SetThreadDescription()` (Win 10 1607+)

The natural trigger point is `xEventLoopEnter()`, which already establishes the thread-loop binding. Adding thread naming there has zero overhead when no name is configured.

## Goals / Non-Goals

**Goals:**
- Allow tagging event loops with a human-readable name at creation
- Automatically set the OS thread name on `xEventLoopEnter`
- Unify `group` and `name` into a single `xEventLoopConf` struct
- Full backward compatibility — zero call site changes required

**Non-Goals:**
- Thread name save/restore across nested `Enter`/`Leave` (not portable, no macOS `pthread_getname_np`)
- Cross-platform thread naming abstraction (just three `#ifdef` blocks in `event_run.c`)
- Per-iteration performance monitoring or telemetry

## Decisions

### 1. Use a config struct (`xEventLoopConf`) instead of a setter

**Chosen**: `xEventLoopCreateWithConf(const xEventLoopConf *conf)` with `group` and `name` fields.

**Alternative**: `xEventLoopSetName(loop, name)` setter.

Set up once at creation. A setter raises questions — what if called after `Enter`? After `Run`? The config struct avoids these. It also groups `group` and `name` naturally, and future creation-time options slot into the same struct.

### 2. Keep existing creation functions as wrappers

`xEventLoopCreate(void)` ⇒ `xEventLoopCreateWithConf(NULL)`  
`xEventLoopCreateWithGroup(group)` ⇒ `xEventLoopCreateWithConf(&(xEventLoopConf){.group = group})`

Existing callers compile without changes. The canonical API is `CreateWithConf`.

### 3. Strategic no-restore for thread naming

**Chosen**: Set thread name on `Enter` if `name[0] != '\0'`. Do nothing on `Leave`.

**Alternative**: Save old thread name before `Enter`, restore on `Leave`.

Saving and restoring requires `pthread_getname_np` — not available on macOS. The strategic approach is simpler and covers the common case: if you named your loop, the thread is labeled. If you didn't, it stays as-is. Nested `Enter` just overwrites (the innermost loop owns the thread).

### 4. Name field: `char name[16]` in `struct xEventLoop_`

16 bytes (15 chars + NUL) matches the Linux/macOS `pthread_setname_np` limit. Truncation happens on copy from `conf->name` during `CreateWithConf` — a one-time `strncpy` no-op.

### 5. Thread naming via inline `#ifdef` in `event_run.c`

No separate `thread.c` file. Three platforms, ~6 lines each. Not worth abstracting into a reusable helper until a second consumer appears.

```c
#if defined(__linux__) || defined(__APPLE__)
  pthread_setname_np(pthread_self(), l->name);  // POSIX calls differ by platform
#endif
```

On macOS, `pthread_setname_np` takes only the name (sets the calling thread). On Linux, it takes `pthread_self()` + name. Both accept up to 15 chars + NUL.

## Risks / Trade-offs

- **Name truncation**: Names > 15 chars are silently truncated. Acceptable — thread names are hints, not identifiers. Users who need unique identification should include a short prefix.
- **No Windows thread naming yet**: The project doesn't actively test on Windows. `SetThreadDescription` can be added later without changing the API.
- **Nested Enter/Leave**: If two loops have names and one nests inside the other, the outer loop's name is lost when the inner loop's `Enter` overwrites it. This is acceptable — nesting is rare, and the debugger shows whichever loop is currently active.
