## 1. Project scaffolding

- [ ] 1.1 Create `libxdl/` directory structure: `libxdl/xdl/`, `libxdl/examples/`, `libxdl/CMakeLists.txt`
- [ ] 1.2 Add `add_subdirectory(libxdl)` to root `CMakeLists.txt`
- [ ] 1.3 Create `libxdl/CMakeLists.txt` — build `xdl` library (links `xhttp`, `xfs`, `xcrypto`, `xp2p`), CLI executable, and test target

## 2. Bencoding parser (xdl/bencode.c)

- [ ] 2.1 Implement `bencode_parse(const char *data, size_t len)` — recursive parser for strings, integers, lists, dictionaries
- [ ] 2.2 Implement `bencode_write(buf, value)` — serialize to bencoding format
- [ ] 2.3 Implement `bencode_sorted_dict_keys()` — ensure dictionary key ordering for deterministic SHA1
- [ ] 2.4 Implement `bencode_info_hash(torrent_data)` — extract info dict, compute SHA1(bencode(info))

## 3. Magnet URI parser (xdl/magnet.c)

- [ ] 3.1 Implement `magnet_parse(uri)` — extract info_hash (hex→20 bytes), display name (dn), tracker list (tr[])
- [ ] 3.2 Implement `magnet_validate(uri)` — validate format: `magnet:?xt=urn:btih:<40-char-hex>`
- [ ] 3.3 Test with valid URIs (full, minimal), invalid URIs (no xt, bad hex, malformed)

## 4. Public API header (xdl/xdl.h)

- [ ] 4.1 Define `xdl_conf_t` (mode, tracker_url), `xdl_task_conf_t` (magnet, dir, timeout_ms, max_peers, cb, arg), `xdl_task_t`, `xdl_task_cb_t`, `xdl_progress`, phase enum, run mode enum
- [ ] 4.2 Declare `xdl_init(conf)` / `xdl_destroy()` for global lifecycle
- [ ] 4.3 Declare `xdl_task_create(const xdl_task_conf_t *conf)` / `xdl_task_start` / `xdl_task_pause` / `xdl_task_resume` / `xdl_task_stop` / `xdl_task_destroy`
- [ ] 4.4 Declare `xdl_task_seek(xdl_task_t *task, uint64_t offset)` returning int

## 5. Global state, run loop, and P2P module (xdl/xdl.c)

- [ ] 5.1 Implement `xdl_init(XDL_MODE_THREAD)` — create event loop, HTTP client, spawn worker thread, use conf.peer_id, initialize global P2P module (peers map, sources map)
- [ ] 5.2 Implement worker thread entry: `xEventLoopEnter` + `xEventLoopRun(RUN_DEFAULT)` + `xEventLoopLeave`
- [ ] 5.3 Implement global announce timer (fires every `announce_ttl_ms`): collect all active P2P sources' `{info_hash, pct}` → `PUT /announce` → route signals to sources → discover peers → manage PeerConnections
- [ ] 5.4 Implement signal routing: offer/answer/candidate from PUT response → create/update PeerConnection → DataChannel ondatachannel → route to source by label (info_hash)
- [ ] 5.5 Implement `p2p_ensure_peer(remote_peer_id, info_hash)`: reuse existing PeerConnection or create new one with DataChannel(label=info_hash)
- [ ] 5.6 Implement `xdl_destroy()` — stop loop, join thread, clean up all PeerConnections, destroy HTTP client and loop

## 6. Task lifecycle (xdl/xdl.c)

- [ ] 6.1 Implement `xdl_task_create` — parse magnet URI, compute/validate info_hash, allocate task, create progress relay, subscribe user callback
- [ ] 6.2 Implement `xdl_task_start` — if magnet set: fetch torrent from Tracker, parse bencoding, extract metadata; if urls set: parse URLs (split by ';'); determine scheduler (P2P/Hybrid/HTTP); resume_load() → push to pending queue
- [ ] 6.3 Implement `xdl_task_pause` — set paused flag, stop dispatch (on_tick no-op), keep sources alive
- [ ] 6.4 Implement `xdl_task_resume` — clear paused flag, re-enable dispatch on next tick
- [ ] 6.5 Implement `xdl_task_stop` — `xEventLoopPost(do_stop)` → set stopped flag, close sources, cancel in-flight requests, send final PUT with pct=0 for this task's info_hash
- [ ] 6.6 Implement `xdl_task_destroy` — Post destroy → completion flag → free

