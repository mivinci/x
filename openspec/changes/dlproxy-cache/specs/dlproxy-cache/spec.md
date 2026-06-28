# dlproxy-cache

## ADDED Requirements

### Requirement: Resource lifecycle

The cache SHALL support creating, opening, querying, and destroying resources. A resource is identified by a unique `rid` string and may contain zero or more clips.

#### Scenario: Create and open resource
- **WHEN** `dlp_cache_open(rid)` is called for a new resource
- **THEN** a resource struct is created with no clips and the data directory is created

#### Scenario: Re-open existing resource
- **WHEN** `dlp_cache_open(rid)` is called for a previously created resource
- **THEN** existing clips and blocks are loaded from disk metadata

### Requirement: Clip management

Each resource SHALL support multiple clips identified by sequence id. A clip has a flat data file and an array of blocks.

#### Scenario: Add clip to resource
- **WHEN** a clip is opened under a resource with a given clip_id
- **THEN** a data file is created and the clip is added to the resource's clip map

### Requirement: Block done flag

Each block SHALL have a boolean `done` flag that is set when all pieces in the block are downloaded. Querying block readiness SHALL be O(1).

#### Scenario: Block becomes done
- **WHEN** the last missing piece in a block is written
- **THEN** `block.done` is set to true

#### Scenario: Check block readiness
- **WHEN** `dlp_block_done(block)` is called
- **THEN** the result is returned in O(1) time

### Requirement: Piece tracking bitmap

Each block SHALL track individual piece completion via a 256-bit bitmap (32 bytes). Writing data SHALL update the corresponding piece bits.

#### Scenario: Write updates piece bitmap
- **WHEN** data is written covering pieces 0-15 of a block
- **THEN** bits 0-15 in the piece bitmap are set

### Requirement: Range scan via block skip

Range scans SHALL skip complete blocks using the block-level `done` flag, and only scan pieces within the first incomplete block.

#### Scenario: Find next empty range
- **WHEN** `dlp_find_empty_range(clip, start_piece)` is called
- **THEN** complete blocks are skipped in O(1) per block, and the first piece within the incomplete block is found in O(pieces per block)

### Requirement: Async I/O

All file read/write operations SHALL use `xFsReqSubmit` for asynchronous I/O on the event loop.

#### Scenario: Write block data asynchronously
- **WHEN** `dlp_cache_write_async(clip, block, data, len, cb)` is called
- **THEN** the write is dispatched via `xFsReqSubmit` and `cb` fires on completion

### Requirement: Block allocation on first write

Blocks SHALL be allocated lazily when data is first written to them, not pre-allocated at clip creation.

#### Scenario: First write to block N creates block
- **WHEN** data is written to block N that doesn't exist yet
- **THEN** a new `dlp_block_t` is calloc'd and inserted at position N in the blocks array
