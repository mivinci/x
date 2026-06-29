## 1. m3u8 Parser Module

- [x] 1.1 Create `m3u8.h` with public structs: `hls_variant`, `hls_segment`, `hls_playlist` and parser API (`hls_parse_playlist`)
- [x] 1.2 Implement `m3u8.c` — line-based parser handling `#EXTM3U`, `#EXT-X-VERSION`, `#EXT-X-TARGETDURATION`, `#EXTINF`, `#EXT-X-BYTERANGE`, `#EXT-X-STREAM-INF`, `#EXT-X-ENDLIST`, `#EXT-X-MEDIA-SEQUENCE`
- [x] 1.3 Implement URI resolution: relative URIs resolved against playlist base URL; absolute URIs kept as-is
- [x] 1.4 Detect master vs media playlist (presence of `#EXT-X-STREAM-INF`)
- [x] 1.5 Mark playlist as encrypted if `#EXT-X-KEY` is present (parser doesn't decrypt, just flags)
- [x] 1.6 Write unit tests for m3u8 parser: master playlist, media playlist, byterange, relative/absolute URIs, unknown tags ignored, encrypted flag

## 2. Task Model Extension

- [x] 2.1 Add `dlp_format_t` enum (`DLP_FMT_MP4=0`, `DLP_FMT_HLS`) and `format` field to `dlp_task_conf_t` in `dlproxy.h`
- [x] 2.2 Add HLS-specific fields to `struct dlp_task` in `dlproxy_internal.h`: segment array, segment count, current segment index, downloading segment index, playlist URL, playlist parsed flag
- [x] 2.3 Add `dlp_sched_hls` vtable declaration in `dlproxy_internal.h`
- [x] 2.4 Update `dlp_task_create` in `dlproxy.c` to select scheduler based on `format` field (default MP4)

## 3. HLS Scheduler

- [x] 3.1 Create `sched_hls.c` with `dlp_sched_hls` vtable (`on_start`, `on_stop`, `on_tick`, `on_block_done`)
- [x] 3.2 Implement `hls_on_start`: fetch m3u8 from CDN URL, parse it, build segment list; if master playlist, select first variant and fetch its media playlist
- [x] 3.3 Implement `hls_on_tick`: compute `remain_time_ms` from contiguous cached segments starting at player position; apply three-zone hysteresis (emergency 10s / safe 30s); fetch next uncached segment if needed
- [x] 3.4 Implement `hls_on_block_done`: clear `downloading_segment`, update `remain_time_ms` estimate
- [x] 3.5 Implement duplicate-fetch prevention via `downloading_segment` field (same pattern as MP4's `downloading_off`)
- [x] 3.6 Implement `hls_on_stop`: free segment array and playlist resources

## 4. Proxy Serving

- [x] 4.1 Add route for `GET /:rid/:seg` in `dlp_proxy_init` that dispatches to `serve_hls` (handles both m3u8 and .ts)
- [x] 4.2 Implement m3u8 serving handler: fetch from CDN (or use cached parse), rewrite segment URIs to `./<seq>.ts`, set `Content-Type: application/vnd.apple.mpegurl`, send via `xHttpCtxSend`
- [x] 4.3 Segment serving via `serve_hls` dispatch (strips `.ts` suffix, parses seq, calls `serve_segment`)
- [x] 4.4 Implement segment serving handler: parse seq from path, map to clip_id, check cache readiness; on hit, read from cache and send with `Content-Type: video/mp2t`; on miss, defer response via bus subscribe and trigger scheduler tick
- [x] 4.5 Handle `EXT-X-BYTERANGE` segments: when fetching from CDN, pass byte offset and length to `dlp_http_fetch`
- [x] 4.6 Update `on_cache_read_done` to set Content-Type based on request type (m3u8 vs ts vs mp4) instead of hardcoding `video/mp4`
- [x] 4.7 Update player position tracking: `.ts` requests update `task->read_segment` (analogous to how `serve_range` updates `task->read_offset` for MP4)

## 5. Segment-to-Clip Cache Integration

- [x] 5.1 On segment fetch, call `dlp_cache_open_clip` with `clip_id = str(seq)` and segment size (0 if unknown)
- [x] 5.2 On segment data arrival, call `dlp_cache_write` with the correct clip_id and offset
- [x] 5.3 On segment read, call `dlp_cache_read` with the correct clip_id
- [x] 5.4 Verify `.meta` persistence works per-clip (each segment gets its own `.meta` file via existing cache logic)

## 6. HTTP Fetch Adaptation

- [x] 6.1 Ensure `dlp_http_fetch` can be called with a full URL (not just the task's base URL) — HLS segments may have different CDN URLs
- [x] 6.2 Add `dlp_http_fetch_full` helper for full segment fetch (no Range header) and `dlp_http_fetch_text` for m3u8 fetching

## 7. Example App

- [x] 7.1 Add an HLS test task to `vod.cpp` (auto-detect from `.m3u8` extension)
- [x] 7.2 Add a public HLS test stream URL for manual testing (https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8)

## 8. Testing

- [x] 8.1 Write unit tests for m3u8 parser (cover all scenarios from `hls-playlist-parsing` spec)
- [ ] 8.2 Write integration test: create HLS task, verify playlist is fetched and parsed, verify segments are cached on access
- [ ] 8.3 Write cache integration test: open clip per segment, write data, read back, verify `.meta` per segment
- [ ] 8.4 Manual test with Safari: start `dlp_vod` with an HLS URL, open in Safari, verify playback and seeking
- [x] 8.5 Manual test with curl: request m3u8 (verify URL rewriting), request segment (verify 200 + correct Content-Type)

## 9. Build & CI

- [x] 9.1 Add `m3u8.c` and `sched_hls.c` to `libdlproxy/CMakeLists.txt` (via GLOB)
- [x] 9.2 Add `m3u8_test.cpp` to the test target
- [x] 9.3 Verify all existing tests still pass (MP4 mode unaffected)
- [ ] 9.4 Verify CI passes on all 4 jobs (ubuntu/macos × openssl/mbedtls)
