## Context

libdlproxy has a battle-tested download engine: block/piece bitmaps with `.meta` persistence, streaming writes through `xFsReqSubmit`, Range-based HTTP fetching with a sliding-window scheduler. This design extracts that engine into a standalone library (`libxdl`) that handles one file per task, keeping the checkpoint format compatible for future code sharing.

## Goals / Non-Goals

**Goals:**
- Task-based API: `xdl_task_create(&conf)` -> `start` / `pause` / `resume` / `stop` / `destroy`
- Seek-driven download via `xdl_task_seek(task, offset)`
- Checkpoint/resume via `.meta` file (block 256 KB, piece 1 KB)
- Streaming SHA1 verification (file-level, xSha1Final once at completion)
- Tick-driven scheduling — unified driver for HTTP and P2P
- Pluggable sources: HTTP (`xdl_source_http`) and P2P (`xdl_source_p2p`) share the same vtable
- Per-task timeout (`conf.timeout_ms`) passed to HTTP client
- `.part` temp file with atomic rename on completion
- No Content-Length fallback (single-connection streaming, no checkpoints)
- Progress via internal `xRelay` per task
- Two run modes: `XDL_MODE_INHERIT` / `XDL_MODE_THREAD`
- CLI example

**Non-Goals:**
- Multi-file or group/batch operations
- Bandwidth throttling or per-host concurrency limits
- HTTP authentication or custom headers
- Upload/sharing

## Decisions

### Directory layout: `libxdl/xdl/` + `libxdl/examples/`

Mirrors `libdlproxy/dlproxy/` + `libdlproxy/examples/`.

### Dependencies

```
xdl
├── xhttp   (curl-based HTTP client)
├── xfs     (async file I/O)
├── xcrypto (SHA1)
├── xp2p    (WebRTC: DataChannel for P2P piece transfer)
├── x/json  (tracker response parsing, SDP signaling)
└── xbase   (xArray, xMap, xList, xHeap — data structures shared across all modules)
```

Internal data structures reuse libx containers:
- `xMap` — peer_id → p2p_peer lookup (hash table, string keys)
- `xArray` — dynamically sized peer list, pending request queues
- `xList` — intrusive linked list for per-block retry/pending chains
- `xHeap` — priority queue for seek-window block scheduling (if needed)
- `x/json` — parse tracker announce responses, SDP exchange payloads

### Naming: `xdl_` prefix, snake_case

`xdl_init`, `xdl_task_create`. The `x` prefix signals this is part of the libx ecosystem.

### Run modes: `XDL_MODE_INHERIT` / `XDL_MODE_THREAD`

Same pattern as mcdl and libdlproxy:
- `INHERIT`: reuse caller's `xEventLoopCurrent()`. Requires caller to pump the loop.
- `THREAD`: `xdl_init()` spawns a dedicated worker thread that blocks on `xEventLoopRun(RUN_DEFAULT)`, and starts a global tick timer (1000ms) that iterates all active tasks and calls each `task->sched->on_tick(task)`.

First version ships `XDL_MODE_THREAD` only.

### Task configuration: `xdl_task_conf_t`

```c
typedef void (*xdl_task_cb_t)(const xdl_progress *p, void *arg);

struct xdl_task_conf {
    const char   *url;          // HTTP source URL (NULL = HTTP disabled)
    const char   *file_id;      // file id for tracker (NULL = P2P disabled)
    const char   *tracker_url;  // Tracker URL, e.g. https://tracker.example.com
    const char   *dest;
    const char   *sha1_hex;     // 40-char hex, NULL = skip verification
    long          timeout_ms;   // default 30000
    int           max_peers;    // max P2P connections, default 8
    xdl_task_cb_t cb;
    void         *arg;
};

xdl_task_t xdl_task_create(const xdl_task_conf_t *conf);
```

