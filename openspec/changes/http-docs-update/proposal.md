## Why

The HTTP module API was completely refactored (xHttpCtx unified context, xHttpMux decoupled routing, streaming callbacks, removed xHttpResponse/xHttpRequest/xHttpResponseWriter). All 7 documentation files (~2700 lines) still describe the old API and are now misleading.

## What Changes

- Rewrite `docs/libx/http/client.md` — new xHttpRequestConf (content_length, on_response/on_data/on_read/on_done), xHttpCtx, helpers (xHttpBodyCollect/Provide)
- Rewrite `docs/libx/http/server.md` — xHttpServerConf, xHttpMux, xHttpRouteConf, resolver pattern, on_request/on_data/on_done, xHttpCtx write functions
- Update `docs/libx/http/README.md` — overview of new unified types
- Update `docs/libx/http/sse.md` — SSE API with new conf types
- Update `docs/libx/http/ws_client.md` and `ws_server.md` — xHttpCtx in upgrade handlers, xHttpMuxHandle for routes
- Update `docs/libx/http/tls.md` — updated code examples using new API
- Update `docs/SUMMARY.md` if any TOC entries change

## Capabilities

### New Capabilities
<!-- None — this is a documentation-only change. -->

### Modified Capabilities
<!-- None — specs are not affected, only docs. -->

## Impact

- **Docs only** — no code changes. All 7 files in `docs/libx/http/` need rewrite.
- **CODEBUDDY.md** — may need API examples updated.
