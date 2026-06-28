## 1. Data structures

- [ ] 1.1 Define `DL_BLOCK_KB`, `DL_PIECE_KB`, `DL_PIECES_PER_BLOCK` constants
- [ ] 1.2 Define `dlp_piece_t`, `dlp_block_t`, `dlp_clip_t`, `dlp_resource_t` structs in `cache.h`
- [ ] 1.3 Implement `dlp_block_done()` and bitmap helpers (set/clear/test piece)

## 2. Resource and clip lifecycle

- [ ] 2.1 Implement `dlp_cache_open(rid)` — create or load resource
- [ ] 2.2 Implement `dlp_cache_close(res)` — close all clips, free
- [ ] 2.3 Implement `dlp_clip_open(res, clip_id, total_size)` — create data file, init metadata
- [ ] 2.4 Implement `dlp_clip_close(clip)` — close fd, free blocks

## 3. Block management

- [ ] 3.1 Implement lazy block allocation on first write
- [ ] 3.2 Implement `dlp_block_ensure(clip, block_no)` — alloc block N if not exist
- [ ] 3.3 Implement piece bitmap update on write

## 4. I/O operations

- [ ] 4.1 Implement `dlp_cache_read(clip, offset, buf, len, cb)` — async read via xfs
- [ ] 4.2 Implement `dlp_cache_write(clip, offset, buf, len, cb)` — async write via xfs, update bitmap
- [ ] 4.3 Implement range scan `dlp_find_empty_range(clip, start_piece, ...)` with block skip

## 5. Tests

- [ ] 5.1 Test resource create/open/close
- [ ] 5.2 Test block done flag after full write
- [ ] 5.3 Test piece bitmap updates
- [ ] 5.4 Test range scan with mixed full/empty blocks
- [ ] 5.5 Test lazy block allocation
- [ ] 5.6 Test async read/write via xfs

## 6. Build and verify

- [ ] 6.1 Add CMakeLists.txt for dlproxy cache
- [ ] 6.2 Build with cmake, fix compilation errors
- [ ] 6.3 Run cache tests, verify all pass
