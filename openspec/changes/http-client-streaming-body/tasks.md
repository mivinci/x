## 1. API Definition

- [x] 1.1 Define `xHttpCtx` struct (method, url, status_code, curl_code, curl_error, headers, headers_len, internal_)
- [x] 1.2 Define `xHttpInitFunc`: `int(xHttpCtx*, void*)` — headers done, 0=continue
- [x] 1.3 Define `xHttpDataFunc`: `int(const char*, size_t, void*)` — body chunks (reuse existing)
- [x] 1.4 Define `xHttpDoneFunc`: `void(xHttpCtx*, void*)` — completion
- [x] 1.5 Define `xHttpReadFunc`: `size_t(char*, size_t, void*)` — upload (reuse existing)
- [x] 1.6 Define `xHttpRequestConf` with: url, method, content_length, headers, timeout_ms, http_version, on_response, on_data, on_read, on_done
- [x] 1.7 Define `xHttpBody` / `xHttpBodyCollect` / `xHttpBodyFree` helper
- [x] 1.8 Define `xHttpBodyProvider` / `xHttpBodyProvide` helper
- [ ] 1.9 Define `xHttpCtx*` write functions: SetStatus, SetHeader, Send, Write, Defer, Resume, Param (deferred to server change)
- [x] 1.10 Change `xHttpClientDo/Get/Post` to `(client, conf, arg)`
- [ ] 1.11 Delete: `xHttpResponse`, `xHttpRequest`, `xHttpResponseWriter`, `xHttpHeaderFunc`, `xHttpHandlerFunc`, `xHttpResponseFunc`, `xHttpRequestFunc`, all `xHttpResponse*` functions (deferred to server change — server still uses old types)

## 2. Core Implementation

- [x] 2.1 Implement `xHttpCtx` as wrapper around internal request/stream state
- [ ] 2.2 Implement `xHttpCtx*` write functions (deferred to server change)
- [x] 2.3 Implement `on_response` callback: called once after response headers, with ctx->status_code/headers
- [x] 2.4 Implement `on_data` for response body (streaming, no buffering)
- [x] 2.5 Implement `on_read` for request body upload (CURLOPT_READFUNCTION)
- [x] 2.6 Implement `on_done` with `xHttpCtx*` (replaces on_response completion)
- [x] 2.7 Remove body buffering: no `stream->body` xBuffer, no `body`/`body_len` in ctx
- [x] 2.8 Remove `on_header` per-line callback (replaced by `on_response`)
- [x] 2.9 Implement `xHttpBodyCollect` / `xHttpBodyProvide` helpers
- [x] 2.10 Refactor `Get`/`Post` to delegate to `Do`

## 3. Migrate Existing Call Sites

- [x] 3.1 Update `http_test.cpp` — migrate all xHttpClientDo/Get/Post calls
- [x] 3.2 Update `https_test.cpp` — migrate all calls
- [x] 3.3 Update `client_test.cpp` — migrate all calls + existing streaming tests
- [x] 3.4 Update `sse_test.cpp` — update SSE code to use new types
- [ ] 3.5 Update `server_h1_test.cpp` — handlers use `xHttpCtx*` instead of writer+req (deferred to server change)
- [ ] 3.6 Update `server_h2_test.cpp`, `ws_test.cpp`, `ws_connect_test.cpp` (deferred to server change)
- [ ] 3.7 Update `sse.c` — SSE server code to use `xHttpCtx*` (deferred to server change)

## 4. Tests

- [x] 4.1 Test: streaming GET — on_data chunks, on_done has no body field
- [x] 4.2 Test: on_response checks status before on_data
- [x] 4.3 Test: on_data abort (non-zero return)
- [x] 4.4 Test: streaming upload via on_read + content_length
- [x] 4.5 Test: streaming upload chunked (content_length=0)
- [x] 4.6 Test: xHttpBodyCollect helper collects full body
- [x] 4.7 Test: xHttpBodyProvide helper sends static body
- [x] 4.8 Test: fire-and-forget (on_done=NULL)
- [ ] 4.9 Test: response writing via xHttpCtxSend etc. (deferred to server change)

## 5. Documentation

- [ ] 5.1 Update `docs/libx/http/client.md` with new API
- [ ] 5.2 Add streaming download/upload examples
- [ ] 5.3 Document xHttpCtx field access and write methods
