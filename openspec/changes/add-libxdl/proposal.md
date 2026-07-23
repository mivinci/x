## Why

libx lacks a reusable, business-agnostic async download engine. `libdlproxy` has a solid download core with piece-level checkpoint/resume, but it's entangled with VOD proxy scheduling, HLS playlist parsing, and cache logic. Extracting this engine as `libxdl` enables Minecraft downloaders, game launchers, CI artifact fetchers, and future P2P/HTTP hybrid download scheduling — all sharing the same concurrency, verification, retry, and resume machinery.

## What Changes

- **New library** `libxdl/` at repo root, sibling to `libdlproxy`, with source in `libxdl/xdl/`
- `xdl_init(&conf)` / `xdl_destroy()` for global lifecycle with two run modes (`XDL_MODE_INHERIT`, `XDL_MODE_THREAD`)
- `xdl_task_t` opaque handle with `xdl_task_create/start/stop/destroy` — each task downloads one file (`{url, dest_path, sha1}`)
- Block/piece bitmap checkpoint (256 KB blocks, 1 KB pieces, `.meta` file persistence compatible with libdlproxy)
- Per-file streaming SHA1 verification via `xcrypto` (on_data incremental, on_done final compare)
- Configurable concurrency (global in_flight counter, sliding window), retry (max 3, 403 skip)
- Progress reporting via internal `xRelay` per task
- `libxdl/examples/` CLI tool (`xdl <url> <dest> <sha1>` for single-file download)
- Top-level `CMakeLists.txt` updated to add `libxdl` as a subdirectory

## Capabilities

### New Capabilities

- `xdl-client`: Client-side async download engine. Each task downloads one file with concurrent I/O, streaming SHA1 verification, checkpoint/resume via piece-level bitmap, configurable retry, seek-driven scheduling, and progress reporting. Operates on a dedicated internal thread.
- `xdl-server`: Server-side infrastructure. Seed Server (`PUT /file/:fid/peer/:peer_id`, `GET /file/:fid/peer`) maintains a peer x file matrix for peer discovery. Signal Server (WebSocket relay) routes SDP offers/answers and ICE candidates between peers.
- `xdl-protocol`: Communication protocols. Defines HTTP/JSON Seed protocol, WebSocket/JSON Signal protocol, and DataChannel binary protocol (Handshake, BitField, Request, Piece, HAVE).

### Modified Capabilities

None (no existing capabilities affected).

## Impact

- **Code**: New `libxdl/` directory at repo root (+CMakeLists.txt, ~8 source files, 2 test files, 1 CLI example)
- **Build**: Root `CMakeLists.txt` gains `add_subdirectory(libxdl)`
- **Dependencies**: `libxdl` depends on `xhttp` (HTTP), `xfs` (file I/O), `xcrypto` (SHA1), `xp2p` (WebRTC DataChannel), `x/json` (tracker protocol parsing)
- **API style**: snake_case with `xdl_` prefix, task-based lifecycle matching libdlproxy's pattern
- **Examples**: CLI example under `libxdl/examples/` demonstrating single-file download with progress
