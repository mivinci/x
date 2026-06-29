## ADDED Requirements

### Requirement: Cache bitmap is persisted to .meta file

The cache SHALL save block download progress (piece bitmaps) to a `.meta` file alongside each clip's `.data` file. The bitmap SHALL be written after the block's data write completes. The bitmap SHALL be loaded when the clip is opened. The `.meta` file SHALL be deleted when all blocks are complete.

#### Scenario: Meta file is created on first block completion

- **WHEN** a block's async data write completes and `b.done` becomes true
- **THEN** the cache writes a `.meta` file at `<rid>/<clip_id>.meta` containing the clip header and the full piece bitmap

#### Scenario: Meta file is loaded on clip open

- **WHEN** `dlp_cache_open_clip` is called and a `.meta` file exists for the clip
- **THEN** the cache reads the `.meta` header, validates `block_count` and `block_size` match
- **AND** restores the piece bitmaps into memory, marking `b.done = true` for completed blocks

#### Scenario: Meta file is discarded on header mismatch

- **WHEN** a `.meta` file exists but its `block_count` or `block_size` differs from the current clip configuration
- **THEN** the cache ignores the `.meta` file and starts with an empty bitmap

#### Scenario: Meta file is deleted when clip is fully downloaded

- **WHEN** all blocks in a clip have `b.done = true`
- **THEN** the `.meta` file is unlinked (deleted) from disk
- **AND** the `.data` file remains

#### Scenario: Restart resumes download from persisted bitmap

- **WHEN** the proxy is restarted and a clip is re-opened with a valid `.meta` file
- **THEN** blocks previously marked as complete in `.meta` are skipped by the scheduler
- **AND** only incomplete blocks are downloaded
