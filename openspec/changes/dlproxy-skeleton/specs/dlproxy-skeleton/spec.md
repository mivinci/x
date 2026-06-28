# dlproxy-skeleton

## ADDED Requirements

### Requirement: Project directory structure

The library SHALL be located at `libdlproxy/` with `libdlproxy/dlproxy.h` as the public API header. An example usage SHALL be at `examples/dlproxy/vod.cpp`.

#### Scenario: Files exist
- **WHEN** the project is scaffolded
- **THEN** `libdlproxy/dlproxy.h`, `libdlproxy/dlproxy.c`, `libdlproxy/proxy.c/h`, `libdlproxy/scheduler.c/h`, `libdlproxy/cache.c/h`, `libdlproxy/bus.c/h`, `libdlproxy/CMakeLists.txt` exist, and `examples/dlproxy/vod.cpp` exists

### Requirement: Two operation modes

The project SHALL support POLL mode (caller-driven event loop) and DETACHED mode (background thread with its own event loop).

#### Scenario: POLL mode
- **WHEN** `dlp_run(ctx, DL_MODE_POLL)` is called
- **THEN** the calling thread enters the event loop and blocks until `dlp_stop` is called

#### Scenario: DETACHED mode
- **WHEN** `dlp_run(ctx, DL_MODE_DETACHED)` is called
- **THEN** a background thread is spawned that owns the event loop, and `dlp_run` returns immediately

### Requirement: Public API

The project SHALL expose `dlp_init`, `dlp_run`, `dlp_stop`, `dlp_destroy`, `dlp_task_add`, `dlp_port` in `dlproxy.h`.

#### Scenario: Init and run
- **WHEN** `dlp_init` followed by `dlp_run(POLL)` is called
- **THEN** the proxy server listens on the configured port and responds to player requests

### Requirement: Bus-based module communication

The proxy and scheduler SHALL communicate exclusively through the pub/sub bus. No direct function calls between them.

#### Scenario: Cache miss notification
- **WHEN** a player request hits a cache miss in the proxy
- **THEN** the proxy subscribes to the bus and the scheduler is invoked through the bus
