## 1. Core API Changes (server.h / server.c)

- [x] 1.1 Remove `xHttpCtxYield` declaration from server.h
- [x] 1.2 Remove `xHttpCtxResume` declaration from server.h
- [x] 1.3 Add `xHttpCtxEndStream` declaration to server.h
- [x] 1.4 Remove `yielded` field from `struct xHttpConn_` in server_private.h
- [x] 1.5 Remove `xHttpCtxYield` implementation from server.c
- [x] 1.6 Remove `xHttpCtxResume` implementation from server.c
- [x] 1.7 Implement `xHttpCtxEndStream` — calls proto.end_stream + conn_after_response
- [x] 1.8 Make `xHttpCtxSend` call `conn_after_response` internally after sending
- [x] 1.9 Simplify `conn_dispatch_request` — remove auto-200, remove yielded check, remove auto-end-stream; only check hijacked

## 2. Update Existing Handlers (libx tests + benchmarks)

- [x] 2.1 Update all `on_request` handlers in `server_h1_test.cpp` that rely on auto-200 — add explicit `xHttpCtxSend`
- [x] 2.2 Update all `on_request` handlers in `server_tls_test.cpp` — same
- [x] 2.3 Update all `on_request` handlers in `https_test.cpp` — same
- [x] 2.4 Update all `on_request` handlers in `http_test.cpp` — same
- [x] 2.5 Remove yield/resume usage from test handlers — just return without sending
- [ ] 2.6 Update benchmark handlers in `libx/bench/` — add explicit send
- [x] 2.7 Run all tests, fix failures (2 pre-existing failures from curl multi-socket changes, not from this change)

## 3. Update dlproxy

- [x] 3.1 Remove `xHttpCtxYield` from proxy.c cache-hit path — just call `dlp_cache_read`, return without sending
- [x] 3.2 Remove `xHttpCtxYield` from proxy.c cache-miss path — just subscribe to bus, return without sending
- [x] 3.3 Remove `xHttpCtxResume` from `on_cache_read_done` — `xHttpCtxSend` finalizes internally
- [x] 3.4 Remove `xHttpCtxResume` from error paths in `on_chunk_ready`
- [x] 3.5 Remove `served` flag and re-check logic — no longer needed (bus publish + on_chunk_ready just calls send)
- [x] 3.6 Run dlproxy tests, fix failures

## 4. Update Examples

- [x] 4.1 Update `libdlproxy/examples/vod.cpp` if needed (no direct handler changes expected)
- [x] 4.2 Update any other example handlers

## 5. Verification

- [x] 5.1 Full test suite passes (`ctest --output-on-failure`) — 2 pre-existing failures unrelated to this change
- [ ] 5.2 dlproxy browser playback works (cold cache + warm cache + seek)
- [ ] 5.3 No connection leaks (check with `lsof -i :19080` after test)
