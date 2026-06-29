## ADDED Requirements

### Requirement: Handler return does not trigger response

The framework SHALL NOT send any response when `on_request` returns. The connection SHALL remain open until the handler explicitly sends a response, the idle timeout fires, or the client disconnects.

#### Scenario: Handler returns without sending

- **WHEN** an `on_request` handler returns without calling `xHttpCtxSend`, `xHttpCtxWrite`, or `xHttpCtxEndStream`
- **THEN** the connection remains open with the idle timeout active
- **AND** no response is sent to the client

#### Scenario: Handler sends response then returns

- **WHEN** a handler calls `xHttpCtxSend(ctx, body, len)` and then returns
- **THEN** the response is sent to the client
- **AND** the connection is finalized (close or keep-alive based on Connection header)

### Requirement: xHttpCtxSend finalizes the connection

`xHttpCtxSend` SHALL send the complete response (status + headers + body) and finalize the connection lifecycle. No separate resume or end call is needed.

#### Scenario: Complete response via xHttpCtxSend

- **WHEN** `xHttpCtxSend(ctx, body, len)` is called
- **THEN** status line, headers, and body are sent in a single response
- **AND** `conn_after_response` is called internally to handle close/keep-alive

### Requirement: xHttpCtxEndStream finalizes streaming responses

For streaming responses using `xHttpCtxWrite`, the handler SHALL call `xHttpCtxEndStream` to signal completion. The framework SHALL finalize the connection after `xHttpCtxEndStream`.

#### Scenario: Streaming response with explicit end

- **WHEN** a handler calls `xHttpCtxWrite(ctx, chunk1, len1)` followed by `xHttpCtxEndStream(ctx)`
- **THEN** the streaming response is sent with the chunk data
- **AND** the connection is finalized after the stream ends

#### Scenario: Streaming response without end

- **WHEN** a handler calls `xHttpCtxWrite` but never calls `xHttpCtxEndStream`
- **THEN** the connection remains open until idle timeout

### Requirement: xHttpCtxYield and xHttpCtxResume are removed

The `xHttpCtxYield` and `xHttpCtxResume` functions SHALL be removed. Deferred responses are handled naturally by not calling `xHttpCtxSend` until the response is ready.

#### Scenario: Deferred response without yield/resume

- **WHEN** a handler needs to defer a response (e.g., waiting for async I/O)
- **THEN** the handler returns without sending
- **AND** later calls `xHttpCtxSend` from a callback when data is ready
- **AND** no yield or resume calls are needed

## REMOVED Requirements

### Requirement: Auto-200 OK on handler return

**Reason**: In an event-driven framework, the handler return should not trigger implicit responses. "Not sent yet" is the default state, not an exception.

**Migration**: All handlers that relied on auto-200 must add an explicit `xHttpCtxSend(ctx, NULL, 0)` call (for empty-body 200) or `xHttpCtxSend(ctx, body, len)` with the appropriate body.

### Requirement: xHttpCtxYield prevents auto-200

**Reason**: With auto-200 removed, yield is unnecessary — not sending is the default.

**Migration**: Remove all `xHttpCtxYield` calls. The handler simply returns without sending.

### Requirement: xHttpCtxResume finalizes after deferred send

**Reason**: `xHttpCtxSend` now finalizes internally. Resume is redundant.

**Migration**: Remove all `xHttpCtxResume` calls. The `xHttpCtxSend` call handles finalization.
