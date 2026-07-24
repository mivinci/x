## Why

libx lacks a reusable, business-agnostic async download engine. `libdlproxy` has a solid download core with block-level checkpoint/resume, but it's entangled with VOD proxy scheduling, HLS playlist parsing, and cache logic. Extracting this engine as `libxdl` enables Minecraft downloaders, game launchers, CI artifact fetchers, and future P2P/HTTP hybrid download scheduling — all sharing the same concurrency, verification, retry, and resume machinery.

## What Changes

- **New library** `libxdl/` at repo root, sibling to `libdlproxy`, with source in `libxdl/xdl/`
- `xdl_init(&conf)` / `xdl_destroy()` for global lifecycle with two run modes (`XDL_MODE_INHERIT`, `XDL_MODE_THREAD`)
- `xdl_task_t` opaque handle with `xdl_task_create/start/pause/resume/stop/destroy` — each task downloads one file specified by a magnet URI (`magnet:?xt=urn:btih:<info_hash>`)
- Magnet URI parsing (`magnet.h/c`), BitTorrent-compatible bencoding parser (`.torrent`, `bencode.h/c` — internal module)
- Block-level checkpoint via `.resume` file (separate from the read-only `.torrent`)
- Per-block streaming SHA1 verification via `xcrypto` (from `info.blocks` hashes)
- P2P source via WebRTC DataChannels (`xp2p`): peer discovery, ICE/DTLS signaling relay via Tracker, block request/transfer
- Seek-driven scheduling (`xdl_task_seek`), pause/resume (`xdl_task_pause`/`resume`) for media playback
- Progress reporting via internal `xRelay` per task
- `libxdl/examples/` CLI tool (`xdl <magnet_uri>` for single-file download)
- Top-level `CMakeLists.txt` updated to add `libxdl` as a subdirectory

## Capabilities

### New Capabilities

- `xdl-client`: Client-side async download engine. Each task downloads one file with concurrent I/O, per-block SHA1 verification, checkpoint/resume via `.resume` bitmap, configurable retry, seek-driven scheduling, and progress reporting. Operates on a dedicated internal thread.
- `xdl-server`: Tracker server (HTTP/3, stateful). A single service handling peer discovery (`PUT /announce` with peer_id and `changes`), keep-alive heartbeat, signaling relay (offer/answer/candidate delivered in PUT response inboxes), and per-peer TTL-based expiry.
- `xdl-protocol`: Communication protocols. HTTP/JSON Tracker protocol (announce/heartbeat with inbox, signaling relay), binary DataChannel protocol (hello_req/rsp, bitfield_req/rsp, block_req/rsp, have_req, bye_req/rsp).

### Modified Capabilities

None (no existing capabilities affected).

## Impact

- **Code**: New `libxdl/` directory at repo root (+CMakeLists.txt, ~9 source files including bencoding parser, 2 test files, 1 CLI example)
- **Build**: Root `CMakeLists.txt` gains `add_subdirectory(libxdl)`
- **Dependencies**: `libxdl` depends on `xhttp` (HTTP), `xfs` (file I/O), `xcrypto` (SHA1), `xp2p` (WebRTC DataChannel), `x/json` (tracker protocol parsing)
- **API style**: snake_case with `xdl_` prefix, task-based lifecycle matching libdlproxy's pattern
- **Examples**: CLI example under `libxdl/examples/` demonstrating single-file download with progress
