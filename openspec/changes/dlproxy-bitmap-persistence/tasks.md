## 1. .meta File Format & I/O

- [x] 1.1 Define `.meta` format constants (header size, magic/version if desired) in cache.c
- [x] 1.2 Implement `meta_save()` — collect all block piece bitmaps, write header + bitmap body to `<rid>/<clip_id>.meta`
- [x] 1.3 Implement `meta_load()` — read `.meta`, validate header fields, restore piece bitmaps into clip's blocks
- [x] 1.4 Implement `meta_delete()` — unlink `.meta` file when clip is fully downloaded

## 2. Integrate with Cache Lifecycle

- [x] 2.1 Add `meta_dirty` flag to `struct dlp_block` — set when any piece is marked, cleared after `.meta` is written
- [x] 2.2 In `dlp_cache_open_clip`, after creating the clip, call `meta_load()` if `.meta` exists
- [x] 2.3 In the write callback (`on_write_done`), after freeing the write context, iterate blocks and call `meta_save()` for dirty ones
- [x] 2.4 After updating `b.done` in `dlp_block_check_done`, check if all blocks are complete; if yes, call `meta_delete()`

## 3. Testing & Verification

- [x] 3.1 Verify cache re-opens with correct bitmap after restart (manual: run proxy, download partial file, restart, confirm scheduler skips done blocks)
- [x] 3.2 Verify `.meta` is deleted when clip is fully downloaded
- [x] 3.3 Verify header mismatch discards `.meta` and starts fresh
