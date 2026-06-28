## Why

dlproxy needs a local chunk cache to serve video data to players without re-downloading. The cache stores data from MP4 Range downloads, HLS ts segments, and fMP4 fragments — all in a unified flat file format with bitmap tracking at the Piece → Block → Clip → Resource level. The Block layer is essential for efficient bitmap scans, future P2P checksum verification, memory eviction granularity, and batched disk I/O.

## What Changes

- New `cache.h` / `cache.c` in `examples/dlproxy/`
- Four-layer data model: Resource → Clip → Block → Piece
- Piece = 1KB (P2P exchange unit), Block = 256KB (download/I/O/checksum unit)
- Two-level bitmap: Block.done + pieces[] bitmap per block
- Flat file storage: one data file per clip, blocks written at `offset = block_no * BLOCK_SIZE`
- Async I/O via xfs (`xFsReqSubmit`)
- O(1) block done checks, O(blocks) range scans via Block-level bitmap

## Capabilities

### New Capabilities

- `dlproxy-cache`: Hierarchical bitmap cache with Resource/Clip/Block/Piece layers, flat file storage, async I/O, P2P-ready piece tracking.

## Impact

- `examples/dlproxy/cache.h` — public API
- `examples/dlproxy/cache.c` — implementation
- Depends on `xbase` (xMap) + `xfs` (async file I/O)
- No changes to library code
