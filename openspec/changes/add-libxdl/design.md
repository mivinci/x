## Context

libdlproxy has a battle-tested download engine: block bitmaps with checkpoint persistence, streaming writes through `xFsReqSubmit`, Range-based HTTP fetching with a sliding-window scheduler. This design extracts that engine into a standalone library (`libxdl`) that handles one file per task, aligning with BitTorrent conventions for the torrent file format and content addressing.

## Goals / Non-Goals

**Goals:**
- Task-based API: `xdl_task_create({.magnet = "magnet:?...", ...})` -> `start` / `pause` / `resume` / `stop` / `destroy`
- Magnet URI as the user-facing identifier; `.torrent` file is an internal artifact fetched from the Tracker
- Seek-driven download via `xdl_task_seek(task, offset)`
- Checkpoint/resume via `.resume` file (block bitmap, separate from the read-only `.torrent`)
- Per-block SHA1 verification (streaming `xSha1Update` per received data, verified against `blocks` hashes from torrent)
- Tick-driven scheduling — unified driver for HTTP and P2P
- Pluggable sources: HTTP (`xdl_source_http`) and P2P (`xdl_source_p2p`) share the same vtable
- Per-task timeout (`conf.timeout_ms`) passed to HTTP client
- `.part` temp file with atomic rename on completion
- Torrent file format: BitTorrent-compatible bencoding
- Content addressing via `info_hash` = SHA1(bencode(info dict))
- Progress via internal `xRelay` per task
- Two run modes: `XDL_MODE_INHERIT` / `XDL_MODE_THREAD`
- CLI example

**Non-Goals:**
- Multi-file or group/batch operations (multi-file handled via zip at a higher layer)
- Bandwidth throttling or per-host concurrency limits
- HTTP authentication or custom headers
- Upload/sharing
- DHT (outsourced to Tracker for v1)

## Decisions

### Directory layout: `libxdl/xdl/` + `libxdl/examples/`

Mirrors `libdlproxy/dlproxy/` + `libdlproxy/examples/`.

### Dependencies

```
xdl
├── xhttp   (curl-based HTTP client)
├── xfs     (async file I/O)
├── xcrypto (SHA1)
├── xp2p    (WebRTC: DataChannel for P2P block transfer)
├── x/json  (tracker response parsing, SDP signaling)
└── xbase   (xArray, xMap, xList, xHeap — data structures shared across all modules)
```

Additional internal modules:
- `bencode.h/c` — Lightweight bencoding parser/writer (~200 lines) for `.torrent` files. Supports strings, integers, lists, and dictionaries.
- `magnet.h/c` — Magnet URI parser (~100 lines). Parses `magnet:?xt=urn:btih:<info_hash>&dn=<name>&tr=<tracker_url>`. Extracts info_hash (hex→bytes), display name, and tracker list.

Internal data structures reuse libx containers:
- `xMap` — peer_id → p2p_peer lookup (hash table, string keys)
- `xArray` — dynamically sized peer list, pending request queues
- `xList` — intrusive linked list for per-block retry/pending chains
- `xHeap` — priority queue for seek-window block scheduling (if needed)
- `x/json` — parse tracker announce responses, SDP exchange payloads

### Naming: `xdl_` prefix, snake_case

`xdl_init`, `xdl_task_create`. The `x` prefix signals this is part of the libx ecosystem.

### Run modes: `XDL_MODE_INHERIT` / `XDL_MODE_THREAD`

Same pattern as libdlproxy:
- `INHERIT`: reuse caller's `xEventLoopCurrent()`. Requires caller to pump the loop.
- `THREAD`: `xdl_init()` spawns a dedicated worker thread that blocks on `xEventLoopRun(RUN_DEFAULT)`, and starts a global tick timer (1000ms) that iterates all active tasks and calls each `task->sched->on_tick(task)`.

First version ships `XDL_MODE_THREAD` only.

### Global P2P module

`xdl_init` creates a single global P2P module that manages the peer identity and Tracker communication for all tasks:

```c
struct xdl_p2p {
    char    peer_id[33];        // user-provided via xdl_conf (max 32 chars + NUL)
    char   *tracker_url;        // from xdl_conf
    int     announce_ttl_ms;    // server-assigned TTL

    xMap   *sources;            // info_hash → xdl_source_p2p (for signal routing)
    xTimer *announce_timer;     // global timer, not per-task
};
```

