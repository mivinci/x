## ADDED Requirements

### Requirement: Segment-level prefetch scheduling
The HLS scheduler SHALL decide which segments to prefetch based on the player's current segment position and remaining buffer time, using a three-zone hysteresis model.

#### Scenario: Emergency zone — buffer below threshold
- **WHEN** the remaining playable time (sum of cached segment durations from the player's current segment) is below `emergency_ms` (default 10s)
- **THEN** the scheduler SHALL fetch the next uncached segment after the player's current position

#### Scenario: Safe zone — buffer above threshold
- **WHEN** the remaining playable time is at or above `safe_ms` (default 30s)
- **THEN** the scheduler SHALL NOT fetch any new segments

#### Scenario: Hysteresis zone — between emergency and safe
- **WHEN** the remaining playable time is between `emergency_ms` and `safe_ms`
- **AND** the scheduler was already fetching in the previous tick
- **THEN** the scheduler SHALL continue fetching the next uncached segment
- **WHEN** the scheduler was not fetching in the previous tick
- **THEN** the scheduler SHALL NOT start fetching

### Requirement: Remaining time computation
The scheduler SHALL compute `remain_time_ms` by summing the durations of contiguous cached segments starting from the player's current segment.

#### Scenario: Contiguous cached segments
- **WHEN** the player is on segment 5 and segments 5, 6, 7 are cached (durations 10s, 10s, 8s)
- **THEN** `remain_time_ms` SHALL be 28000 (28 seconds)

#### Scenario: Gap in cached segments
- **WHEN** the player is on segment 5 and segments 5 and 7 are cached but 6 is not
- **THEN** `remain_time_ms` SHALL be 10000 (only segment 5's duration, since 6 breaks continuity)

### Requirement: Segment fetch on tick
On each tick, the scheduler SHALL fetch at most one uncached segment — the next one after the player's current position.

#### Scenario: Next segment uncached
- **WHEN** the player is on segment 5 and segment 6 is uncached
- **THEN** the scheduler SHALL issue a fetch for segment 6

#### Scenario: Next segment already cached
- **WHEN** the player is on segment 5 and segment 6 is already cached
- **THEN** the scheduler SHALL check segment 7, 8, ... and fetch the first uncached one within the prefetch window

#### Scenario: All segments cached
- **WHEN** all segments up to the end of the playlist are cached
- **THEN** the scheduler SHALL not fetch anything and set `was_pulling = false`

### Requirement: Duplicate fetch prevention
The scheduler SHALL NOT issue a duplicate fetch for a segment that is already being downloaded.

#### Scenario: In-flight segment
- **WHEN** segment 6 is currently being fetched (downloading_segment = 6)
- **THEN** the scheduler SHALL NOT issue another fetch for segment 6 on subsequent ticks

### Requirement: Player position tracking
The scheduler SHALL track the player's current segment based on the `.ts` requests received by the proxy.

#### Scenario: Player advances to next segment
- **WHEN** the player requests `GET /:rid/6.ts`
- **THEN** the scheduler SHALL update `read_segment = 6` so the next tick computes remaining time from segment 6 onward

### Requirement: Playlist initialization
On task start, the HLS scheduler SHALL fetch and parse the m3u8 playlist before accepting any segment requests.

#### Scenario: Master playlist URL
- **WHEN** the task URL is a master playlist
- **THEN** the scheduler SHALL fetch it, select the first variant, fetch the media playlist, and build the segment list

#### Scenario: Media playlist URL
- **WHEN** the task URL is directly a media playlist
- **THEN** the scheduler SHALL skip variant selection and build the segment list directly

#### Scenario: Playlist fetch failure
- **WHEN** the m3u8 fetch fails (network error, HTTP error)
- **THEN** the scheduler SHALL retry on the next tick (1s interval)