## 7. Checkpoint/resume (xdl/resume.c)

- [ ] 7.1 Define `.resume` file format (magic "XDL_RESUME_V1\0\0", 20B info_hash, block_count u32 LE, bitmap N bytes)
- [ ] 7.2 Implement `resume_save(task)` — serialize block bitmap to `<dest>.part.resume`
- [ ] 7.3 Implement `resume_load(task)` — read and validate `.resume` (magic + info_hash match), restore block bitmap, set `done` flags
- [ ] 7.4 Implement `resume_delete(task)` — remove `.resume` file

## 8. Block management (xdl/block.c)

- [ ] 8.1 Define block struct (offset, size, done flag, retry count) — single unified granularity
- [ ] 8.2 Implement `block_mark_complete(block_index)` — set done flag in bitmap
- [ ] 8.3 Implement `block_pending_count(task)` — count remaining blocks for progress reporting

## 9. Scheduler (xdl/sched.c)

- [ ] 9.1 Implement `xdl_schedule_http(http_source)` — HTTP-only scheduler: on_tick dispatches blocks to single HTTP source, up to max_in_flight (default 32)
- [ ] 9.2 Implement `xdl_schedule_hybrid(http_source, p2p_source, pre, post, min_p2p)` — hybrid scheduler: blocks in [seek-pre, seek+post] → HTTP, outside → P2P
- [ ] 9.3 Implement `on_tick(task)`: scan pending blocks, dispatch highest-priority to available sources, respect source concurrency limits
- [ ] 9.4 Implement `on_block_done(task, offset, len, ok)`: block_mark_complete, resume_save, xRelayEmit

## 10. HTTP source (xdl/http_source.c)

- [ ] 10.1 Implement `xdl_source_http(url, timeout_ms)` — create HTTP source vtable with libcurl multi handle
- [ ] 10.2 Implement `http_source_fetch(task, offset, len, on_data, on_done)` — HEAD→Content-Length validation → `Range: bytes=offset-(offset+len-1)` → async GET via `xHttpClientDo`
- [ ] 10.3 Implement `on_http_data` / `on_http_done` callbacks — SHA1 update per chunk, per-block SHA1 verify, cell_ready call, retry on failure
- [ ] 10.4 Implement retry logic: max 3 attempts, skip 403/404. Per-task timeout from `conf.timeout_ms`.
- [ ] 10.5 Implement no-Content-Length fallback: single GET without blocks, no `.resume`
- [ ] 10.6 Implement source lifecycle: `open()` / `has_free_slot()` / `close()`

## 11. P2P source (xdl/p2p_source.c)

- [ ] 11.1 Implement `xdl_source_p2p(info_hash, max_peers)` — create P2P source vtable, register with global P2P module
- [ ] 11.2 Implement `p2p_source_fetch(task, offset, len, on_data, on_done)` — block_index = offset/BLOCK_SIZE, iterate ACTIVE peers with bitfield[block_index] && reqs_pending < 4, send DataChannel Request
- [ ] 11.3 Implement DataChannel message handlers: Handshake validation, BitField update, Block receive → SHA1 verify → on_done, HAVE → update peer bitfield, Cancel/Disconnect cleanup
- [ ] 11.4 Implement peer state machine: IDLE→HANDSHAKE→BITFIELD→ACTIVE, timestamp-based timeout → DEAD
- [ ] 11.5 Implement source lifecycle: `open()` (register) / `has_free_slot()` / `close()` (unregister + pct=0)

## 12. Progress reporting