**Key principle: peer_id is global.** A peer with 3 active downloads announces all 3 files in a single PUT, receiving signals for all of them in the response. This avoids redundant Tracker round-trips.

**Global announce tick** (fires every `announce_ttl_ms`):

```
p2p_global_tick():
    1. Collect all active P2P sources → [{info_hash, pct}, ...]
    2. PUT /announce to Tracker
       body: {"changes": [{info_hash, pct}, ...]}
       ← {status:"ok", ttl_ms, signals: [...]}
    3. Route signals to sources by info_hash:
       offer/answer/candidate → src → setRemoteDescription / addIceCandidate
    4. For each source with connected < max_peers:
       GET /torrent/<info_hash>/peer → initiate WebRTC connections
    5. Clean up timed-out peers (last_recv > 30s) per source
    6. Re-arm timer with new ttl_ms
```

**On task stop**: collect all remaining active sources' info hashes, send one final PUT with `pct: 0` for each.

### Announce protocol (`PUT /announce`)

```json
// Request
{"peer_id": "alice", "changes": [{"info_hash": "<hex>", "pct": 45.7}]}

// Pure heartbeat (no file changes)
{"peer_id": "alice"}

// Response
{"status": "ok", "ttl_ms": 5000, "signals": [{"type": "offer", "from": "bob", "sdp": "..."}]}
```

- `peer_id` identifies the announcing peer.
- `changes` array upserts: creates or updates the peer+file entry. `pct: 0.0` removes the peer from that file.
- `pct` is client-computed: `blocks_done / blocks_total * 100.0`. The server stores it as-is without validation.
- `signals` inbox delivers pending WebRTC signaling messages (offer/answer/candidate).
- `ttl_ms` is server-assigned; the peer reschedules its global announce timer accordingly.

### Global configuration: `xdl_conf_t`

```c
struct xdl_conf {
    int         mode;          // XDL_MODE_INHERIT or XDL_MODE_THREAD
    const char *peer_id;       // peer identifier (user-provided, e.g. from SaaS auth)
    const char *tracker_url;   // global fallback tracker URL (required for P2P)
};

int xdl_init(const xdl_conf_t *conf);
int xdl_destroy(void);
```

`peer_id` is caller-provided. v1 treats it as an opaque string (max 32 bytes) with no collision prevention. `tracker_url` is used as the Tracker URL when a magnet URI does not include `tr` parameters. Required when any task uses `magnet`; optional for HTTP-only tasks.

### Task configuration: `xdl_task_conf_t`

```c
typedef void (*xdl_task_cb_t)(const xdl_progress *p, void *arg);

struct xdl_task_conf {
    const char    *magnet;       // magnet URI (optional if urls or torrent is set)
    xdl_torrent_t *torrent;      // direct torrent reference (optional, useful for tests)
    const char    *urls;         // CDN HTTP URLs, semicolon-separated (optional)
    const char    *dir;          // output directory
    long           timeout_ms;   // default 30000
    int            max_peers;    // max P2P connections, default 8
    xdl_task_cb_t  cb;
    void          *arg;
};

xdl_task_t xdl_task_create(const xdl_task_conf_t *conf);
```

At least one of `magnet`, `torrent`, or `urls` MUST be provided. The scheduler is determined by which fields are set:

| magnet | torrent | urls | Scheduler | Notes |
|--------|---------|------|-----------|-------|
| set | — | — | P2P | Fetches `.torrent` from Tracker |
| — | set | — | P2P | Uses provided torrent directly, no Tracker needed |
| set | — | set | Hybrid | HTTP from CDN URLs, P2P from magnet |
| — | set | set | Hybrid | HTTP from CDN URLs, P2P from provided torrent |
| — | — | set | HTTP-only | Downloads directly from CDN, no Tracker |
| — | — | — | — | Error |

`torrent` takes precedence over `magnet` for torrent metadata. When both are provided, `torrent` is used and `magnet` is ignored. This allows tests to inject a torrent without setting up a Tracker.

HTTP source URL priority:
1. `conf.urls` (caller-provided)
2. Torrent `url-list` (from `.torrent`, BEP-19 WebSeed)

