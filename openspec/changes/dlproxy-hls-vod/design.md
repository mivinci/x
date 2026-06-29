## Context

dlproxy's current architecture is built around MP4 single-file streaming:
- One task = one CDN URL = one cache resource with a single clip (clip_id `"0"`)
- `sched_mp4.c` drives block-level prefetch using a 1s tick timer
- `proxy.c` serves `GET /:rid` with byte-range support, hardcoding `Content-Type: video/mp4`

The cache layer (`Resource → Clip → Block → Piece`) is already generic — clips are keyed by string ID and a resource can hold multiple clips. The scheduler vtable (`on_tick / on_block_done / on_start / on_stop`) allows plugging in format-specific logic. These two design choices mean HLS support can be added without touching the cache layer at all.

HLS VOD differs from MP4 in three fundamental ways:
1. **Multiple files**: a master playlist references media playlists, which reference segment (`.ts`) files. Each segment is an independently decodable file.
2. **URL rewriting**: the player fetches an m3u8 from the proxy; segment URLs inside must be rewritten to route back through the proxy so the proxy can cache and serve them.
3. **Segment-level scheduling**: instead of "which byte block to fetch", the scheduler decides "which segment to prefetch". The remaining-buffer estimate uses segment durations (from the m3u8) rather than block counts ÷ bitrate.

## Goals / Non-Goals

**Goals:**
- Parse VOD m3u8 playlists (master + media) without external dependencies
- Rewrite segment URLs so the player routes all requests through the proxy
- Cache individual segments as cache clips, reusing the existing cache layer
- Implement an HLS VOD scheduler with segment-level prefetch using the existing hysteresis model
- Support `EXT-X-BYTERANGE` segments (some CDNs serve multiple segments from one file via byte ranges)
- Verify with Safari and a public HLS test stream

**Non-Goals:**
- Live HLS (m3u8 polling, sliding-window eviction, `EXT-X-MEDIA-SEQUENCE` roll-over)
- LL-HLS (Low-Latency HLS, CMAF chunks)
- Multi-bitrate ABR (adaptive bitrate) logic — the scheduler picks one variant and sticks with it; switching is a future enhancement
- DRM / encrypted segments (`EXT-X-KEY` with AES-128)
- Subtitles / alternate audio tracks (`EXT-X-MEDIA`)
- Segment eviction (VOD segments are cached permanently, same as MP4 blocks today)

## Decisions

### D1: Segment = Clip (no cache layer changes)

Each HLS segment maps to one cache clip. The clip_id is the segment's sequence number as a string (e.g. `"42"`).

**Rationale**: The cache layer already supports multiple clips per resource, keyed by string ID. A segment is just a file — blocks and pieces work identically. Zero changes to `cache.c`.

**Alternative considered**: Store all segments in a single clip with a virtual offset mapping (segment_seq × max_segment_size + offset). Rejected because it wastes space for variable-length segments and complicates readiness checks.

### D2: m3u8 parsing as a standalone module (`m3u8.c`)

A simple line-based parser that produces:
- `struct hls_variant`: bandwidth, resolution, codecs, URI (for master playlist)
- `struct hls_segment`: sequence number, URI, duration (EXTINF), byte range (EXT-X-BYTERANGE)
- `struct hls_playlist`: type (master/media), version, target duration, segment list, variant list

**Rationale**: m3u8 is line-oriented text. A hand-rolled parser avoids external deps and handles the subset of tags needed for VOD. The parser only needs to handle: `#EXTM3U`, `#EXT-X-VERSION`, `#EXT-X-TARGETDURATION`, `#EXTINF`, `#EXT-X-BYTERANGE`, `#EXT-X-STREAM-INF`, `#EXT-X-ENDLIST`. Other tags are ignored (per spec, unknown tags should be ignored).

**Alternative considered**: Use a library like `libm3u8`. Rejected — the format is simple enough and adds a dependency for no real benefit.

### D3: URL rewriting via path prefix

The proxy serves HLS content under `GET /:rid/...`:
- `GET /test/playlist.m3u8` → fetch from CDN, rewrite segment URLs to `./<seq>.ts` (relative)
- `GET /test/0.ts` → segment seq 0 of resource "test"

The proxy maintains a `segment_index → CDN_URL` map per task. When serving a `.ts` request, it looks up the CDN URL by the segment index in the path.

