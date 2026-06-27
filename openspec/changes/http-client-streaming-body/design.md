## Context

The xHTTP client wraps libcurl's multi interface on top of the xEventLoop. Currently it buffers the entire request and response body in memory. This change refactors the client to use streaming callbacks for both upload and download, and unifies the context type with the server side.

## Goals / Non-Goals

**Goals:**
- Stream response body via `on_data` (O(1) memory)
- Stream request body via `on_read` (O(1) memory for uploads)
- Early response metadata access via `on_response` (status/headers before body)
- Unify context type: `xHttpCtx` replaces `xHttpResponse` + `xHttpResponseWriter`
- Unify callback types with server: `xHttpInitFunc`, `xHttpDataFunc`, `xHttpDoneFunc`

**Non-Goals:**
- Pull-based reader API (incompatible with single-threaded event loop)
- Backpressure / flow control
- Changing the SSE API

## Decisions

### D1: Unified context — `xHttpCtx`

**Decision**: Replace `xHttpResponse`, `xHttpRequest` (server), and `xHttpResponseWriter` with a single `xHttpCtx` struct.

```c
XDEF_STRUCT(xHttpCtx) {
    const char *method;       // server: request method
    const char *url;          // server: request URL
    long        status_code;  // client: response status
    int         curl_code;    // client: curl error code
    const char *curl_error;   // client: error message
    const char *headers;      // both: raw headers
    size_t      headers_len;
    void       *internal_;    // internal state (writer, etc.)
};
```

No `body`/`body_len` — body is always delivered via `on_data`.

### D2: Four callback types, shared with server

```c
typedef int    (*xHttpInitFunc)(xHttpCtx *ctx, void *arg);        // headers done, 0=continue
typedef int    (*xHttpDataFunc)(const char *data, size_t len, void *arg);  // body chunks
typedef void   (*xHttpDoneFunc)(xHttpCtx *ctx, void *arg);        // completion
typedef size_t (*xHttpReadFunc)(char *buf, size_t bufsize, void *arg);     // upload (client only)
```

### D3: `xHttpRequestConf` — no body pointer, all callbacks

```c
XDEF_STRUCT(xHttpRequestConf) {
    const char  *url;
    xHttpMethod  method;
    size_t       content_length;  // Content-Length for on_read (0 = chunked)
    const char **headers;
    long         timeout_ms;
    xHttpVersion http_version;

    xHttpInitFunc on_response;  // optional: response headers arrived
    xHttpDataFunc on_data;      // optional: response body chunks (NULL = discard)
    xHttpReadFunc on_read;      // optional: request body provider (NULL = no body)
    xHttpDoneFunc on_done;      // optional: completion (NULL = fire-and-forget)
};
```

`body`/`body_len` removed. Upload via `on_read` only. `content_length` provides Content-Length header.

### D4: `on_response` replaces `on_header` — called once, not per-line

**Decision**: `on_response` (type `xHttpInitFunc`) is called once after all response headers are parsed, with `ctx->status_code` and `ctx->headers` available. Returns 0 to continue, non-0 to abort.

**Rationale**: Symmetric with server's `on_request`. Simpler for callers — one callback with full context, not per-line collection.

### D5: Response writing via `xHttpCtx*` functions

```c
XCAPI(void)   xHttpCtxSetStatus(xHttpCtx *ctx, int status);
XCAPI(xErrno) xHttpCtxSetHeader(xHttpCtx *ctx, const char *name, const char *value);
XCAPI(xErrno) xHttpCtxSend(xHttpCtx *ctx, const char *body, size_t len);
XCAPI(xErrno) xHttpCtxWrite(xHttpCtx *ctx, const char *data, size_t len);
XCAPI(void)   xHttpCtxDefer(xHttpCtx *ctx);
XCAPI(xErrno) xHttpCtxResume(xHttpCtx *ctx);
```

`xHttpResponseWriter` deleted. All write methods take `xHttpCtx*`.

### D6: `xHttpClientDo/Get/Post` all take `(client, conf, arg)`

```c
XCAPI(xErrno) xHttpClientDo(xHttpClient client, const xHttpRequestConf *conf, void *arg);
XCAPI(xErrno) xHttpClientGet(xHttpClient client, const xHttpRequestConf *conf, void *arg);
XCAPI(xErrno) xHttpClientPost(xHttpClient client, const xHttpRequestConf *conf, void *arg);
```

### D7: Helper utilities

```c
// Collect response body into a malloc'd buffer (for on_data)
XDEF_STRUCT(xHttpBody) { char *data; size_t len; };
XCAPI(int)  xHttpBodyCollect(const char *data, size_t len, void *arg);
XCAPI(void) xHttpBodyFree(xHttpBody *body);

// Provide static body data via on_read
XDEF_STRUCT(xHttpBodyProvider) { const char *data; size_t len; size_t offset; };
XCAPI(size_t) xHttpBodyProvide(char *buf, size_t bufsize, void *arg);
```

## Risks / Trade-offs

- **[BREAKING]** → All client API consumers must migrate. `body`/`body_len` → `on_read` + `content_length`. `on_response` → `xHttpResponseFunc` → `xHttpDoneFunc`. `xHttpResponse*` → `xHttpCtx*`.
- **[No body in ctx]** → Callers who want full body must use `xHttpBodyCollect` helper or collect in `on_data`.
- **[Simple POST more verbose]** → `xHttpBodyProvider` helper adds 2 lines vs old `.body = ptr, .body_len = len`.
- **[Data lifetime]** → `on_data`'s `data` valid only during callback. `on_read`'s `buf` owned by libcurl.