**Magnet URI format** (BitTorrent-compatible):

```
magnet:?xt=urn:btih:<info_hash_hex>&dn=<display_name>&tr=<tracker_url>&tr=<tracker_url2>
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `xt` | Yes | Exact topic: `urn:btih:<info_hash>` (40-char hex) |
| `dn` | No | Display name (for progress/logging, overridden by torrent `info.name`) |
| `tr` | No | Tracker URL(s). Multiple `tr` parameters for redundancy. |

**Flow on `xdl_task_start`**:

```
xdl_task_start(task):

    # Source determination:
    if urls:
        if magnet:
            scheduler = Hybrid  (HTTP source = CDN URLs, P2P source = magnet)
        else:
            scheduler = HTTP-only (CDN URLs only, no tracker needed)
    else:
        scheduler = P2P-only (magnet, fetch .torrent from Tracker)

    # P2P path (when magnet is set):
    1. magnet_parse(magnet_uri) → info_hash, display_name, tracker_list
    2. tracker = pick_tracker(tracker_list, xdl_conf.tracker_url)
    3. GET /torrent/<info_hash_hex> from tracker → raw bencoded .torrent
    4. bencode_parse(torrent) → name, block_size, file_size, block_hashes, announce

    # HTTP path (when urls is set):
    5. Parse URLs: urls → [url1, url2, ...] (split by ';')

    # Common:
    6. resume_load() → restore block bitmap if valid
    7. Start scheduler tick
```

### Torrent struct (`xdl_torrent_t`)

Plain C struct, no opaque handle. Callers populate fields directly for programmatic use / testing:

```c
typedef struct {
    char     *name;          // file name
    uint64_t  length;        // total file size in bytes
    uint32_t  block_length;  // block size, default 262144 (256KB)
    uint32_t  block_count;   // ceil(length / block_length)
    uint8_t  *block_hashes;  // block_count × 20 bytes SHA1
    char     *announce;      // tracker URL (optional)
    char    **http_urls;     // CDN URLs (optional)
    int       http_url_count;
} xdl_torrent_t;

// From raw bencoded data (allocates, caller must xdl_torrent_destroy)
XCAPI(xdl_torrent_t *) xdl_torrent_parse(const uint8_t *data, size_t len);

// Free memory allocated by xdl_torrent_parse
XCAPI(void) xdl_torrent_destroy(xdl_torrent_t *t);
```

Stack-allocated for tests:

```c
uint8_t hashes[80]; // 4 blocks × 20 bytes

xdl_torrent_t t = {
    .name = "test.bin",
    .length = 1048576,
    .block_length = 262144,
    .block_count = 4,
    .block_hashes = hashes,
    .announce = "http://localhost:8080",
};

xdl_task_create(&(xdl_task_conf_t){ .torrent = &t, .dir = "/tmp" });
```

### Torrent file format (`.torrent`) — internal to libxdl

The bencoded `.torrent` file is the serialized form. Callers normally don't create it — it's published to the Tracker or parsed via `xdl_torrent_parse`:

```
d
  8:announce  <tracker_url>
  4:info      d
                  4:name          <file_name>
                  12:block length i<block_size>e          // e.g. i262144e (256KB)
                  6:blocks        <N*20 bytes SHA1>       // concatenated SHA1 of each block
                  6:length        i<total_size>e
                  8:url-list      l
                      <http_url>                          // optional, WebSeed (BEP-19)
                  e
              e
e
```

Rules:
- `info_hash` = SHA1(bencode(info dict)).
- `block length` — block size in bytes (default 262144 = 256KB). Each block has one SHA1 hash in `blocks`.
- `blocks` = concatenation of per-block SHA1 hashes (20 bytes each). Block count = `ceil(length / block_length)`.
- Piece size is fixed at 16KB (16384 bytes). A block is divided into `block_length / 16384` pieces. Not stored in torrent metadata.
- `url-list` — optional HTTP CDN URLs, BitTorrent-compatible (BEP-19 WebSeed). Used as HTTP fallback when `conf.urls` is not set.
- The `.torrent` file is read-only after publishing. libxdl fetches it, parses it, and never modifies it.

Usage:

```c
// P2P-only
task = xdl_task_create(&(xdl_task_conf_t){
    .magnet = "magnet:?xt=urn:btih:a1b2c3...&dn=ubuntu.iso&tr=http://t1:8080",
    .dir = "./dl", .max_peers = 8, .cb = on_progress
});

