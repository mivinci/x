## 1. Client Docs

- [x] 1.1 Rewrite `docs/libx/http/client.md` — xHttpCtx, xHttpRequestConf (content_length, on_response/on_data/on_read/on_done), helpers, streaming download/upload examples
- [x] 1.2 Add symmetric callback model diagram (on_response → on_data → on_done)

## 2. Server Docs

- [x] 2.1 Rewrite `docs/libx/http/server.md` — xHttpServerConf, xHttpMux, xHttpRouteConf, resolver pattern, on_request/on_data/on_done, xHttpCtx write functions
- [x] 2.2 Add streaming upload example (file upload via on_data)
- [x] 2.3 Add custom resolver example (no mux)

## 3. Other HTTP Docs

- [x] 3.1 Update `docs/libx/http/README.md` — overview of unified types, symmetric callback diagram
- [x] 3.2 Update `docs/libx/http/sse.md` — SSE with new conf (on_read for POST, content_length)
- [x] 3.3 Update `docs/libx/http/ws_client.md` — xWsUpgrade takes xHttpCtx*, routes via mux
- [x] 3.4 Update `docs/libx/http/ws_server.md` — same
- [x] 3.5 Update `docs/libx/http/tls.md` — code examples with new API

## 4. Verify

- [x] 4.1 Search all docs/libx/http/*.md for old type names — zero matches
- [x] 4.2 Update CODEBUDDY.md if HTTP API examples are outdated (no changes needed — CODEBUDDY.md doesn't have HTTP examples)
