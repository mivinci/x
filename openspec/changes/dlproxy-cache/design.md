## Context

The cache is the central data store for dlproxy. It holds downloaded video data in flat files, indexed by a hierarchical bitmap that tracks completion at Piece (1KB), Block (256KB), and Clip levels. The Player queries "is this range ready?" and the cache answers O(1) via Block.done or O(blocks) via Block-level bitmap for range scans.

## Goals / Non-Goals

**Goals:**
- Store downloaded data in flat files (one per clip)
- Track download progress at Piece granularity (P2P-ready)
- O(1) Block.done queries for player readiness checks
- Async I/O via `xFsReqSubmit`
- Unified model for MP4 Range, HLS ts, fMP4 fragments

**Non-Goals:**
- Per-block checksum storage (v2, for P2P)
- Cache eviction policy (v1 stores everything)
- Manifest/m3u8 parsing

## Decisions

### 1. Four-layer data model

```
dlp_resource  (one per video, keyed by rid)
  └── clips[] (one per ts segment / mp4 fragment, keyed by sequence id)
       └── blocks[] (256KB each, created on first write)
            └── pieces[] (1KB each, tracked via uint8 bitmap)
```

```c
#define DL_BLOCK_KB   256
#define DL_PIECE_KB   1
#define DL_PIECES_PER_BLOCK  (DL_BLOCK_KB / DL_PIECE_KB)  // 256

typedef struct {
  uint32_t offset;     // byte offset in clip file
  uint32_t size;       // actual block size (last block may be smaller)
  bool     done;       // all pieces downloaded
  uint8_t  pieces[DL_PIECES_PER_BLOCK / 8];  // 32 bytes = 256 bits
} dlp_block_t;

typedef struct {
  char            id[64];
  int             fd;           // flat data file
  uint32_t        block_count;
  struct dlp_block *blocks;     // array, allocated on first write
  uint64_t        total_size;
} dlp_clip_t;

typedef struct {
  char            rid[64];
  xMap            clips;        // id → dlp_clip_t*
  char            cache_dir[256];
} dlp_resource_t;
```

### 2. Flat file per clip, pread/pwrite

Each clip gets `{cache_dir}/{clip_id}.data`. Sparse by nature — only written blocks exist on disk. Blocks are created on first write (lazy allocation).

### 3. Two-level bitmap for range scans

```c
/* O(1): is this block fully downloaded? */
bool dlp_block_done(dlp_block_t *b) { return b->done; }

/* O(blocks): find next empty range starting from piece */
void dlp_find_range(dlp_clip_t *c, uint32_t start_piece, ...) {
    // Skip full blocks in O(1) per block using block.done
    // Only scan pieces within the first non-full block
}
```

### 4. Async I/O via xfs

All file operations go through `xFsReqSubmit`:
- `dlp_cache_write(clip, block_no, offset, data, len)` → `xFsOpWrite`
- `dlp_cache_read(clip, block_no, offset, buf, len)` → `xFsOpRead`

Ready checks (`done` flag) are synchronous memory reads — no I/O needed.

### 5. Block creation on first write

Unlike the ThumbPlayer implementation which pre-allocates based on known file size, our cache creates blocks lazily. When data arrives for block N and no block[N] exists yet, we allocate it and set offset = N * BLOCK_SIZE. This handles variable-length clips (HLS segments of unknown size).

## Risks / Trade-offs

- **Memory for bitmap**: 4KB per 1MB of clip data (32 bytes per block × 4 blocks/MB). For a 1GB clip: ~4MB of block metadata. Acceptable.
- **Sparse files**: Only written ranges consume disk space. OS handles sparse files natively on POSIX (lseek + write creates holes).
- **Lazy block creation**: First write to a new block triggers allocation. This is fast (one calloc) but happens inline with the write path.