// Hybrid HTTP+P2P (magnet + CDN URLs)
task = xdl_task_create(&(xdl_task_conf_t){
    .magnet = "magnet:?xt=urn:btih:a1b2c3...&tr=http://t1:8080",
    .urls = "https://cdn1.example.com/ubuntu.iso;https://cdn2.example.com/ubuntu.iso",
    .dir = "./dl", .max_peers = 8, .cb = on_progress
});

// HTTP-only (CDN download, no tracker)
task = xdl_task_create(&(xdl_task_conf_t){
    .urls = "https://cdn.example.com/ubuntu.iso",
    .dir = "./dl", .cb = on_progress
});
```

### P2P source (per-task): DataChannel data transfer

Each task has its own `xdl_source_p2p` that handles **data transfer only**. Announce, peer discovery, and signaling are handled by the global P2P module.

```c
// Internal config (not exposed to user)
struct xdl_p2p_conf {
    uint8_t  info_hash[20];   // SHA1(bencode(info)) — file identifier
    int      max_peers;
};

xdl_source_vtable_t *xdl_source_p2p(const xdl_p2p_conf_t *conf);
```

#### Internal state

```c
struct p2p_peer {
    xPeerConnection  pc;
    xDataChannel     dc;
    uint8_t         *bitfield;      // copy of peer's block bitmap
    int              reqs_pending;  // in-flight block requests to this peer
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
Handshake  : [0x01][20B info_hash][32B peer_id_utf8]
BitField   : [0x02][4B block_count LE][N bytes bitmap]   // N = ceil(block_count/8)
Request    : [0x03][4B block_index LE][4B offset LE][4B length LE]
Block      : [0x04][4B block_index LE][4B offset LE][N bytes data]
HAVE       : [0x05][4B block_index LE]
Cancel     : [0x06][4B block_index LE]
Disconnect : [0x07]
```

All multi-byte integers are little-endian. `Request` and `Block` use `offset` + `length` to specify a byte range within a block — this enables a single block to be fetched from multiple peers in parallel pieces.

Block is the scheduler's unit of dispatch: the scheduler assigns an entire block to a source (HTTP or P2P). The P2P source internally decomposes the block fetch into piece-level requests across peers.

#### PeerConnection and DataChannel lifecycle

PeerConnections are managed by the global P2P module, not per-source. One peer pair shares one PeerConnection with multiple DataChannels (one per torrent).

```
Global P2P module state:
  peers:   xMap[peer_id → xPeerConnection]     // at most one per remote peer
  sources: xMap[info_hash → p2p_source]         // info_hash → owning source

PeerConnection establishment (called from global tick):

  p2p_ensure_peer(remote_peer_id, info_hash):
    if peers[remote_peer_id] exists:
      → pc = peers[remote_peer_id]
      → xDataChannelCreate(pc, label = info_hash)   // no new ICE needed
    else:
      → pc = xPeerConnectionCreate()
      → xDataChannelCreate(pc, label = info_hash, reliable + ordered)
      → offer = xPeerConnectionCreateOffer(pc)
      → xPeerConnectionSetLocalDescription(pc, offer)
      → POST /relay {peer_id: remote, signal: {type:"offer", from:peer_id, sdp:offer}}
      → peers[remote_peer_id] = pc  (tentative, confirmed on connected)

Signal handling (from PUT response inbox):

  p2p_handle_signal(signal):
    {"type":"offer", "from":"bob", "sdp":"..."}:
      → pc = peers["bob"] or xPeerConnectionCreate()
      → xPeerConnectionSetRemoteDescription(pc, signal.sdp)    // offer
      → answer = xPeerConnectionCreateAnswer(pc)
      → xPeerConnectionSetLocalDescription(pc, answer)
      → POST /relay {peer_id:"bob", signal:{type:"answer", from:peer_id, sdp:answer}}
      → peers["bob"] = pc

    {"type":"answer", "from":"bob", "sdp":"..."}:
      → xPeerConnectionSetRemoteDescription(peers["bob"], signal.sdp)  // answer