The library selects the scheduler based on which fields are set:
- `url != NULL, file_id == NULL` → `xdl_schedule_http()`
- `url != NULL, file_id != NULL` → `xdl_schedule_hybrid()`
- `url == NULL, file_id != NULL` → P2P-only

### P2P source: `xdl_source_p2p()`

```c
// Internal config (not exposed to user)
struct xdl_p2p_conf {
    const char *file_id;      // file id for tracker
    const char *tracker_url;  // Tracker URL (HTTP/3)
    int         max_peers;
};

xdl_source_vtable_t *xdl_source_p2p(const xdl_p2p_conf_t *conf);
```

#### Internal state

```c
struct p2p_peer {
    xPeerConnection  pc;
    xDataChannel     dc;
    uint8_t         *bitfield;      // copy of peer's piece bitmap
    int              reqs_pending;  // in-flight piece requests to this peer
    int              state;         // IDLE/HANDSHAKE/BITFIELD/ACTIVE/DEAD
    uint64_t         last_recv_ms;
};

struct p2p_source {
    xdl_p2p_conf_t   conf;
    xdl_task_t      *task;
    xArray          *peers;         // xArray of struct p2p_peer
    xMap            *peer_by_id;    // peer_id -> index (for HAVE/DISCONNECT lookup)
};
```

#### DataChannel message protocol (binary frames)

Messages are sent via `xDataChannelSendBinary` on reliable ordered channels:

```
Handshake  : [0x01][20B file_id][32B peer_id_utf8]
BitField   : [0x02][4B piece_count LE][N bytes bitmap]   // N = ceil(piece_count/8)
Request    : [0x03][4B piece_index LE]
Piece      : [0x04][4B piece_index LE][4B offset LE][N bytes data]
HAVE       : [0x05][4B piece_index LE]
Cancel     : [0x06][4B piece_index LE]
Disconnect : [0x07]
```

All multi-byte integers are little-endian.

#### Peer state machine

```
IDLE  --xP2P connected--> HANDSHAKE --recv Handshake--> BITFIELD --recv BitField--> ACTIVE
                                                                              |
DEAD  <-- disconnect/timeout/error -- (any state)                             |
                                                                              |
                                                                      recv Piece -> on_data/on_done
                                                                      recv HAVE  -> update peer->bitfield
```

#### Internal tick (1000ms, driven by scheduler)

```
p2p_internal_tick(src):  // called from P2P source's own timer (TTL from Tracker)
    1. PUT /peer/:peer_id/seed to Tracker (announce + heartbeat)
       body: {"tracker_addr":"...", "add":[...], "del":[...]}
       response: {"status":"ok", "ttl_ms":5000, "signals":[...]}
    2. Process signals (offer/answer/candidate):
       setRemoteDescription / addIceCandidate
    3. If we have signals to send:
       POST /relay {peer_id:"...", signal:{type:"answer",from:"...",to:"..."}}
    4. If connected < max_peers:
       GET /file/:file_id/peer → initiate WebRTC connections to new peers
    5. Clean up timed-out peers (last_recv > 30s)
    6. Re-schedule timer to new ttl_ms from step 1 response
    7. On task stop: DELETE /peer/:peer_id/seed (graceful departure)
    8. Log stats
```

#### fetch(offset, len, on_data, on_done) implementation

```
p2p_source_fetch(task, offset, len, on_data, on_done):
    piece_index = offset / PIECE_SIZE

    for peer in peers (ACTIVE):
        if peer->bitfield[piece_index] && peer->reqs_pending < 4:
            peer->reqs_pending++
            send Request{piece_index} via dc
            register on_data, on_done callbacks for this request
            return

    // no available peer -> push to pending queue
    // internal tick will retry this request on next cycle
```

On Piece message received:
1. `on_data(data, len)` — SHA1 update + cache write
2. `on_done(offset, len, ok=true)` — notify scheduler
3. peer->reqs_pending--
4. send HAVE{piece_index} to all other ACTIVE peers
5. next pending request dispatched from queue

