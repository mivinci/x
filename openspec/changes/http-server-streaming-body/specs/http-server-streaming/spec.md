## ADDED Requirements

### Requirement: Server takes resolver at creation

```c
XDEF_STRUCT(xHttpServerConf) {
    xHttpResolveFunc resolve;          // required
    void            *router;           // required
    int              idle_timeout_ms;  // 0 = default
};

XCAPI(xHttpServer) xHttpServerCreate(const xHttpServerConf *conf);
```

No `xHttpServerRoute`, no `xHttpServerSetResolver`. The server does not own routes.

#### Scenario: Create server with mux

- **WHEN** a caller creates a server with `xHttpServerCreate(&conf)` where `conf.resolve = xHttpMuxResolve, conf.router = mux`
- **THEN** the server uses the mux for route resolution

### Requirement: Resolver returns xHttpRouteInfo

```c
XDEF_STRUCT(xHttpRouteInfo) {
    xHttpInitFunc on_request;  // optional: headers done, before body
    xHttpDataFunc on_data;     // optional: body chunks (NULL = discard)
    xHttpDoneFunc on_done;     // required: completion
    void         *arg;
};

typedef const xHttpRouteInfo* (*xHttpResolveFunc)(void *router, xHttpCtx *ctx);
```

Called after headers are parsed. Returns route info or NULL (404).

#### Scenario: Route matched

- **WHEN** the resolver finds a matching route
- **THEN** it returns `xHttpRouteInfo*` with `on_request`, `on_data`, `on_done`, `arg`
- **AND** the server proceeds with the request

#### Scenario: No route matched

- **WHEN** the resolver returns NULL
- **THEN** the server sends 404 and does not read the body

### Requirement: on_request — early access, can reject

`on_request` (type `xHttpInitFunc`) is called after headers, before body. `ctx->method`, `ctx->url`, `ctx->headers` are available. Returns 0 to continue, non-0 to abort (response already sent, body not read, `on_done` not called).

#### Scenario: Auth rejection

- **WHEN** `on_request` checks headers and sends 401 + returns non-0
- **THEN** the body is not read
- **AND** `on_done` is not called

#### Scenario: Prepare resource for on_data

- **WHEN** `on_request` opens a file and stores the handle in `arg`
- **THEN** `on_data` can access the file handle via `arg`

### Requirement: No body buffering — on_data only

Body is delivered via `on_data` (type `xHttpDataFunc`). If `on_data` is NULL, body is discarded. No `stream->body` buffer, no `max_body_size` check.

#### Scenario: Streaming upload

- **WHEN** a client POSTs 100 MB to a route with `on_data` set
- **THEN** `on_data` is called multiple times with body chunks
- **AND** the server never buffers the full body
- **AND** `on_done` is called after the full body is received

#### Scenario: Body discarded

- **WHEN** `on_data` is NULL and a client POSTs data
- **THEN** the body is read and discarded
- **AND** `on_done` is called with no body available

### Requirement: on_done — completion with xHttpCtx

`on_done` (type `xHttpDoneFunc`) is called after the body is fully received. `ctx` has `method`, `url`, `headers`. No `body` field. The caller writes the response via `xHttpCtxSetStatus` / `xHttpCtxSend`.

#### Scenario: Send response

- **WHEN** `on_done` is called
- **THEN** the caller calls `xHttpCtxSetStatus(ctx, 200)` and `xHttpCtxSend(ctx, "ok", 2)`
- **AND** the response is sent to the client

### Requirement: xHttpMux — built-in router

```c
XDEF_STRUCT(xHttpRouteConf) {
    const char     *pattern;
    xHttpInitFunc   on_request;
    xHttpDataFunc   on_data;
    xHttpDoneFunc   on_done;
    void           *arg;
};

XCAPI(xHttpMux) xHttpMuxCreate(void);
XCAPI(void)     xHttpMuxDestroy(xHttpMux mux);
XCAPI(void)     xHttpMuxHandle(xHttpMux mux, const xHttpRouteConf *conf);
XCAPI(const xHttpRouteInfo*) xHttpMuxResolve(void *router, xHttpCtx *ctx);
```

One `xHttpMuxHandle` function. `on_data` in conf controls streaming.

#### Scenario: Register routes

- **WHEN** a caller registers `xHttpMuxHandle(mux, &conf)` with `conf.pattern = "POST /upload"`
- **THEN** POST requests to `/upload` are routed to `conf.on_request` / `conf.on_data` / `conf.on_done`

### Requirement: Unified types with client

Server and client share: `xHttpCtx`, `xHttpInitFunc`, `xHttpDataFunc`, `xHttpDoneFunc`. Server uses `on_request` (client uses `on_response`), both type `xHttpInitFunc`.

#### Scenario: Same xHttpDoneFunc type

- **WHEN** a server handler and client completion callback are defined
- **THEN** both use `xHttpDoneFunc` — `void(xHttpCtx*, void*)`

### Requirement: Internal flow

```
on_headers_complete → resolve(router, ctx) → route_info
  → if NULL: send 404, don't read body
  → if route_info->on_request: call it
      └─ non-0: response sent, don't read body, don't call on_done
      └─ 0: continue
on_body → route_info->on_data(data, len, arg) or discard
  └─ non-0: send 413
on_message_complete → route_info->on_done(ctx, arg)
```