    {"type":"candidate", "from":"bob", "candidate":"...", ...}:
      → xPeerConnectionAddIceCandidate(peers["bob"], signal)

ICE + DTLS complete → pc state = connected:

  ondatachannel callback (receiving side):
    → dc label = info_hash
    → src = p2p_sources[info_hash]
    → src->on_dc_ready(src, dc)

  dc->onopen callback (initiating side, DataChannel created locally):
    → same routing: dc label → info_hash → source → on_dc_ready

  on_dc_ready:
    → send Handshake[info_hash][peer_id]
    → receive BitField → peer state → ACTIVE
    → ready for block requests
```

DataChannel labels are the `info_hash` hex string (40 chars). This is the key that routes incoming DataChannels to their owning P2P source.

PeerConnection reuse is the key optimization: if Alice and Bob already have a connection from torrent "def", adding torrent "abc" only creates a new DataChannel on the existing connection — no ICE, no DTLS, no signaling round-trips.

#### Peer state machine

```
IDLE  --xP2P connected--> HANDSHAKE --recv Handshake--> BITFIELD --recv BitField--> ACTIVE
                                                                              |
DEAD  <-- disconnect/timeout/error -- (any state)                             |
                                                                              |
                                                                      recv Block -> on_data/on_done
                                                                      recv HAVE  -> update peer->bitfield
```

#### Data transfer (per source)

Announce, peer discovery, and signaling are handled by the global P2P module's tick (see above). The per-task P2P source only handles data transfer and peer BitField tracking:

#### fetch(offset, len, on_data, on_done) implementation

```
p2p_source_fetch(task, block_offset, block_len, on_data, on_done):
    block_index = block_offset / BLOCK_SIZE

    // Decompose block into piece-level requests
    for offset = 0; offset < block_len; offset += PIECE_SIZE:
        length = min(PIECE_SIZE, block_len - offset)

        for peer in peers (ACTIVE):
            if peer->bitfield[block_index] && peer->reqs_pending < 4:
                peer->reqs_pending++
                send Request{block_index, offset, length} via dc
                register on_data, on_done callbacks for this piece
                break

