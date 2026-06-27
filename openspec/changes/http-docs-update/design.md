## Context

The HTTP module underwent a complete API refactor. Old types (xHttpResponse, xHttpRequest, xHttpResponseWriter, xHttpHandlerFunc, xHttpServerRoute) are gone. New unified types (xHttpCtx, xHttpDoneFunc, xHttpInitFunc, xHttpMux, xHttpRouteConf) are in place. All 7 doc files still reference the old API.

## Goals / Non-Goals

**Goals:**
- All doc code examples compile and match the new API
- Clear explanation of the unified xHttpCtx model
- Streaming examples (upload via on_read, download via on_data)
- Mux/resolver pattern explained for server
- Symmetric callback model (on_request/on_response, on_data, on_done) documented

**Non-Goals:**
- Adding new features or changing code
- Restructuring the docs directory

## Decisions

### D1: Rewrite, not patch

Each doc file gets a full rewrite rather than search-and-replace patches. The old API is fundamentally different — patching would leave inconsistent narratives.

### D2: Structure per file

- **client.md**: xHttpCtx fields, xHttpRequestConf fields, on_response/on_data/on_read/on_done callbacks, helpers, streaming examples
- **server.md**: xHttpServerConf, xHttpMux, xHttpRouteConf, resolver pattern, on_request/on_data/on_done, xHttpCtx write functions, streaming upload example
- **README.md**: overview of unified types, symmetric callback model diagram
- **sse.md**: SSE with new conf (on_read for POST body, content_length)
- **ws_client.md / ws_server.md**: xWsUpgrade takes xHttpCtx*, routes via xHttpMuxHandle
- **tls.md**: updated code examples

## Risks / Trade-offs

- **[Large rewrite]** → ~2700 lines to rewrite. Risk of introducing errors in examples. Mitigate by compiling all code examples mentally against current headers.
