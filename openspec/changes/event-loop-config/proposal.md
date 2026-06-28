## Why

`xEventLoop` currently has no way to configure thread-level behavior at creation time. The event loop name serves no purpose beyond documentation, and threads running event loops are anonymous — invisible in `ps`, `htop`, and debuggers. This makes diagnosing multi-loop applications (HTTP server + WebSocket worker + DNS resolver) unnecessarily difficult. Additionally, adding `xTaskGroup` via a setter (the current API) is inconsistent with how other libx modules handle configuration at creation time.

## What Changes

- Add `xEventLoopConf` struct with `group` (default offload task group) and `name` (thread label)
- Add `xEventLoopCreateWithConf(const xEventLoopConf *conf)` as the canonical creation function
- Existing `xEventLoopCreate()` and `xEventLoopCreateWithGroup()` become convenience wrappers — no call site breakage
- On `xEventLoopEnter`, if the loop has a non-empty `name`, call platform-native thread naming API (Linux: `pthread_setname_np`, macOS: `pthread_setname_np`, Windows: `SetThreadDescription`)
- Add `char name[16]` field to `struct xEventLoop_`

## Capabilities

### New Capabilities

- `event-loop-config`: Thread-name-able event loops via `xEventLoopConf` — set a human-readable label at creation time that becomes the OS thread name on `xEventLoopEnter`. Existing creation functions remain as zero-argument convenience wrappers, preserving full backward compatibility.

### Modified Capabilities

<!-- None — no existing specs to modify -->

## Impact

- `libx/x/base/event.h` — new `xEventLoopConf` type + `xEventLoopCreateWithConf` declaration
- `libx/x/base/event_private.h` — `struct xEventLoop_` gains `char name[16]`
- `libx/x/base/event_run.c` — refactor creation to funnel through `CreateWithConf`; add thread naming in `Enter`
- All existing callers — no change required (wrappers preserve old signatures)