**Rationale**: Relative URLs in m3u8 keep the rewriting simple (just replace the segment URI with `./<seq>.ts`). The segment index is used as both the clip_id and the file path component, making the mapping trivial.

**Alternative considered**: Use a hash of the segment URL as the clip_id. Rejected — opaque IDs make debugging harder and the player URL less readable.

### D4: Scheduler variant selection

When a task starts in HLS mode, the scheduler:
1. Fetches the master playlist (if the URL ends in `.m3u8` and returns a master playlist)
2. Selects the first (or highest-bandwidth) variant
3. Fetches the media playlist for that variant
4. Builds the segment list

If the URL directly returns a media playlist (no `#EXT-X-STREAM-INF`), step 1-2 are skipped.

**Rationale**: VOD doesn't need ABR logic. Picking one variant up front keeps the scheduler simple. The first variant is a safe default; a future `dlp_task_conf_t` field could let the caller specify bandwidth preference.

### D5: Prefetch strategy — segment-ahead hysteresis

The HLS scheduler reuses the MP4 scheduler's three-zone model:
- `remain < emergency_ms` (10s): fetch the next uncached segment after the player's current position
- `emergency ≤ remain < safe_ms` (30s): fetch only if already pulling (hysteresis)
- `remain ≥ safe_ms`: stop pulling

`remain_time_ms` is computed by summing the durations of contiguous cached segments starting from the player's current segment.

The player's current segment is inferred from the requested `.ts` path: `GET /test/5.ts` → segment 5 → `read_segment = 5`.

**Rationale**: This is a direct translation of the MP4 scheduler's logic, replacing "block count × block_size ÷ bitrate" with "sum of segment durations". The hysteresis prevents oscillation between fetch and idle.

### D6: Task format field on `dlp_task_conf_t`

```c
typedef enum {
  DLP_FMT_MP4 = 0,   // default, backwards compatible
  DLP_FMT_HLS,
} dlp_format_t;

typedef struct {
  /* ... existing fields ... */
  dlp_format_t format;  // 0 = MP4 (default), 1 = HLS
} dlp_task_conf_t;
```

`dlp_task_create` selects the scheduler based on `format`. Default is MP4 for backwards compatibility.

**Rationale**: Explicit format selection is clearer than URL sniffing. Defaulting to MP4 preserves existing behavior.

### D7: `EXT-X-BYTERANGE` handling

Some VOD playlists pack multiple segments into one file using `EXT-X-BYTERANGE`. The parser captures the byte range per segment. When fetching, `dlp_http_fetch` is called with the segment's byte offset and length. The cache stores each segment as a separate clip, even though they come from the same CDN file.

**Rationale**: This keeps the cache model uniform (one clip per segment) while supporting the full VOD spec. The HTTP client already supports Range requests.

## Risks / Trade-offs

- **[Segment URL rewriting breaks if m3u8 uses absolute URLs with query strings]** → Mitigation: the parser preserves the full URI; rewriting replaces the path with `./<seq>.ts` and discards query strings (they're typically cache-busters or auth tokens that the proxy re-attaches when fetching from CDN).

- **[Large playlists (thousands of segments) may be slow to parse]** → Mitigation: the parser is O(n) in segment count. VOD playlists rarely exceed a few hundred segments for a typical movie. If needed, future optimization: lazy-load the segment list.

- **[No segment eviction means cache grows unbounded for long content]** → Same trade-off as current MP4 behavior (blocks are never evicted either). Acceptable for VOD proxy use case. Live HLS (future) would need eviction.

- **[Single-variant selection wastes bandwidth if the chosen variant is too high]** → Future: add ABR logic or let the caller specify bandwidth. For now, the caller can pass a media playlist URL directly to skip master playlist.

- **[m3u8 parser only handles a subset of tags]** → VOD needs: EXTM3U, EXT-X-VERSION, EXT-X-TARGETDURATION, EXTINF, EXT-X-BYTERANGE, EXT-X-STREAM-INF, EXT-X-ENDLIST. Other tags (EXT-X-KEY, EXT-X-MAP, EXT-X-MEDIA, etc.) are silently ignored. This is spec-compliant (unknown tags must be ignored) but means encrypted/content-segment HLS won't work until extended.
