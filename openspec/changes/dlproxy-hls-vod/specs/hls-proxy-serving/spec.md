## ADDED Requirements

### Requirement: Serve m3u8 playlist with URL rewriting
The proxy SHALL serve m3u8 playlist requests by fetching from the CDN, rewriting segment URIs to relative paths (`./<seq>.ts`), and returning with `Content-Type: application/vnd.apple.mpegurl`.

#### Scenario: Player requests master playlist
- **WHEN** the player requests `GET /:rid/playlist.m3u8`
- **THEN** the proxy SHALL fetch the m3u8 from the CDN, select a variant (first or as configured), fetch that variant's media playlist, rewrite segment URIs to `./<seq>.ts`, and return the rewritten media playlist with `Content-Type: application/vnd.apple.mpegurl`

#### Scenario: Player requests media playlist directly
- **WHEN** the CDN URL points directly to a media playlist (no `#EXT-X-STREAM-INF`)
- **THEN** the proxy SHALL skip master playlist processing and rewrite the media playlist's segment URIs directly

#### Scenario: Segment URI with query string
- **WHEN** a segment URI in the m3u8 contains a query string (e.g. `seg1.ts?token=abc`)
- **THEN** the proxy SHALL store the full CDN URL (including query string) in the segment map but rewrite the m3u8 URI to `./<seq>.ts` (query string stripped)

### Requirement: Serve segment requests
The proxy SHALL serve `.ts` segment requests by mapping the segment sequence number to a cache clip, serving from cache on hit or fetching from CDN on miss.

#### Scenario: Cache hit on segment
- **WHEN** the player requests `GET /:rid/5.ts` and segment 5 is fully cached
- **THEN** the proxy SHALL respond with `200 OK`, `Content-Type: video/mp2t`, and the segment body from cache

#### Scenario: Cache miss on segment
- **WHEN** the player requests `GET /:rid/5.ts` and segment 5 is not cached
- **THEN** the proxy SHALL defer the response (subscribe to bus), trigger the scheduler to fetch the segment, and respond when the segment is available

#### Scenario: Segment with EXT-X-BYTERANGE
- **WHEN** the segment was defined with `#EXT-X-BYTERANGE` in the m3u8
- **THEN** the proxy SHALL fetch only the byte range from the CDN URL, not the entire file

### Requirement: Content-Type per resource type
The proxy SHALL set the correct `Content-Type` header based on the requested resource type.

#### Scenario: m3u8 response
- **WHEN** serving an m3u8 playlist
- **THEN** the `Content-Type` SHALL be `application/vnd.apple.mpegurl`

#### Scenario: Segment response
- **WHEN** serving a `.ts` segment
- **THEN** the `Content-Type` SHALL be `video/mp2t`

#### Scenario: MP4 backwards compatibility
- **WHEN** serving an MP4 task (format=DLP_FMT_MP4)
- **THEN** the `Content-Type` SHALL remain `video/mp4` (existing behavior unchanged)

### Requirement: Segment-to-clip mapping
The proxy SHALL map each segment request to a cache clip using the segment sequence number as the clip_id.

#### Scenario: Segment clip lookup
- **WHEN** the player requests segment 5 of resource "test"
- **THEN** the proxy SHALL call `dlp_cache_read` with rid="test", clip_id="5"

#### Scenario: Segment clip creation
- **WHEN** a segment is fetched for the first time
- **THEN** the proxy SHALL call `dlp_cache_open_clip` with rid="test", clip_id="5", and the segment's size (0 if unknown, learned from CDN Content-Range)
