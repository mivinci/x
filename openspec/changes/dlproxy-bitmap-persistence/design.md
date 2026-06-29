## Context

The dlproxy cache (`cache.c`) currently manages a `Resource → Clip → Block → Piece` hierarchy in memory. Each block has a 32-byte piece bitmap (`pieces[32]`) tracking 256 one-KB pieces per 256-KB block. `done` is set to true when all pieces are marked. The bitmap is updated synchronously in `dlp_cache_write` before dispatching the async xfs write. However, the bitmap is **never persisted to disk** — restarting the proxy loses all progress and requires re-downloading every block.

The `.data` file (one per clip) is opened with `O_RDWR | O_CREAT` and persists across restarts. The blocks-array and piece bitmaps are rebuilt from zero on startup via `dlp_cache_open_clip`.

## Goals / Non-Goals

**Goals:**
- Save block piece bitmaps to a `.meta` file alongside each `.data` file
- Automatically load the bitmap from `.meta` when opening a clip that has existing data
- Delete `.meta` when all blocks in the clip are complete
- No API changes to cache.h — the save/load logic is transparent to callers

**Non-Goals:**
- Resource-level metadata file (ThumbPlayer's `.property` equivalent) — not needed; per-clip `.meta` is sufficient for our block design
- Checksum or corruption detection in `.meta` — rely on the data file integrity
- Incremental/mmap-based bitmap persistence — simple `pwrite`/`pread` is adequate

## Decisions

### Decision 1: `.meta` file format

```
[block_count:4][block_size:4][total_size:8][bitmap_bytes:variable]
```

- `block_count` (uint32, LE) — number of blocks in the clip
- `block_size` (uint32, LE) — size of each full block (256K)
- `total_size` (uint64, LE) — total file size in bytes (0 if unknown)
- `bitmap_bytes` — `block_count * DL_PIECE_BYTES` bytes, the raw `pieces[]` arrays concatenated

**Rationale**: Simple fixed-size header + variable bitmap body. The bitmap is stored as raw bytes matching the in-memory `pieces` array, no serialization overhead. The `total_size` and `block_count` fields validate against the current clip configuration on load — mismatch means the cache structure changed and we discard the `.meta`.

**Alternative considered**: Per-block separate files (ThumbPlayer's `.cfg` per clip). Rejected as overkill for our scale — one `.meta` per clip is simpler.

### Decision 2: Write timing

Write `.meta` after each block's async write completes (in `on_write_done` callback), not in `dlp_cache_write`. Mark a `meta_dirty` flag on the block when pieces change; check and flush all dirty blocks in `on_write_done`.

**Rationale**: Writing to `.meta` inside `dlp_cache_write` would interleave with async data writes. Doing it in the completion callback ensures data→meta ordering: data is on disk before we claim the block is done. The `meta_dirty` flag avoids writing on every piece change (up to 256 writes per block); instead we write once per block completion.

**Alternative considered**: Write `.meta` synchronously in `dlp_cache_write` after marking pieces. Rejected — would add latency to the HTTP data callback and cause ordering issues with concurrent writes.

### Decision 3: Load timing

Load `.meta` in `dlp_cache_open_clip` if the file exists and the header matches (`block_count`, `block_size` match). Set `b.done = true` for blocks where all pieces are marked. Delete `.meta` if all blocks are complete after loading.

**Rationale**: Single load point at clip open time. The clip open is called once per task creation/restart. If `.meta` is corrupted or from a different file version, we discard it and start fresh.

### Decision 4: `.meta` cleanup

When `dlp_block_check_done` sets `b.done = true`, check if **all** blocks in the clip are now done. If yes, call `unlink(meta_path)` to delete `.meta`. The `.data` file remains (it's the actual cache).

**Rationale**: A fully downloaded clip doesn't need a `.meta` — the data file is the source of truth. Deleting `.meta` prevents stale files and signals completion.

## Risks / Trade-offs

- **`.meta` and `.data` can get out of sync**: If the proxy crashes between writing `.data` and updating `.meta`, the bitmap may show fewer done pieces than actual data on disk. The pieces are re-downloaded on restart (wasted bandwidth, but no data corruption). Mitigation: write `.meta` AFTER the data write completes in the callback.
- **No `.meta` locking**: Concurrent writes from different threads/processes would corrupt `.meta`. Mitigation: our current design uses a single event-loop thread with xfs offloading — concurrent `.meta` writes to the same clip are serialized by the write callback chain.
- **`.meta` for large files**: 4096 blocks × 32 bytes = 128 KB — acceptable. For truly large files (GB), the bitmap grows proportionally but remains small relative to the data.
