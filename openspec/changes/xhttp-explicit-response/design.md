## Context

The xhttp server's `on_request` handler follows a synchronous callback model:
1. Framework parses HTTP request
2. Calls `on_request(ctx, arg)` — handler does work, returns
3. `conn_dispatch_request` checks flags: `yielded`? `sent`? `streaming`?
4. If nothing sent and not yielded → auto-200 OK
5. `conn_after_response` — close or keep-alive

`xHttpCtxYield` sets `conn->yielded = 1` to prevent step 4. `xHttpCtxResume` clears it and calls `conn_after_response`. This is used by dlproxy's cache-miss path: yield → subscribe to bus → later send + resume.

The problem: "not sent yet" is an exceptional state requiring explicit opt-in (`yield`), when it should be the natural default in an event-driven framework.

## Goals / Non-Goals

**Goals:**
- Handler return = framework does nothing (no auto-200, no auto-close)
- `xHttpCtxSend` = send response + finalize (replaces send + resume)
- `xHttpCtxWrite` + `xHttpCtxEndStream` = streaming response + finalize
- No response sent = connection stays open until idle timeout or explicit close
- Remove `xHttpCtxYield` and `xHttpCtxResume` entirely

**Non-Goals:**
- Changing the HTTP/2 stream lifecycle (nghttp2 manages its own streams)
- Changing the WebSocket upgrade path (hijack model stays)
- Adding a response timeout (idle timeout already exists)

## Decisions

### Decision 1: `xHttpCtxSend` implicitly finalizes

When `xHttpCtxSend` is called, the framework sends the response and calls `conn_after_response` internally. No separate "resume" or "end" call needed.

**Rationale**: `xHttpCtxSend` is a complete response — there's nothing more to send. The handler is done. This matches Node.js `res.end(body)` semantics.

### Decision 2: `xHttpCtxWrite` requires explicit `xHttpCtxEndStream`

Streaming responses may have multiple `xHttpCtxWrite` calls. The handler must call `xHttpCtxEndStream` when done. This finalizes the stream and calls `conn_after_response`.

**Rationale**: For streaming, the framework can't know when the handler is done. An explicit end signal is necessary. This matches Node.js `res.end()` after `res.write()`.

**Alternative considered**: Auto-end on handler return. Rejected — in the new model, handler return does nothing. The handler might write some data, return, then write more later from a callback.

### Decision 3: No response sent = connection stays open

If `on_request` returns without calling send/write/endstream, the connection remains open. The framework sets the idle timeout (if configured). The handler can send a response later from any callback.

**Rationale**: This is the core of the event-driven model. "Not sent yet" is the default, not an exception.

**Risk**: A handler that forgets to send a response leaks the connection until idle timeout. Mitigation: idle timeout (default 60s) + log a warning if a connection closes without ever sending.

### Decision 4: `conn_dispatch_request` simplification

Current:
```
on_done → check hijacked → check yielded → check sent → auto-200 →
check streaming → auto-end-stream → conn_after_response
```

New:
```
on_done → check hijacked → return
```

That's it. Everything else is handled by `xHttpCtxSend` / `xHttpCtxEndStream` which call `conn_after_response` internally.

### Decision 5: Error responses (`xHttpConnSendError`) also finalize

`xHttpConnSendError` (used for 404, 413, 500, etc.) already sends a response. It should also call `conn_after_response` internally, so the caller doesn't need to.

## Risks / Trade-offs

- **BREAKING**: All `on_request` handlers must be updated. Handlers that relied on auto-200 must add an explicit `xHttpCtxSend` call. This affects tests, benchmarks, dlproxy, and examples.
- **Forgotten responses**: A handler that returns without sending leaks the connection. Mitigated by idle timeout + warning log.
- **HTTP/2 interaction**: H2 stream lifecycle is managed by nghttp2 callbacks. The `xHttpCtxSend` / `xHttpCtxEndStream` calls need to trigger the appropriate nghttp2 submit functions. This is already how it works — `send_response` and `end_stream` are protocol vtable methods.
