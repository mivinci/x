## Why

The HTTP client and server buffer entire request/response bodies in memory. For large transfers this wastes memory and adds latency. Additionally, the server couples routing with the server object, and the client/server use different context types and callback conventions. This change unifies everything: one `xHttpCtx`, shared callback types, streaming body via callbacks, and decoupled routing.

## What Changes

- **BREAKING**: Unify `xHttpResponse` + `xHttpRequest` + `xHttpResponseWriter` → single `xHttpCtx` (no `body`/`body_len` — body only via `on_data`)
- **BREAKING**: Unify callback types: `xHttpInitFunc` (on_request/on_response), `xHttpDataFunc` (on_data), `xHttpDoneFunc` (on_done), `xHttpReadFunc` (on_read, client only)
- **BREAKING**: `xHttpRequestConf` — remove `body`/`body_len`, add `content_length` + `on_read`; rename `on_header` → `on_response` (called once with full context, not per-line)
- **BREAKING**: `xHttpClientDo/Get/Post` all take `(client, conf, arg)` — no `on_response` parameter
- **BREAKING**: `xHttpServerCreate(conf)` — takes resolver at creation; delete `xHttpServerRoute`, `xHttpServerSetResolver`, `xHttpServerSetMaxBodySize`
- **BREAKING**: Server routing decoupled — `xHttpMux` as separate router, `xHttpRouteConf` with `on_request`/`on_data`/`on_done`
- Response writing via `xHttpCtxSetStatus` / `xHttpCtxSend` etc. (delete `xHttpResponseWriter`)
- Helpers: `xHttpBodyCollect` (collect response body), `xHttpBodyProvide` (provide static request body)

## Capabilities

### New Capabilities
- `http-client-streaming`: Unified streaming client with `xHttpCtx`, `on_response`/`on_data`/`on_read`/`on_done`
- `http-server-streaming`: Decoupled routing, streaming request body, `on_request`/`on_data`/`on_done`

### Modified Capabilities
<!-- All new — no existing specs to modify. -->

## Impact

- **API** (BREAKING): Complete refactor of client + server public API. All handlers and call sites must migrate.
- **Code**: `client.c`, `client.h`, `client_private.h`, `server.c`, `server.h`, `server_private.h`, `proto_h1.c`, `proto_h2.c`, `sse.c`, `ws.c`, `ws_serve.c` — all affected.
- **Tests**: All HTTP test files need migration (~100+ call sites).
- **Docs**: `docs/libx/http/client.md` and `server.md` need rewrite.
