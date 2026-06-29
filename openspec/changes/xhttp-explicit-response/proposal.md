## Why

The xhttp server uses a synchronous callback model where `on_request` returns and the framework implicitly auto-sends 200 OK if the handler didn't call `xHttpCtxSend`. `xHttpCtxYield` / `xHttpCtxResume` were bolted on to support deferred responses, but they're a flag-based hack on top of the synchronous model. In an event-driven framework, "not sent yet" should be the default state, not an opt-in exception.

## What Changes

- **Remove auto-200**: handlers MUST explicitly call `xHttpCtxSend` or `xHttpCtxWrite` to send a response. If a handler returns without sending, the connection stays open until idle timeout or explicit close.
- **Remove `xHttpCtxYield`**: no longer needed — not sending is the default.
- **Remove `xHttpCtxResume`**: `xHttpCtxSend` / `xHttpCtxWrite` + `xHttpCtxEndStream` implicitly finalize the connection lifecycle.
- **Add `xHttpCtxEndStream`**: explicitly signals "response complete, finalize connection". For `xHttpCtxSend`, this is implicit. For `xHttpCtxWrite` (streaming), the handler must call it when done.
- **Update all existing handlers** to explicitly send responses.
- **BREAKING**: all `on_request` handlers must be updated.

## Capabilities

### New Capabilities

- `xhttp-explicit-response`: Replace the implicit auto-200 + yield/resume model with explicit response semantics. Handlers own the response lifecycle — the framework does nothing when `on_request` returns.

### Modified Capabilities

## Impact

- `libx/x/http/server.h` — remove `xHttpCtxYield`, `xHttpCtxResume`; add `xHttpCtxEndStream`
- `libx/x/http/server.c` — remove auto-200, remove `yielded` flag, simplify `conn_dispatch_request`
- `libx/x/http/server_private.h` — remove `yielded` field
- All `on_request` handlers in tests, benchmarks, dlproxy, and examples must be updated
- `libdlproxy/dlproxy/proxy.c` — remove yield/resume, simplify cache-miss path
