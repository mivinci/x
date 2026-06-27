## ADDED Requirements

### Requirement: Unified xHttpCtx context

The system SHALL replace `xHttpResponse`, `xHttpRequest`, and `xHttpResponseWriter` with a single `xHttpCtx` struct containing request/response metadata and an internal pointer for response writing. No `body`/`body_len` fields — body is delivered via `on_data`.

#### Scenario: Client on_done receives xHttpCtx

- **WHEN** a request completes
- **THEN** `on_done` is called with `xHttpCtx*` containing `status_code`, `headers`, `curl_code`, `curl_error`
- **AND** `ctx->body` does not exist (body was delivered via `on_data` or discarded)

#### Scenario: Server on_done receives xHttpCtx

- **WHEN** a request body is fully received
- **THEN** `on_done` is called with `xHttpCtx*` containing `method`, `url`, `headers`
- **AND** the caller writes the response via `xHttpCtxSetStatus` / `xHttpCtxSend`

### Requirement: Four unified callback types

| Type | Signature | Used by |
|------|-----------|---------|
| `xHttpInitFunc` | `int(xHttpCtx*, void*)` | server `on_request`, client `on_response` |
| `xHttpDataFunc` | `int(const char*, size_t, void*)` | both `on_data` |
| `xHttpDoneFunc` | `void(xHttpCtx*, void*)` | both `on_done` |
| `xHttpReadFunc` | `size_t(char*, size_t, void*)` | client `on_read` only |

#### Scenario: Same xHttpDoneFunc type for server and client

- **WHEN** a server handler and a client completion callback are registered
- **THEN** both use `xHttpDoneFunc` — `void(xHttpCtx*, void*)`

### Requirement: xHttpRequestConf — no body pointer, callbacks only

```c
XDEF_STRUCT(xHttpRequestConf) {
    const char  *url;
    xHttpMethod  method;
    size_t       content_length;  // Content-Length for on_read (0 = chunked)
    const char **headers;
    long         timeout_ms;
    xHttpVersion http_version;

    xHttpInitFunc on_response;  // optional: response headers arrived
    xHttpDataFunc on_data;      // optional: body chunks (NULL = discard)
    xHttpReadFunc on_read;      // optional: upload body (NULL = no body)
    xHttpDoneFunc on_done;      // optional: completion (NULL = fire-and-forget)
};
```

#### Scenario: Simple GET

- **WHEN** `on_read` is NULL and `on_data` is NULL
- **THEN** no request body is sent, response body is discarded
- **AND** `on_done` is called with response status/headers

#### Scenario: POST with static body

- **WHEN** `on_read = xHttpBodyProvide` and `content_length > 0`
- **THEN** `Content-Length: content_length` header is sent
- **AND** `on_read` is called to fill upload buffer until EOF

#### Scenario: POST with streaming upload

- **WHEN** `on_read` is set and `content_length = 0`
- **THEN** `Transfer-Encoding: chunked` is used (automatic)
- **AND** `on_read` is called until it returns 0

### Requirement: on_response callback — once, full context

`on_response` (type `xHttpInitFunc`) SHALL be called once after all response headers are parsed, before any `on_data`. `ctx->status_code` and `ctx->headers` are available. Returns 0 to continue, non-0 to abort.

#### Scenario: Check status before processing body

- **WHEN** `on_response` returns non-0 (e.g., status is 404)
- **THEN** the transfer is aborted, `on_data` is not called
- **AND** `on_done` is called with the error

### Requirement: xHttpClientDo/Get/Post take (client, conf, arg)

All three functions take `(xHttpClient, const xHttpRequestConf*, void*)`. Get/Post force the method and delegate to Do.

#### Scenario: xHttpClientGet forces GET

- **WHEN** `xHttpClientGet(client, &conf, arg)` is called
- **THEN** `conf.method` is forced to GET
- **AND** `xHttpClientDo` is called internally

### Requirement: Response writing via xHttpCtx functions

```c
XCAPI(void)   xHttpCtxSetStatus(xHttpCtx *ctx, int status);
XCAPI(xErrno) xHttpCtxSetHeader(xHttpCtx *ctx, const char *name, const char *value);
XCAPI(xErrno) xHttpCtxSend(xHttpCtx *ctx, const char *body, size_t len);
XCAPI(xErrno) xHttpCtxWrite(xHttpCtx *ctx, const char *data, size_t len);
XCAPI(void)   xHttpCtxDefer(xHttpCtx *ctx);
XCAPI(xErrno) xHttpCtxResume(xHttpCtx *ctx);
XCAPI(const char*) xHttpCtxParam(xHttpCtx *ctx, const char *name, size_t *len);
```

`xHttpResponseWriter` and all `xHttpResponse*` functions are deleted.

### Requirement: Helper utilities

```c
// Collect response body (for on_data)
XDEF_STRUCT(xHttpBody) { char *data; size_t len; };
XCAPI(int)  xHttpBodyCollect(const char *data, size_t len, void *arg);
XCAPI(void) xHttpBodyFree(xHttpBody *body);

// Provide static body (for on_read)
XDEF_STRUCT(xHttpBodyProvider) { const char *data; size_t len; size_t offset; };
XCAPI(size_t) xHttpBodyProvide(char *buf, size_t bufsize, void *arg);
```

### Requirement: Thread safety and callback context

All callbacks run on the event loop thread. The `arg` passed to `Do/Get/Post` is forwarded to all callbacks (`on_response`, `on_data`, `on_read`, `on_done`).
