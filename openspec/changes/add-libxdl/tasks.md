## 1. Project scaffolding

- [ ] 1.1 Create `libxdl/` directory structure: `libxdl/xdl/`, `libxdl/examples/`, `libxdl/CMakeLists.txt`
- [ ] 1.2 Add `add_subdirectory(libxdl)` to root `CMakeLists.txt`
- [ ] 1.3 Create `libxdl/CMakeLists.txt` — build `xdl` library (links `xhttp`, `xfs`, `xcrypto`), CLI executable, and test target

## 2. Public API header (xdl/xdl.h)

- [ ] 2.1 Define `xdl_conf_t`, `xdl_task_conf_t`, `xdl_task_t`, `xdl_task_cb_t`, `xdl_progress`, phase enum, run mode enum
- [ ] 2.2 Declare `xdl_init(conf)` / `xdl_destroy()` for global lifecycle
- [ ] 2.3 Declare `xdl_task_create(const xdl_task_conf_t *conf)` / `xdl_task_start` / `xdl_task_pause` / `xdl_task_resume` / `xdl_task_stop` / `xdl_task_destroy`
- [ ] 2.4 Declare `xdl_task_seek(xdl_task_t *task, uint64_t offset)` returning int

## 3. Global state and run loop (xdl/xdl.c)

- [ ] 3.1 Implement `xdl_init(XDL_MODE_THREAD)` — create event loop, HTTP client, spawn worker thread
- [ ] 3.2 Implement worker thread entry: `xEventLoopEnter` + `xEventLoopRun(RUN_DEFAULT)` + `xEventLoopLeave`
- [ ] 3.3 Implement `xdl_destroy()` — stop loop, join thread, destroy HTTP client and loop

## 4. Task lifecycle (xdl/xdl.c)

- [ ] 4.1 Implement `xdl_task_create` — allocate task, create progress relay, subscribe user callback
- [ ] 4.2 Implement `xdl_task_start` — `xEventLoopPost(do_start)` → HEAD for content-length → determine blocks needed OR fallback to single GET → push to pending queue → `try_dispatch()`
- [ ] 4.3 Implement `xdl_task_pause` — set paused flag, stop dispatch (on_tick no-op), keep sources alive
- [ ] 4.4 Implement `xdl_task_resume` — clear paused flag, re-enable dispatch on next tick
- [ ] 4.5 Implement `xdl_task_stop` — `xEventLoopPost(do_stop)` → set cancelled flag, close sources, cancel in-flight requests
- [ ] 4.6 Implement `xdl_task_destroy` — Post destroy → completion flag → free

## 5. Checkpoint/resume (xdl/meta.c)

- [ ] 5.1 Define `.meta` file format (magic, block_count, block_size, total_size, concatenated 32-byte bitmaps per block)
- [ ] 5.2 Implement `meta_save(task)` — serialize block bitmaps to `<dest>.meta`
- [ ] 5.3 Implement `meta_load(task)` — read and validate `.meta`, restore block bitmaps, set `done` flags
- [ ] 5.4 Implement `meta_delete(task)` — remove `.meta` file
- [ ] 5.5 On `xdl_task_start`: call `meta_load` if file exists, else allocate fresh blocks

## 6. Block management (xdl/block.c)

- [ ] 6.1 Define block struct (offset, size, done flag, piece bitmap[32], retry count)
- [ ] 6.2 Implement piece-level bitmap operations (TODO: verify if resuming from partial download uses piece bitmap, piece_index, piece_index_end, piece_offset, piece_length, piece_count, piece_size, meta_size)
- [ ] 6.3 Implement `block_check_done()` — scan bitmap, set done flag
- [ ] 6.4 Implement `block_mark_range(offset, len)` — set bits in bitmap for download range

## 7. HTTP download engine (xdl/engine.c)

- [ ] 7.1 Implement `try_dispatch()` — pick next pending block from any task, construct Range header, fire `xHttpClientDo()`
- [ ] 7.2 Implement `on_http_data` — `xSha1Update` per chunk, `xFsReqSubmit` async write to `<dest>.part`
- [ ] 7.3 Implement `on_http_done` — in_flight--, sha1_final (file-level), block_mark_range, meta_save, retry on failure, try_dispatch()
- [ ] 7.4 Implement retry logic: max 3 attempts, skip 403/404. Per-task timeout from `conf.timeout_ms` forwarded to HTTP client.
- [ ] 7.5 Implement streaming SHA1 on resume: open existing `.part` file, async read + xSha1Update before downloading remaining blocks
- [ ] 7.6 Implement `.part` rename to `dest` on success + `meta_delete`
- [ ] 7.7 Implement no-Content-Length fallback: single GET without blocks, no `.meta`

## 8. Progress reporting

