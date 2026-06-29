## Why

The dlproxy cache currently stores block download progress (bitmap) only in memory. When the proxy restarts, all bitmap state is lost — previously downloaded data files remain on disk but the proxy doesn't know which blocks are complete, forcing it to re-download everything from zero. This wastes bandwidth and adds unnecessary startup latency. ThumbPlayer's DataTransport already solves this with `.cfg` per-clip bitmap files; dlproxy needs equivalent persistence for practical use.

## What Changes

- Save a `.meta` file alongside each clip's `.data` file, containing the block bitmap and file size metadata
- On `dlp_cache_open_clip`, automatically load the bitmap from an existing `.meta` file
- When all blocks in a clip are complete, delete the `.meta` file (cleanup)
- The bitmap is saved synchronously after each block write completes (inside the write callback)

## Capabilities

### New Capabilities

- `cache-bitmap-save`: Persist block download progress (Piece bitmap per Block) to a `.meta` file alongside the `.data` cache file. Automatically load on cache open, save on write completion, and delete when fully downloaded.

### Modified Capabilities

<!-- None — this is a purely additive change within the existing cache module. -->

## Impact

- `libdlproxy/dlproxy/cache.c` — add `.meta` file I/O, format definition, save/load/delete logic
- `libdlproxy/dlproxy/cache.h` — no API changes needed (`dlp_cache_write` already has a completion callback)
- `libdlproxy/dlproxy/dlproxy_internal.h` — may add flags for `meta_dirty` tracking on blocks
- No changes to proxy, scheduler, bus, or public API
