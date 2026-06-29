## ADDED Requirements

### Requirement: Parse master playlist
The system SHALL parse HLS master playlists (`#EXTM3U` containing `#EXT-X-STREAM-INF`) and extract each variant stream's bandwidth, resolution, codecs, and URI.

#### Scenario: Master playlist with multiple variants
- **WHEN** the parser receives a master playlist with 3 `#EXT-X-STREAM-INF` entries
- **THEN** it SHALL produce 3 variant records, each with bandwidth, resolution (if present), codecs (if present), and the URI from the following line

#### Scenario: Master playlist with URI attributes in EXT-X-STREAM-INF
- **WHEN** a `#EXT-X-STREAM-INF` line contains a `URI="..."` attribute instead of a following line
- **THEN** the parser SHALL extract the URI from the attribute

#### Scenario: Unknown tags in master playlist
- **WHEN** the master playlist contains tags not in the supported set (e.g. `#EXT-X-MEDIA`, `#EXT-X-I-FRAME-STREAM-INF`)
- **THEN** the parser SHALL skip them without error

### Requirement: Parse media playlist
The system SHALL parse HLS media playlists and extract the segment list with sequence numbers, URIs, durations, and byte ranges.

#### Scenario: Simple media playlist with EXTINF segments
- **WHEN** the parser receives a media playlist with `#EXTINF:10.0,\nsegment1.ts` entries
- **THEN** it SHALL produce a segment record for each entry with duration=10.0, URI="segment1.ts", and an incrementing sequence number starting from the `EXT-X-MEDIA-SEQUENCE` value (default 0)

#### Scenario: Media playlist with EXT-X-BYTERANGE
- **WHEN** a segment is preceded by `#EXT-X-BYTERANGE:1000@5000`
- **THEN** the segment record SHALL have byte_offset=5000 and byte_length=1000

#### Scenario: Media playlist with EXT-X-ENDLIST
- **WHEN** the playlist contains `#EXT-X-ENDLIST`
- **THEN** the parser SHALL mark the playlist as VOD (is_vod=true)

#### Scenario: Relative vs absolute segment URIs
- **WHEN** a segment URI is relative (e.g. `segment1.ts`)
- **THEN** the parser SHALL resolve it against the playlist URL's base
- **WHEN** a segment URI is absolute (e.g. `https://cdn.example.com/seg1.ts`)
- **THEN** the parser SHALL keep it as-is

### Requirement: Ignore unsupported tags
The system SHALL ignore HLS tags outside the VOD subset without failing.

#### Scenario: Encrypted playlist
- **WHEN** the playlist contains `#EXT-X-KEY:METHOD=AES-128,...`
- **THEN** the parser SHALL skip the tag and continue parsing, but the resulting playlist SHALL be marked as encrypted=true so the scheduler can refuse to fetch