        // no available peer → this piece stays pending
        // internal tick will retry on next cycle
```

On Block message received:
1. `on_data(data, len)` — SHA1 update + cache write
2. Track received pieces for this block. When all pieces received:
   - `xSha1Final` → compare against `block_hashes[block_index]`
   - If match: `on_done(block_offset, block_len, ok=true)` — notify scheduler
   - If mismatch: re-queue block for retry
3. peer->reqs_pending--
4. send HAVE{block_index} to all other ACTIVE peers (SHA1 verified)
5. next pending piece dispatched from queue

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
xdl_task_create({.magnet = "magnet:?...", .dir = ".", ...})
        |
        | magnet_parse → info_hash
        v
    xdl_task_t
        |
        | xdl_task_start: GET /torrent/:info_hash → parse .torrent
        v
        |
  ┌─────┴───────┐
  │   schedule   │   tick-driven dispatch, seek-aware priority
  │   vtable     │   decides which ranges to fetch
  └─────┬───────┘
        | fetch(offset, len)
  ┌─────┴───────┐
  │   source     │   protocol-specific I/O (HTTP GET, P2P block request)
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
    uint8_t  info_hash[20];       // SHA1(bencode(info))
    char    *name;                // from info.name
    uint64_t file_size;           // from info.length
    uint32_t block_size;          // from info.block length
    uint32_t piece_size;          // fixed 16384 (16KB)
    uint32_t block_count;         // computed from file_size / block_size
    uint8_t *block_hashes;        // decoded from info.blocks (block_count × 20 bytes)
    uint8_t *block_bitmap;        // from .resume, ceil(block_count/8) bytes
    char   **urls;                // from conf.urls (CDN fallback URLs)

    long      timeout_ms;
    uint64_t  seek_point;         // current playback position (0 = sequential)

    xSha1Ctx    sha1_ctx;         // per-block verification context
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
    void  (*on_block_done)(xdl_task_t *task, uint64_t offset, size_t len, bool ok);
    void  (*on_tick)      (xdl_task_t *task);
    void  (*on_stop)      (xdl_task_t *task);
    long   tick_ms;
};
```

`on_tick` is the unified driver. Every `tick_ms` (1000ms default), the scheduler:

1. Scans all tasks' pending blocks
2. For each source with a free concurrency slot, picks the highest-priority block and calls `source->fetch()`
3. Within the seek window `[seek-2MB, seek+8MB]`: blocks assigned to HTTP source; outside: P2P source

`on_block_done` records completion (bitmap update, resume save, relay emit). For the HTTP-only scheduler, `on_block_done` MAY immediately dispatch the next pending block to avoid tick-gap throughput loss (no tick required because there's no P2P coordination to consider). The hybrid scheduler keeps tick-only dispatch to prevent source-selection oscillation.

```c
// HTTP scheduler (single source, tick_ms = 1000, immediate dispatch on on_block_done):
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

Each source manages its own concurrency internally. HTTP source uses `in_flight` and curl multi handle. P2P source manages peer connections and block request queues. The scheduler and source communicate only through `fetch(on_data, on_done)` — the source calls back when data arrives.

```
sched->on_tick(task):
    window = [task->seek_point - pre_bytes, task->seek_point + post_bytes]

    for each pending block:
        if in_range(block, window):
            if http && http->has_free_slot():
                http->fetch(task, block.offset, block.len,
                            http_on_data, http_on_block_done)
        else:
            if p2p && p2p->has_free_slot():
                p2p->fetch(task, block.offset, block.len,
                           http_on_data, http_on_block_done)
            else if http && http->has_free_slot()
                 && (total_slots - in_flight_http) >= min_p2p_slots:
                // P2P has no free slot but reservation is satisfied —
                // spill window-exterior blocks to HTTP
                http->fetch(task, block.offset, block.len,
                            http_on_data, http_on_block_done)

http_on_block_done(task, offset, len, ok):
    block_mark_done(...)           // update block bitmap
    resume_save(task)               // persist bitmap to .resume
    xRelayEmit(progress)            // notify
    task->sched->on_block_done(task, offset, len, ok)
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

// Ring buffer — recent blocks cached for P2P seed reads
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

Per-block SHA1 verification using the hashes from `info.blocks`. `on_data` calls `xSha1Update` per received chunk. When a full block is received, `xSha1Final` runs and the result is compared against `task->block_hashes[block_index]`.

Per-block verification enables:
- **Incremental verification**: each block is validated as it arrives, no need to wait for the full file.
- **P2P trust**: a malicious peer that sends garbage for one block does not invalidate the entire download. Only the bad block is re-requested.

### Block and piece granularity

Two levels of granularity serve different roles:

```
File:  ┌── block 0 (256KB) ──┬── block 1 (256KB) ──┬── block 2 (256KB) ──┐
       │ P0 P1 P2 ... P15     │ P0 P1 P2 ... P15     │ P0 P1 P2 ... P15     │
       └──────────────────────┴──────────────────────┴──────────────────────┘
        \_________  _________/                         1 piece = 16KB
                  \/
           block = scheduler 的最小调度单位:
             • on_tick: "block 0-2 给 HTTP, block 3-5 给 P2P"
             • on_block_done(block_offset, block_len, ok)
             • progress: blocks_done / blocks_total
             • SHA1 验证: per-block, hash 来自 info.blocks

           piece = P2P DataChannel 的传输单位 (默认 16KB):
             • block 3 (256KB) = 16 个 piece
             • Peer A 有 piece 0-7   → 发 8 个 Request
             • Peer B 有 piece 8-15  → 发 8 个 Request
             • 拼回来 → SHA1 校验 → HAVE{block_3}
             • 不同 peer 贡献同一个 block 的不同 piece
```

The scheduler only thinks in blocks. The P2P source internally decomposes a block fetch into piece-level requests across peers. The HTTP source fetches an entire block in one Range request.

### Torrent file (`.torrent`) — internal artifact

The `.torrent` file uses **bencoding**. It is an internal artifact — callers never create, provide, or read it directly. The lifecycle:

1. **A seeder publishes** the `.torrent` to the Tracker via `POST /torrent` (raw bencoded body).
2. **A downloader calls** `xdl_task_create(&{.magnet = "magnet:?xt=urn:btih:<hex>"})`.
3. **libxdl resolves**: `GET /torrent/<info_hash>` from the Tracker → parses the `.torrent` → extracts all metadata.

The torrent file format (internal):

```
bencoding:
  d
    8:announce  <tracker_url>
    4:info      d
                    4:name          <suggested_file_name>
                    12:block length i<block_size>e          // default i262144e (256KB)
                    6:blocks        <N*20 bytes>            // concatenated SHA1 of each block
                    6:length        i<total_file_size>e
                    8:url-list      l
                        <http_url>
                    e                                       // optional, WebSeed (BEP-19)
                e
  e
```

Key design points:
- **`info_hash`** = SHA1(bencode(info)) — this is the content-addressed file identifier, used everywhere: Tracker announce, DataChannel Handshake, peer discovery. No explicit `file_id` string.
- **`blocks`** = concatenation of per-block SHA1 hashes (20 bytes each). Block count = `ceil(length / block_length)`. Enables incremental per-block verification during P2P download.
- **`url-list`** = optional HTTP CDN URLs, BitTorrent-compatible (BEP-19 WebSeed). Used as HTTP fallback when `conf.urls` is not set.
- The `.torrent` file is **read-only** after publishing. It is the canonical seed metadata — pass it to any peer and they know what to download, where to find peers, and how to verify.

### Resume file (`.resume`)

The `.resume` file stores the downloader's block completion bitmap, **separate from the read-only `.torrent`**:

```
.resume file (binary):
  Magic:     "XDL_RESUME_V1\0\0"               (16B)
  info_hash: uint8_t[20]                        (20B SHA1 — identify which torrent this belongs to)
  block_count: uint32 LE                        (4B)
  bitmap:     N bytes                            (N = ceil(block_count/8), 1 bit per block)
```

The `.resume` file:
- Lives alongside the `.part` file: `<dest>.part.resume`
- Is written by the downloader as blocks complete
- On task start: if `.resume` exists and `info_hash` matches, resume from the bitmap. Otherwise start fresh.
- On download completion: deleted together with `.part` rename to `<dest>`.

This decouples the seed metadata (`.torrent`, read-only, shareable) from the downloader's progress state (`.resume`, mutable, per-downloader).

### Temp file: write to `.part`, rename on completion

Data is written to `<dest>.part`. The resume file (`<dest>.part.resume`) tracks progress. On success: `rename(part, dest)` and delete the `.resume` file. Resume always checks `.part` + `.resume`.

### Torrent management (Tracker API)

The Tracker hosts torrent metadata so downloaders can resolve a magnet URI to a `.torrent` file:

```
POST /torrent             — publish a torrent file (bencoded body)
GET  /torrent/:info_hash  — retrieve a torrent file by info_hash (40-char hex)
```

A seeder publishes the torrent once. N downloaders call `xdl_task_create(&{.magnet = "magnet:?xt=urn:btih:<hex>"})` — libxdl internally calls `GET /torrent/<hex>` to fetch the torrent before starting the download.

The `PUT /announce` announce references `info_hash` for peer discovery (separate from torrent retrieval).

### Tracker configuration

```c
struct xdl_tracker_conf {
    uint16_t  port;                 // default 8080
    int       default_ttl_ms;       // default 5000
    int       message_ttl_ms;       // default 30000
    int       cleanup_interval_ms;  // default 1000
    int       max_inbox_per_peer;   // default 256
};
```

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
| `.resume` corruption after crash | Magic + info_hash validation catches mismatches -> delete `.resume`, start fresh |
| Unit tests need real network timing | Use timeouts and progress counters. Mock HTTP callbacks for offline tests. |
| Malicious peer sends fake BitField to drain request slots | Future: rate-limit how many consecutive timeouts a peer can cause before marking DEAD. MVP accepts the waste; SHA1 catches bogus data. |
| Tracker has no auth or rate limiting | MVP: trusted LAN/private network. Production: add shared-secret auth header to `PUT /announce`. |
| DataChannel messages from unverified peers | Handshake info_hash validation is the only gate — sufficient for closed groups, insufficient for open deployment. |
| Bencoding parser is a new internal module | Minimal implementation (~200 lines). Well-defined format with existing reference implementations. |
| No Content-Length fallback for large files | Chunked-transfer files above ~50 MB will use single GET, blocking the event loop and losing resume capability. Consider `xWorkSubmit` or streaming-write-to-disk for files exceeding a size threshold. |

## Open Questions

None remaining.
