## ADDED Requirements

### Requirement: Client documentation matches new API

`docs/libx/http/client.md` SHALL document the current xHttpCtx-based API: xHttpRequestConf fields (url, method, content_length, on_response, on_data, on_read, on_done), xHttpCtx fields, xHttpBodyCollect/xHttpBodyProvide helpers, and streaming examples.

#### Scenario: Client doc code examples compile

- **WHEN** a reader copies a code example from client.md
- **THEN** it compiles against the current `client.h` without errors

### Requirement: Server documentation matches new API

`docs/libx/http/server.md` SHALL document the xHttpServerConf, xHttpMux, xHttpRouteConf, resolver pattern, on_request/on_data/on_done callbacks, and xHttpCtx write functions (SetStatus, Send, Write, Defer, Resume, Param).

#### Scenario: Server doc code examples compile

- **WHEN** a reader copies a code example from server.md
- **THEN** it compiles against the current `server.h` without errors

### Requirement: All other HTTP docs updated

README.md, sse.md, ws_client.md, ws_server.md, and tls.md SHALL have all code examples and API references updated to match the new types. No references to deleted types (xHttpResponse, xHttpRequest, xHttpResponseWriter, xHttpHandlerFunc, xHttpServerRoute).

#### Scenario: No old type references in docs

- **WHEN** searching all docs/libx/http/*.md for old type names
- **THEN** zero matches are found
