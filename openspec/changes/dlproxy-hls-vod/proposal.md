## Why

dlproxy currently only supports MP4 (single-file, byte-range) streaming. Many CDNs serve video as HLS (HTTP Live Streaming) — a master playlist referencing media playlists, each referencing dozens of `.ts` segment files. To act as a caching proxy for HLS content, dlproxy needs to parse m3u8 playlists, rewrite segment URLs to route through the proxy, cache individual segments, and schedule prefetch of upcoming segments based on the player's position.

VOD (Video-on-Demand) HLS is the natural first target: the playlist is static, all segment URLs are known upfront, and there is no live-edge tracking or segment eviction. This keeps scope bounded while building the HLS plumbing that live HLS can later extend.

## What Changes

- Add an m3u8 parser that handles both master playlists (variant selection) and media playlists (segment list with URI + duration + sequence number).
- Add a new `sched_hls.c` scheduler implementing the existing `dlp_scheduler_vtable`, driving segment-level prefetch instead of block-level.
- Extend the proxy (`proxy.c`) to:
  - Serve `*.m3u8` requests by fetching from CDN, rewriting segment URLs to point back at the proxy, and returning with `Content-Type: application/vnd.apple.mpegurl`.
  - Serve `*.ts` segment requests by mapping the segment to a cache clip, serving from cache or fetching from CDN on miss.
  - Set correct `Content-Type` per resource type instead of hardcoding `video/mp4`.
- Map each HLS segment to a cache clip (clip_id = segment sequence number), reusing the existing `Resource → Clip → Block → Piece` cache layer without changes.
- Track segment metadata (URL, duration, byte range if EXT-X-BYTERANGE present) on the task for scheduler use.
- Add an HLS VOD test stream to the example app for manual verification.

## Capabilities

### New Capabilities
- `hls-playlist-parsing`: Parse master and media m3u8 playlists (VOD only); extract variant streams, segment URIs, durations, and sequence numbers.
- `hls-proxy-serving`: Serve m3u8 (with URL rewriting) and `.ts` segment requests through the local HTTP proxy with correct Content-Type and cache integration.
- `hls-scheduler`: Segment-level prefetch scheduler that uses the player's current segment position and remaining buffer time to decide which segments to fetch, reusing the three-zone hysteresis model from the MP4 scheduler.

### Modified Capabilities
- `dlproxy-task-model`: Task creation needs to accept an HLS mode flag (or infer from URL/content-type) so the correct scheduler is selected; `dlp_task_conf_t` gains an optional format field.

## Impact

- **New files**: `sched_hls.c`, `m3u8.c` (+ `m3u8.h`), `hls_test.cpp` (or extend existing test).
- **Modified files**: `proxy.c` (m3u8 + segment serving, Content-Type), `dlproxy.c` (scheduler selection), `dlproxy_internal.h` (HLS fields on `dlp_task`), `dlproxy.h` (format field on `dlp_task_conf_t`), `CMakeLists.txt` (new sources).
- **No changes** to: `cache.c`/`cache.h` (clip abstraction already supports multiple clips per resource), `http.c`/`http.h` (HTTP client is content-agnostic), `bus.c` (already keyed by rid).
- **Dependencies**: None new — m3u8 parsing is text-based, no external library needed.
- **Non-goals (deferred to live HLS)**: m3u8 polling/refresh, sliding-window cache eviction, `EXT-X-MEDIA-SEQUENCE` roll-over, low-latency HLS (LL-HLS).
