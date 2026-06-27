## Why

The HTTP server buffers the entire request body in memory and couples routing with the server object. This change decouples routing (via resolver + `xHttpMux`), removes body buffering (body only via `on_data`), adds `on_request` for early request access, and unifies all types with the client side (`xHttpCtx`, `xHttpInitFunc`, `xHttpDataFunc`, `xHttpDoneFunc`).

## What Changes

- **BREAKING**: `xHttpServerCreate(conf)` — takes `xHttpServerConf` with resolver + router at creation
- **BREAKING**: Delete `xHttpServerRoute`, `xHttpServerSetResolver`, `xHttpServerSetMaxBodySize` — routing is external
- **BREAKING**: Server calls `resolve(router, ctx)` after headers → returns `xHttpRouteInfo` (on_request/on_data/on_done/arg) or NULL (404)
- **BREAKING**: No body buffering — body only via `on_data`; `on_data` NULL = discard. No `max_body_size`.
- **BREAKING**: `on_request` (type `xHttpInitFunc`) — called after headers, before body. Can reject (return non-0, send 401 etc.)
- **BREAKING**: `on_done` (type `xHttpDoneFunc`) — called after body complete with `xHttpCtx*`. No `body` field.
- **BREAKING**: `xHttpMux` built-in router with `xHttpMuxHandle(mux, &conf)` — one function, `on_data` in conf controls streaming
- **BREAKING**: Delete `xHttpRequest`, `xHttpResponseWriter`, `xHttpHandlerFunc` — replaced by `xHttpCtx` + `xHttpDoneFunc`
- Response writing via `xHttpCtxSetStatus` / `xHttpCtxSend` etc. (shared with client)

## Capabilities

### New Capabilities
- `http-server-streaming`: Decoupled routing, streaming request body, unified `xHttpCtx` and callback types

### Modified Capabilities
<!-- All new — no existing specs to modify. -->

## Impact

- **API** (BREAKING): Server API completely refactored. All server handlers must migrate to `xHttpCtx*` + `xHttpDoneFunc`.
- **Code**: `server.c`, `server.h`, `server_private.h`, `proto_h1.c`, `proto_h2.c`, `sse.c`, `ws_serve.c` — all affected.
- **Tests**: All server test files need migration.
- **Docs**: `docs/libx/http/server.md` needs rewrite.
