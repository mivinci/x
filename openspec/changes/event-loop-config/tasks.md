## 1. Add xEventLoopConf and update public API

- [x] 1.1 Add `XDEF_STRUCT(xEventLoopConf) { xTaskGroup group; const char *name; }` to `event.h`
- [x] 1.2 Add `xEventLoopCreateWithConf(const xEventLoopConf *conf)` declaration to `event.h`
- [x] 1.3 Update doc comments for `xEventLoopCreate` and `xEventLoopCreateWithGroup` to note they are convenience wrappers

## 2. Add name field to internal loop struct

- [x] 2.1 Add `char name[16]` field to `struct xEventLoop_` in `event_private.h`
- [x] 2.2 Zero-initialize `name` in the struct (already handled by `calloc`)

## 3. Implement creation refactoring and thread naming

- [x] 3.1 Move creation logic from `xEventLoopCreateWithGroup` into `xEventLoopCreateWithConf`
- [x] 3.2 Copy `conf->name` to `loop->name` (truncate to 15 chars + NUL, default "xEventLoop")
- [x] 3.3 Change `xEventLoopCreate` and `xEventLoopCreateWithGroup` to one-liner wrappers
- [x] 3.4 Change `xEventLoopEnter`/`xEventLoopLeave` to `void`, add thread naming on Enter and name restore (from prev) on Leave

## 4. Write tests

- [x] 4.1 Test `xEventLoopCreateWithConf(NULL)` — verifies default name "xEventLoop"
- [x] 4.2 Test `xEventLoopCreateWithConf` with group + name
- [x] 4.3 Test name truncation (> 15 chars becomes 15)
- [x] 4.4 Test that `xEventLoopCreate` and `xEventLoopCreateWithGroup` wrappers work identically
- [x] 4.5 Test that `xEventLoopEnter` sets thread name for named loops
- [x] 4.6 Test that default name "xEventLoop" overrides pre-existing thread name
- [x] 4.7 Test that thread name is cleared after outermost Leave, restored on nested Leave

## 5. Build and verify

- [x] 5.1 Build with cmake (macOS), fix compilation errors
- [x] 5.2 Run `xbase_test`, verify all tests pass (558/558)
- [x] 5.3 Run full test suite, verify no regressions (8/8 suites pass)
- [x] 5.4 Update callers: `server_test_helper.h`, `nat_probe_test.cpp` to use `Leave()` instead of `Enter(old)`