On peer disconnect or Request timeout: `on_done(offset, len, ok=false)` with retry counter. Scheduler requeues this block, next tick re-dispatches to another peer.

### Seek API

```c
int xdl_task_seek(xdl_task_t *task, uint64_t offset);
```

Sets the current playback position. Returns 0 on success, -1 if the task is stopped/destroyed. Schedulers read `task->seek_point` on every tick. `offset = 0` (default) means sequential download.

Edge cases:
- `offset > total_size`: clamped to `total_size - 1` (or 0 if unknown)
- Task not started: seek is queued and applied on next `xdl_task_start`
- Seek into already-downloaded region: no duplicate requests; scheduler skips completed blocks

### Pause/Resume

```c
int xdl_task_pause(xdl_task_t *task);
int xdl_task_resume(xdl_task_t *task);
```

Lightweight pause: stops dispatch (`on_tick` becomes a no-op for this task) but keeps sources, peer connections, and checkpoint state intact. Use for mobile app backgrounding — cheaper than `stop` which tears down all connections and requires re-discovery on resume. `xdl_task_resume` re-enables dispatch on the next tick.

### Architecture layers

```
xdl_task_create("url", "dest", sha1)
        |
        v
    xdl_task_t
        |
  ┌─────┴───────┐
  │   schedule   │   tick-driven dispatch, seek-aware priority
  │   vtable     │   decides which ranges to fetch
  └─────┬───────┘
        | fetch(offset, len)
  ┌─────┴───────┐
  │   source     │   protocol-specific I/O (HTTP GET, P2P piece request)
  │   vtable     │   manages its own concurrency internally
  └─────┬───────┘
        | write(offset, data, len) / read(offset, len)
  ┌─────┴───────┐
  │   cache      │   ring buffer (optional) between source and storage
  │   vtable     │   direct: passthrough; ring: FIFO eviction
  └─────┬───────┘
        | write(offset, data, len) / read(offset, len)
  ┌─────┴───────┐
  │   storage    │   async file I/O via xFsReqSubmit
  │   vtable     │   .part temporary file, rename on completion
  └─────────────┘
```

### Task state

```c
struct xdl_task {
    char     *url, *dest, *sha1_hex;
    long      timeout_ms;
    uint64_t  seek_point;     // current playback position (0 = sequential)

    xSha1Ctx    sha1_ctx;
    dlp_block_t **blocks;
    xRelay     *progress_relay;
    int         phase;

    struct xdl_schedule_vtable *sched;
    struct xdl_cache_vtable    *cache;
    void *sched_state;
    void *cache_state;
};
```

### Scheduler vtable — tick-driven

```c
struct xdl_schedule_vtable {
    void  (*on_start)     (xdl_task_t *task);
    void  (*on_range_done)(xdl_task_t *task, uint64_t offset, size_t len, bool ok);
    void  (*on_tick)      (xdl_task_t *task);
    void  (*on_stop)      (xdl_task_t *task);
    long   tick_ms
};
```

`on_tick` is the unified driver. Every `tick_ms` (1000ms default), the scheduler:

1. Scans all tasks' pending blocks
2. For each source with a free concurrency slot, picks the highest-priority block and calls `source->fetch()`
3. Within the seek window `[seek-2MB, seek+8MB]`: blocks assigned to HTTP source; outside: P2P source