- [ ] 12.1 Define `xdl_progress` struct (phase, blocks_done, blocks_total, bytes_done, bytes_total)
- [ ] 12.2 Emit progress via `xRelayEmit` on each block completion and on task done/error

## 13. Tracker server (xdl/tracker.c)

- [ ] 13.1 Define `xdl_tracker_conf_t` struct (port, default_ttl_ms, message_ttl_ms, cleanup_interval_ms, max_inbox_per_peer)
- [ ] 13.2 Implement `PUT /announce` — parse body (peer_id + changes), upsert peer entries by info_hash, return inbox signals + TTL
- [ ] 13.3 Implement `GET /torrent/:info_hash/peer` — lookup and return peer array with relay_addr (40-char hex)
- [ ] 13.5 Implement `POST /relay` — enqueue signaling message in target peer's inbox
- [ ] 13.6 Implement `POST /torrent` — accept bencoded body, compute info_hash, store torrent
- [ ] 13.7 Implement `GET /torrent/:info_hash` — return bencoded torrent body
- [ ] 13.8 Implement `GET /health` — return uptime + counts
- [ ] 13.9 Implement `GET /stats` — return peer_count, file_count, seed_count, leech_count
- [ ] 13.10 Implement periodic cleanup (1000ms timer, prune peers with last_seen > TTL)
- [ ] 13.11 Implement rate limiting (60 req/s per IP) and field validation (info_hash 40-char hex, pct clamp)
- [ ] 13.12 Implement per-file peer cap (max 256) and global file cap (max 1024)

## 14. CLI example (libxdl/examples/main.c)

- [ ] 14.1 Create `libxdl/examples/CMakeLists.txt`
- [ ] 14.2 Implement CLI: `xdl <magnet_uri>`, parse URI, populate `xdl_task_conf_t`, call `xdl_init` → `xdl_task_create` → `xdl_task_start`
- [ ] 14.3 Print progress to stderr, handle SIGINT for graceful stop via `xdl_task_destroy`

## 15. Integration tests (xdl/xdl_test.cpp)

- [ ] 15.1 Test `xdl_task_create` with valid and invalid magnet URIs, direct torrent, and URL-only configs
- [ ] 15.2 Test full download of a small file with per-block SHA1 verification
- [ ] 15.3 Test checkpoint/resume: partial download → stop → resume → verify re-uses existing blocks via `.resume`
- [ ] 15.4 Test retry on network failure
- [ ] 15.5 Test SHA1 mismatch per block triggers retry
- [ ] 15.6 Test `xdl_task_stop` mid-download
- [ ] 15.7 Test concurrent download with multiple tasks
- [ ] 15.8 Test corrupt `.resume` triggers clean re-download
- [ ] 15.9 Test pause/resume: pause mid-download, verify no new dispatch, resume, verify download continues
- [ ] 15.10 Test pause on completed task (no-op)
- [ ] 15.11 Test `.part` renamed to `dest` on completion, `.part` and `.resume` missing
- [ ] 15.12 Test timeout triggers error (use a slow/unreachable URL with short timeout)
- [ ] 15.13 Test info_hash mismatch in `.resume` triggers clean re-download
- [ ] 15.14 Test global P2P module: multiple tasks share PeerConnection to same remote peer

## 16. Server tests (xdl/server_test.cpp)

- [ ] 16.1 Test `PUT /announce` creates and upserts peer entries by info_hash, pct=0 removes entry
- [ ] 16.2 Test `GET /torrent/:info_hash/peer` returns correct active peers with relay_addr
- [ ] 16.3 Test stale peer cleanup after TTL timeout
- [ ] 16.4 Test rate limiting blocks excessive requests
- [ ] 16.6 Test field validation (missing info_hash, invalid pct, oversized fields)
- [ ] 16.7 Test `POST /torrent` publish and `GET /torrent/:info_hash` retrieve
- [ ] 16.8 Test `POST /torrent` with invalid bencoding returns 400

## 17. Polish

- [ ] 17.1 Run `clang-format` on all new C/C++ files
- [ ] 17.2 Ensure CI passes (cmake build + ctest)
