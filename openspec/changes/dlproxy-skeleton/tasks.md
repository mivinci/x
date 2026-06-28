## 1. Project scaffold

- [ ] 1.1 Create `libdlproxy/` directory at repo root
- [ ] 1.2 Create `libdlproxy/dlproxy.h` with public types and API declarations
- [ ] 1.3 Create `libdlproxy/CMakeLists.txt` linking xhttp + xfs + xbase
- [ ] 1.4 Wire `libdlproxy` into root `CMakeLists.txt`
- [ ] 1.5 Create `examples/dlproxy/vod.cpp` with POLL mode example

## 2. Context lifecycle (dlproxy.c)

- [ ] 2.1 Implement `dlp_init` — create loop, bus, cache, scheduler, proxy
- [ ] 2.2 Implement `dlp_run(POLL)` — enter and run event loop
- [ ] 2.3 Implement `dlp_run(DETACHED)` — spawn thread with event loop
- [ ] 2.4 Implement `dlp_stop` — post deinit to event loop
- [ ] 2.5 Implement `dlp_destroy` — cleanup external resources

## 3. Bus (bus.c/h)

- [ ] 3.1 Implement subscribe/unsubscribe with linked list per key
- [ ] 3.2 Implement publish — detach list, invoke all callbacks synchronously

## 4. Proxy (proxy.c/h)

- [ ] 4.1 Create xHttpServer on configured port
- [ ] 4.2 Implement `GET /:rid` route handler
- [ ] 4.3 Parse Range header, calculate offset/length
- [ ] 4.4 Cache hit path: read from cache, send 200/206
- [ ] 4.5 Cache miss path: yield response, trigger scheduler, subscribe bus

## 5. Scheduler (scheduler.c/h)

- [ ] 5.1 Create xHttpClient for upstream downloads
- [ ] 5.2 Implement `dlp_scheduler_fetch` — issue Range GET to CDN
- [ ] 5.3 Implement on_data callback — write to cache, mark done, publish bus
- [ ] 5.4 Implement on_done callback — handle errors, cleanup

## 6. Build and verify

- [ ] 6.1 Build dlproxy, fix compilation errors
- [ ] 6.2 Test POLL mode with curl against proxy
- [ ] 6.3 Test DETACHED mode with curl against proxy
- [ ] 6.4 Test Range request returns correct chunk
