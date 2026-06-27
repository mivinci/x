## 1. API Definition

- [x] 1.1 Define `xHttpServerConf` (resolve, router, idle_timeout_ms, max_header_size)
- [x] 1.2 Define `xHttpRouteInfo` (on_request, on_data, on_done, arg)
- [x] 1.3 Define `xHttpResolveFunc`: `const xHttpRouteInfo*(void*, xHttpCtx*)`
- [x] 1.4 Define `xHttpRouteConf` (pattern, on_request, on_data, on_done, arg)
- [x] 1.5 Define `xHttpMux` handle + `xHttpMuxCreate/Destroy/Handle/Resolve`
- [x] 1.6 Change `xHttpServerCreate` to take `const xHttpServerConf*`
- [x] 1.7 Delete: `xHttpServerRoute`, `xHttpServerSetMaxBodySize`, `xHttpServerSetResolver`, `xHttpServerSetIdleTimeout` (moved to conf), `xHttpRequest` (replaced by `xHttpCtx`), `xHttpResponseWriter` (replaced by `xHttpCtx*` functions), `xHttpHandlerFunc` (replaced by `xHttpDoneFunc`)

## 2. Core Implementation

- [x] 2.1 Implement `xHttpServerCreate(conf)` — store resolve func + router
- [x] 2.2 Move route matching to `on_headers_complete`: call `resolve(router, ctx)` → store `route_info` on stream
- [x] 2.3 Implement `on_request` dispatch: if `route_info->on_request`, call it; non-0 = abort
- [x] 2.4 In `on_body`: if `route_info->on_data`, call it (no buffering); else discard
- [x] 2.5 In `on_message_complete`: call `route_info->on_done(ctx, arg)` — no body field in ctx
- [x] 2.6 Remove `stream->body` xBuffer — no allocation, no buffering
- [x] 2.7 Remove `max_body_size` enforcement
- [x] 2.8 Implement `xHttpMux` — route table with pattern matching, `xHttpMuxResolve` returns `xHttpRouteInfo*`
- [x] 2.9 Implement `xHttpMuxHandle(mux, &conf)` — stores route with on_request/on_data/on_done
- [x] 2.10 Handle 404 (resolver returns NULL) and 405 (method mismatch)
- [x] 2.11 Same changes for HTTP/2 (nghttp2 `on_data` callback)

## 3. Migrate Existing Call Sites

- [x] 3.1 Update `server_test_helper.h` — `HttpServerTest` fixture uses `xHttpServerConf`, `xHttpMux`
- [x] 3.2 Update `server_h1_test.cpp` — handlers use `xHttpCtx*`, routes via `xHttpMuxHandle`
- [x] 3.3 Update `server_h2_test.cpp` — same
- [x] 3.4 Update `server_tls_test.cpp` — same
- [x] 3.5 Update `ws_test.cpp`, `ws_connect_test.cpp` — WebSocket upgrade uses `xHttpCtx*`
- [x] 3.6 Update `sse.c` — SSE server code uses `xHttpCtx*`
- [x] 3.7 Update `http_test.cpp` — server setup uses mux

## 4. Tests

- [x] 4.1 Test: streaming POST — on_data chunks, on_done has no body
- [x] 4.2 Test: on_request rejects with 401 (body not read)
- [x] 4.3 Test: on_data abort (non-zero → 413)
- [x] 4.4 Test: large upload (>1MB) — no max_body_size rejection
- [x] 4.5 Test: custom size limit via on_data return
- [x] 4.6 Test: GET with no body — on_data not called, on_done called
- [x] 4.7 Test: mux pattern matching (/users/:id params)
- [x] 4.8 Test: 404 (no route match)
- [x] 4.9 Test: custom resolver (no mux)

## 5. Documentation

- [ ] 5.1 Update `docs/libx/http/server.md` with new API
- [ ] 5.2 Add streaming upload example
- [ ] 5.3 Document resolver pattern and xHttpMux usage