- [ ] 8.1 Define `xdl_progress` struct (phase, blocks_done, blocks_total, bytes_done, bytes_total)
- [ ] 8.2 Emit progress via `xRelayEmit` on each block completion and on task done/error

## 9. Seed Server (xdl/seed.c)

- [ ] 9.1 Define `xdl_server_conf_t` struct with all config fields
- [ ] 9.2 Implement `PUT /file/:fid/peer/:peer_id` — parse URL params + body, upsert peer entry, return peer list
- [ ] 9.3 Implement `DELETE /file/:fid/peer/:peer_id` — remove peer entry, return 200 or 404
- [ ] 9.4 Implement `GET /file/:fid/peer` — lookup and return peer array
- [ ] 9.5 Implement `GET /health` — return uptime + counts
- [ ] 9.6 Implement `GET /stats` — return peer_count, file_count, seed_count, leech_count
- [ ] 9.7 Implement periodic cleanup (1000ms timer, prune peers with last_seen > 5s)
- [ ] 9.8 Implement rate limiting (60 req/s per IP) and field validation (fid/peer_id length caps, have_pct clamp)
- [ ] 9.9 Implement per-file peer cap (max 256) and global file cap (max 1024)

## 10. Signal Server (xdl/signal.c) — UDP stateless relay

- [ ] 10.1 Create UDP socket on `signal_port`, bind, register with event loop for `recvfrom()`
- [ ] 10.2 Implement `recvfrom()` handler — parse JSON, dispatch by `type`: heartbeat vs relay
- [ ] 10.3 Implement `heartbeat` handler — always respond with `heartbeat_ack` containing queued messages; update `last_addr` and `last_beat_ms` from source address
- [ ] 10.4 Implement relay handler — extract `to` field, enqueue message in target peer's ring buffer (max 256)
- [ ] 10.5 Implement message queue per peer (ring buffer, max 256, TTL 5000ms), keyed by `peer_id`
- [ ] 10.6 Implement TTL sweep (per 1000ms tick): drop messages older than 5s
- [ ] 10.7 Implement queue full policy: drop oldest message, no error to sender
- [ ] 10.8 Implement message size limit (64KB) — drop silently on overflow

## 11. CLI example (libxdl/examples/main.c)

- [ ] 11.1 Create `libxdl/examples/CMakeLists.txt`
- [ ] 11.2 Implement CLI: `xdl <url> <dest> [sha1]`, populate `xdl_task_conf_t`, call `xdl_init` → `xdl_task_create` → `xdl_task_start`
- [ ] 11.3 Print progress to stderr, handle SIGINT/SIGTERM for graceful stop

## 12. Integration tests (xdl/xdl_test.cpp)

- [ ] 12.1 Test `xdl_task_create` with valid and invalid arguments
- [ ] 12.2 Test full download of a small file with SHA1 verification
- [ ] 12.3 Test checkpoint/resume: partial download → stop → resume → verify re-uses existing blocks
- [ ] 12.4 Test retry on network failure
- [ ] 12.5 Test SHA1 mismatch triggers retry
- [ ] 12.6 Test `xdl_task_stop` mid-download
- [ ] 12.7 Test concurrent download with multiple tasks
- [ ] 12.8 Test corrupt `.meta` triggers clean re-download
- [ ] 12.9 Test pause/resume: pause mid-download, verify no new dispatch, resume, verify download continues
- [ ] 12.10 Test pause on completed task (no-op)
- [ ] 12.11 Test `.part` renamed to `dest` on completion, `.part` missing, `.meta` missing
- [ ] 12.12 Test timeout triggers error (use a slow/unreachable URL with short timeout)

## 13. Server tests (xdl/server_test.cpp)

- [ ] 13.1 Test `PUT /file/:fid/peer/:peer_id` creates and upserts peer entries
- [ ] 13.2 Test `GET /file/:fid/peer` returns correct active peers
- [ ] 13.3 Test stale peer cleanup after 5s timeout
- [ ] 13.4 Test `DELETE /file/:fid/peer/:peer_id` removes peer gracefully
- [ ] 13.5 Test rate limiting blocks excessive requests
- [ ] 13.6 Test field validation (missing fid, invalid have_pct, oversized fields)
- [ ] 13.7 Test Signal Server auth flow (hello → hello_ack)
- [ ] 13.8 Test Signal Server relay (offer/answer/candidate forwarding)
- [ ] 13.9 Test Signal Server sender mismatch rejection
- [ ] 13.10 Test Signal Server duplicate peer_id replacement
- [ ] 13.11 Test Signal Server ping/pong timeout disconnection
- [ ] 13.12 Test Signal Server oversized message rejection

## 14. Polish

- [ ] 14.1 Run `clang-format` on all new C/C++ files
- [ ] 14.2 Ensure CI passes (cmake build + ctest)
