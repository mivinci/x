## Why

A high-performance HTTP download proxy for video streaming, built on the new libx (event loop, async HTTP client/server, async filesystem I/O). Replaces the old tvkproxy with streaming downloads, async cache I/O, and a clean module architecture. Serves as both a practical tool and a reference implementation for libx application patterns.

## What Changes

- New `libdlproxy/` library directory, at same level as `libx/`
- Modular architecture: context, proxy, scheduler, cache, bus
- Public API in `libdlproxy/dlproxy.h`
- Example usage in `examples/dlproxy/vod.cpp`
- Two operation modes: POLL (caller-driven) and DETACHED (background thread)
- Unified cache model: Resource → Clip → Block → Piece
- Streaming HTTP download via new xhttp client `on_data` callback
- Async cache I/O via `xfs` module
- Deferred HTTP responses via `xHttpCtxYield` / `xHttpCtxResume`
- Pub/sub notification bus for scheduler → proxy communication
- POSIX only (no Windows support for v1)

## Capabilities

### New Capabilities

- `dlproxy-skeleton`: Library project structure, module interfaces, build system, and DETACHED/POLL mode lifecycle.

## Impact

- `libdlproxy/` — new library directory
- `libdlproxy/CMakeLists.txt` — build system
- `libdlproxy/dlproxy.h` — public API
- `libdlproxy/dlproxy.c` — context lifecycle
- `libdlproxy/proxy.c/h` — HTTP proxy server
- `libdlproxy/scheduler.c/h` — async Range downloader
- `libdlproxy/cache.c/h` — chunk cache
- `libdlproxy/bus.c/h` — pub/sub notification bus
- `examples/dlproxy/vod.cpp` — example entry point
