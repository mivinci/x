## Context

dlproxy is a local HTTP proxy that serves video data to players. It downloads chunks from upstream CDNs on demand and caches them locally. The project replaces an older tvkproxy implementation with the new libx API features.

## Architecture

```
                      ┌──────────────────────┐
                      │     Player (VLC/...  │
                      │   GET /rid HTTP/1.1  │
                      │   Range: bytes=X-Y   │
                      └──────────┬───────────┘
                                 │ 127.0.0.1:19080
                      ┌──────────▼───────────┐
                      │       proxy.c        │
                      │   xHttpServer        │
                      │   GET /:rid handler  │
                      └──────┬──────┬────────┘
                             │      │
              cache hit? ────┘      └──── cache miss?
              (200/206)                  │
                              ┌──────────▼───────────┐
                              │     scheduler.c       │
                              │   xHttpClient         │
                              │   GET {cdn_url}       │
                              │   Range: bytes=X-Y    │
                              └──────┬───────────────┘
                                     │
                                     │ on_data callback
                              ┌──────▼───────────────┐
                              │      cache.c          │
                              │   flat file + bitmap  │
                              │   (async via xfs)     │
                              └──────┬───────────────┘
                                     │
                                     │ cache done
                              ┌──────▼───────────────┐
                              │       bus.c           │
                              │   publish(rid:chunk)  │
                              └──────┬───────────────┘
                                     │
                         ┌───────────▼───────────────┐
                         │         proxy.c           │
                         │   on_chunk_ready callback │
                         │   → serve_range (hit)     │
                         │   → xHttpCtxResume        │
                         └───────────────────────────┘
```

## Module Interfaces

### dlproxy.h — Public API

```c
XDEF_HANDLE(dlp_ctx_t);

XDEF_STRUCT(dlp_conf_t) {
  uint16_t port;
  char     cache_dir[256];
  char     log_level;      // 0-3
};

XDEF_ENUM(dlp_mode_t) { DL_MODE_POLL, DL_MODE_DETACHED };

xErrno dlp_init(dlp_ctx_t *ctx, const dlp_conf_t *conf);
xErrno dlp_run(dlp_ctx_t ctx, dlp_mode_t mode);   // POLL: block; DETACHED: background
xErrno dlp_task_add(dlp_ctx_t ctx, const char *rid, const char *url, uint64_t size);
void   dlp_stop(dlp_ctx_t ctx);
void   dlp_destroy(dlp_ctx_t ctx);
uint16_t dlp_port(dlp_ctx_t ctx);
```

### proxy.h — HTTP Proxy Server

```c
xErrno dlp_proxy_init(dlp_ctx_t ctx, xEventLoop loop, xHttpMux mux, uint16_t port);
void   dlp_proxy_deinit(dlp_ctx_t ctx);
```

Two routes on `xHttpMux`:
- `GET /:rid` — main player endpoint, parses Range header

### scheduler.h — Async Scheduler

```c
xErrno dlp_scheduler_init(dlp_ctx_t ctx, xEventLoop loop, xHttpMux mux);
void   dlp_scheduler_deinit(dlp_ctx_t ctx);

// Called by proxy on cache miss
xErrno dlp_scheduler_fetch(dlp_ctx_t ctx, const char *rid, uint64_t offset, size_t len);

// Called by user to proactively download
xErrno dlp_scheduler_prefetch(dlp_ctx_t ctx, const char *rid);
```

### cache.h — Chunk Cache

From the dlproxy-cache change. Async I/O via xfs, bitmap tracking.

### bus.h — Pub/Sub Bus

```c
typedef void (*dlp_bus_func)(void *arg);

void dlp_bus_subscribe(dlp_ctx_t ctx, const char *key, dlp_bus_func cb, void *arg);
void dlp_bus_publish(dlp_ctx_t ctx, const char *key);
```

## Component Lifecycle

```
dlp_init:
  ctx->loop = xEventLoopCreate()
  ctx->bus  = dlp_bus_create()
  ctx->cache = dlp_cache_init(ctx->conf.cache_dir)
  ctx->scheduler = dlp_scheduler_init(ctx)
  ctx->proxy = dlp_proxy_init(ctx)

dlp_run(POLL):
  xEventLoopEnter(ctx->loop)
  ctx->conf.cb.on_running(ctx)
  xEventLoopRun(ctx->loop, X_RUN_DEFAULT)

dlp_run(DETACHED):
  xThreadCreate(dlp_detached_thread, ctx)

dlp_detached_thread:
  xEventLoopEnter(ctx->loop)
  xEventLoopRun(ctx->loop, X_RUN_DEFAULT)

dlp_stop:
  xEventLoopPost(ctx->loop, dlp_do_stop, ctx)

dlp_do_stop (on event loop thread):
  dlp_proxy_deinit(ctx)
  dlp_scheduler_deinit(ctx)
  dlp_cache_deinit(ctx)
  dlp_bus_destroy(ctx->bus)
  xEventLoopStop(ctx->loop)
```

## Dependency Graph

```
libdlproxy
  ├── libxhttp (server + client)
  ├── libxfs (cache I/O)
  ├── libxdns (optional, async DNS)
  ├── libxnet (URL parsing)
  ├── libxbase (event loop, map, thread, log)
  └── libxbuf (transitive via xhttp)
```

## Decisions

### 1. Internal coupling through dlp_ctx_t

All internal modules take `dlp_ctx_t` as their first argument. The context holds event loop, bus, cache, scheduler, and proxy handles. Modules access each other through the context — no global state.

### 2. Bus for cross-module communication

The bus is the only communication channel between scheduler and proxy. No direct function calls between them. This keeps the modules independently testable and avoids circular dependencies.

### 3. Single event loop thread

Both POLL and DETACHED modes run all callbacks on the event loop thread. No locks needed for module-internal data. The only synchronization is `xEventLoopPost` for external API calls in DETACHED mode.

### 4. DETACHED mode via xThread

DETACHED mode spawns a background thread that owns the event loop. Task creation/stop posted via `xEventLoopPost`. The `xNote` synchronization primitive is used where the caller needs to wait for the operation to complete on the event loop thread before returning.