`on_range_done` records completion (bitmap update, meta_save, relay emit). For the HTTP-only scheduler, `on_range_done` MAY immediately dispatch the next pending block to avoid tick-gap throughput loss (no tick required because there's no P2P coordination to consider). The hybrid scheduler keeps tick-only dispatch to prevent source-selection oscillation.

```c
// HTTP scheduler (single source, tick_ms = 1000, immediate dispatch on on_range_done):
xdl_schedule_vtable_t *xdl_schedule_http(xdl_source_vtable_t *http);

// Hybrid scheduler (tick_ms = 1000):
//   - Blocks within [seek - pre, seek + post] go to HTTP source
//   - Blocks outside the window go to P2P source
//   - Pure sequential download: pre=0, post=SIZE_MAX (entire file via HTTP)
//   - Stream playback: pre=2MB, post=8MB (seek-close blocks via HTTP)
//   - min_p2p_slots: reserved P2P concurrency (default 4) — prevents HTTP from
//     consuming all global slots and starving P2P
xdl_schedule_vtable_t *xdl_schedule_hybrid(
    xdl_source_vtable_t *http, xdl_source_vtable_t *p2p,
    uint64_t pre_bytes, uint64_t post_bytes, int min_p2p_slots);
```

Scheduler state (per-task in `sched_state`):

```c
// HTTP
struct http_sched_state {
    xdl_source_vtable_t *http;
    xdl_block_t *pending_head, *pending_tail;
    int in_flight, max_concurrency;
};

// Hybrid
struct hybrid_sched_state {
    xdl_source_vtable_t *http, *p2p;
    uint64_t pre_bytes, post_bytes;
    int in_flight_http, in_flight_p2p;
    int min_p2p_slots;
};
```

### Source vtable — protocol I/O

```c
struct xdl_source_vtable {
    const char *name;

    void (*open)(xdl_task_t *task);
    bool (*has_free_slot)(xdl_source_vtable_t *src);

    void (*fetch)(xdl_task_t *task, uint64_t offset, size_t len,
                  void (*on_data)(xdl_task_t *task, const uint8_t *data, size_t len),
                  void (*on_done)(xdl_task_t *task, uint64_t offset, size_t len, bool ok));

    void (*close)(xdl_task_t *task);
};

xdl_source_vtable_t *xdl_source_http(const char *url, long timeout_ms);
xdl_source_vtable_t *xdl_source_p2p(const xdl_p2p_conf_t *conf);
```

Each source manages its own concurrency internally. HTTP source uses `in_flight` and curl multi handle. P2P source manages peer connections and piece request queues. The scheduler and source communicate only through `fetch(on_data, on_done)` — the source calls back when data arrives.

```
sched->on_tick(task):
    window = [task->seek_point - pre_bytes, task->seek_point + post_bytes]

    for each pending block:
        if in_range(block, window):
            if http && http->has_free_slot():
                http->fetch(task, block.offset, block.len,
                            http_on_data, http_on_range_done)
        else:
            if p2p && p2p->has_free_slot():
                p2p->fetch(task, block.offset, block.len,
                           http_on_data, http_on_range_done)
            else if http && http->has_free_slot()
                 && (total_slots - in_flight_http) >= min_p2p_slots:
                // P2P has no free slot but reservation is satisfied —
                // spill window-exterior blocks to HTTP
                http->fetch(task, block.offset, block.len,
                            http_on_data, http_on_range_done)

http_on_range_done(task, offset, len, ok):
    block_mark_range(...)     // update block bitmap
    meta_save(task)            // persist
    xRelayEmit(progress)       // notify
    task->sched->on_range_done(task, offset, len, ok)
    // HTTP-only scheduler: may immediately try_dispatch() here
    // Hybrid scheduler: waits for next tick
```

### Cache vtable

```c
struct xdl_cache_vtable {
    void (*read)(xdl_task_t *task, uint64_t offset, size_t len,
                 void (*done)(xdl_task_t *task, const uint8_t *data, size_t len));
    void (*write)(xdl_task_t *task, uint64_t offset,
                  const uint8_t *data, size_t len,
                  void (*done)(xdl_task_t *task, xErrno err));
    void (*flush)(xdl_task_t *task);
};

// Passthrough — writes go directly to storage
xdl_cache_vtable_t *xdl_cache_direct(xdl_storage_vtable_t *storage);

// Ring buffer — recent pieces cached for P2P seed reads
xdl_cache_vtable_t *xdl_cache_ring(xdl_storage_vtable_t *storage, size_t ring_bytes);
```

### Storage vtable

```c
struct xdl_storage_vtable {
    void (*open)(xdl_task_t *task);
    void (*read)(xdl_task_t *task, uint64_t offset, size_t len,
                 void (*done)(xdl_task_t *task, const uint8_t *data, size_t len));
    void (*write)(xdl_task_t *task, uint64_t offset,
                  const uint8_t *data, size_t len,
                  void (*done)(xdl_task_t *task, xErrno err));
    void (*finalize)(xdl_task_t *task, bool success);
};

xdl_storage_vtable_t *xdl_storage_file(void);
```

### Progress dispatch: internal relay per task

```c
xdl_task_create(conf):
    t->progress_relay = xRelayCreate()
    xRelayOn(t->progress_relay, (xRelayFunc)conf->cb, conf->arg)

xRelayEmit(t->progress_relay, &progress, sizeof(progress))
```

### SHA1 verification

SHA1 context lives in `task->sha1_ctx`. `on_data` calls `xSha1Update` per chunk. `xSha1Final` runs once when all blocks complete, compared against expected hash. On resume, existing bytes are re-read via `cache->read` and fed to `xSha1Update` before downloading new blocks.

### Checkpoint format: `.meta` file

```
Offset  Size  Field
[0]     16    Magic: "XDL_META_V1\0\0\0\0\0"
[16]     4    block_count (uint32 LE)
[20]     4    block_size  (uint32 LE) = 262144
[24]     8    total_size  (uint64 LE)
[32]    N*32  piece bitmaps (256 bits per block, 1 bit per 1 KB piece)
```

Blocks are lazily allocated. On resume: `meta_load` reads the bitmap, sets `done` flag per block, and the download skips completed blocks.

### Temp file: write to `.part`, rename on completion

Data is written to `<dest>.part` (`.meta` alongside at `<dest>.meta`). On success: `rename(part, dest)` + `meta_delete`. Resume always checks `.part`.

### No Content-Length fallback

If the server omits `Content-Length`, the download falls back to a single GET request without Range-based blocks. No checkpoint/resume.

### Retry: max 3 attempts, 403/404 skip

HTTP 403 and 404 are not retried. All other failures retry up to 3 times per block.

### Thread safety

| API | Strategy |
|-----|----------|
| `xdl_task_create` | Heap allocation + `xRelayCreate`, no loop access |
| `xdl_task_start` | `xEventLoopPost(loop, do_start, task)` |
| `xdl_task_stop` | `xEventLoopPost(loop, do_stop, task)` |
| `xdl_task_destroy` | Post -> completion flag -> free |

`xEventLoopPost` bridges caller thread to loop thread (MPSC done queue).

## Risks / Trade-offs

| Risk | Mitigation |
|------|-----------|
| Tick-driven dispatch may add latency before first block request | Timer fires immediately after `xdl_task_start`, then every 1000ms |
| Re-reading partial file on resume may block event loop | Use `xWorkSubmit` for re-hash if files > 50 MB |
| `.meta` corruption after crash | Header validation catches mismatches -> delete `.meta`, start fresh |
| Unit tests need real network timing | Use timeouts and progress counters. Mock HTTP callbacks for offline tests. |
| Malicious peer sends fake BitField to drain request slots | Future: rate-limit how many consecutive timeouts a peer can cause before marking DEAD. MVP accepts the waste; SHA1 catches bogus data. |
| Tracker has no auth or rate limiting | MVP: trusted LAN/private network. Production: add shared-secret auth header to `PUT /peer/:peer_id/seed`. |
| DataChannel messages from unverified peers | Handshake file_id validation is the only gate — sufficient for closed groups, insufficient for open deployment. |

## Open Questions

None remaining.
