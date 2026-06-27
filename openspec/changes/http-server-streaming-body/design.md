## Context

The xHTTP server uses llhttp/nghttp2 to parse requests. Currently the server owns the route table, buffers the full request body, and dispatches via `xHttpServerRoute`. This change decouples routing from the server, removes body buffering, and unifies the context/callback types with the client side.

## Goals / Non-Goals

**Goals:**
- Decouple routing from server — server calls a resolver, routing is external
- Stream request body via `on_data` (O(1) memory, no body buffer)
- Early request metadata via `on_request` (URL/method/headers before body)
- Unify context and callbacks with client: `xHttpCtx`, `xHttpInitFunc`, `xHttpDataFunc`, `xHttpDoneFunc`
- Provide built-in `xHttpMux` router for convenience

**Non-Goals:**
- Changing response streaming (already handled via `xHttpCtxWrite`/`xHttpCtxDefer`)
- Backpressure / flow control
- Changing the SSE API

## Decisions

### D1: Server takes resolver at creation, not routes

```c
XDEF_STRUCT(xHttpServerConf) {
    xHttpResolveFunc resolve;          // required: called after headers
    void            *router;           // required: passed to resolve
    int              idle_timeout_ms;  // 0 = default (60000)
};

XCAPI(xHttpServer) xHttpServerCreate(const xHttpServerConf *conf);
```

No `xHttpServerRoute`, no `xHttpServerSetResolver`. The server never owns routes.

### D2: Resolver returns `xHttpRouteInfo`

```c
XDEF_STRUCT(xHttpRouteInfo) {
    xHttpInitFunc on_request;  // optional: headers done, before body
    xHttpDataFunc on_data;     // optional: body chunks (NULL = discard)
    xHttpDoneFunc on_done;     // required: completion
    void         *arg;         // shared across all three callbacks
};

typedef const xHttpRouteInfo* (*xHttpResolveFunc)(void *router, xHttpCtx *ctx);
```

Called after headers are parsed. `ctx->method`, `ctx->url`, `ctx->headers` are available. Resolver fills `ctx->params_` (via `xHttpCtxSetParam` internally) and returns the route info. Returns NULL = 404.

### D3: `on_request` — early access, can reject

Called after headers, before body. Has `xHttpCtx*` with request info. Can write a response (e.g., 401) and return non-0 to abort — body is not read, `on_done` is not called.

```c
int auth_check(xHttpCtx *ctx, void *arg) {
    if (!has_auth(ctx->headers)) {
        xHttpCtxSetStatus(ctx, 401);
        xHttpCtxSend(ctx, "Unauthorized", 12);
        return 1;  // abort — don't read body
    }
    return 0;  // continue
}
```

### D4: No body buffering — `on_data` only

- `on_data` set → body chunks delivered via callback
- `on_data` NULL → body discarded
- No `stream->body` xBuffer, no `max_body_size` check
- Caller controls memory by processing chunks in `on_data`

### D5: `xHttpMux` — built-in router

```c
XDEF_STRUCT(xHttpRouteConf) {
    const char     *pattern;    // "POST /upload"
    xHttpInitFunc   on_request; // optional
    xHttpDataFunc   on_data;    // optional
    xHttpDoneFunc   on_done;    // required
    void           *arg;
};

XCAPI(xHttpMux) xHttpMuxCreate(void);
XCAPI(void)     xHttpMuxDestroy(xHttpMux mux);
XCAPI(void)     xHttpMuxHandle(xHttpMux mux, const xHttpRouteConf *conf);
XCAPI(const xHttpRouteInfo*) xHttpMuxResolve(void *router, xHttpCtx *ctx);
```

One `xHttpMuxHandle` function — no separate stream variant. `on_data` in conf controls streaming.

### D6: Unified types with client

Same `xHttpCtx`, `xHttpInitFunc`, `xHttpDataFunc`, `xHttpDoneFunc` as client side. Server uses `on_request` (client uses `on_response`), both are `xHttpInitFunc`.

## Risks / Trade-offs

- **[BREAKING]** → `xHttpServerRoute`, `xHttpServerCreate()`, `xHttpServerSetMaxBodySize`, `xHttpRequest`, `xHttpResponseWriter` all deleted. All server code must migrate.
- **[on_request can't undo]** → If `on_request` sends a response and returns non-0, the connection is committed to that response. No retry.
- **[No body in on_done]** → `on_done` has no `body` field. Body was delivered via `on_data` or discarded. Callers who need body must collect it.
- **[Resolver called per request]** → The resolver is called for every request. For the mux, this is a linear scan of routes. For large route tables, a radix tree could be added later.
