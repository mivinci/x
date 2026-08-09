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
- `xMap` — peer_id → xdl_peer lookup (hash table, string keys)
- `xArray` — dynamically sized peer list, pending request queues
- `xList` — intrusive linked list for per-block retry/pending chains
- `xHeap` — priority queue for seek-window block scheduling (if needed)
- `x/json` — parse tracker announce responses, SDP exchange payloads

### Naming: `xdl_` prefix, snake_case

`xdl_init`, `xdl_task_create`. The `x` prefix signals this is part of the libx ecosystem.

### Run modes: `XDL_MODE_INHERIT` / `XDL_MODE_THREAD`

- `INHERIT`: reuse caller's `xEventLoopCurrent()`. Requires caller to pump the loop.
- `THREAD`: `xdl_init()` spawns a dedicated worker thread that blocks on `xEventLoopRun(RUN_DEFAULT)`.

Per-task scheduling is driven independently — each task's scheduler registers its own tick timer via `xEventTimerStart` in `on_start`, and cancels it in `on_stop`. A global management timer (1000ms) handles housekeeping: task completion scanning, cleanup.

### Global P2P module

`xdl_init` creates a global P2P module with its own announce timer (independent of per-task scheduler ticks):

```c
typedef struct xdl_p2p     xdl_p2p_t;
typedef struct xdl_peer    xdl_peer_t;

struct xdl_p2p {
    char    peer_id[33];        // user-provided via xdl_conf (max 32 chars + NUL)
    char   *tracker_url;        // from xdl_conf
    int     announce_ttl_ms;    // server-assigned TTL

    xMap   *peers;              // remote_peer_id → xdl_peer_t
    xMap   *sources;            // info_hash → xdl_source_p2p (for signal routing)
    xTimer *announce_timer;     // global timer, not per-task
};

struct xdl_peer {
    xPeerConnection  *pc;       // one per remote peer (shared across torrents)
    xMap             *channels; // info_hash → xdl_peer_channel
};

struct xdl_peer_channel {
    xDataChannel  *dc;          // labeled by info_hash
    xBitmap        bitfield;    // peer's block bitmap for this torrent
    int            reqs_pending; // in-flight piece requests on this channel
    int            state;       // HELLO / BITFIELD / ACTIVE / DEAD
    uint64_t       last_recv_ms;
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
    int         mode;           // XDL_MODE_INHERIT or XDL_MODE_THREAD
    const char *peer_id;        // peer identifier (caller-provided)
    const char *tracker_url;    // global fallback tracker URL
    size_t      cache_bytes;    // ring buffer size, default 64MB, 0 = passthrough
    int         concurrency;    // global in-flight cap, default 32
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
    const char    *sha1;         // file-level SHA1 hex (40 chars, optional, HTTP-only)
    xdl_schedule_vtable_t *sched; // custom scheduler (optional, NULL = default)
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

**Responsibilities**:

```
xdl_task_create:  malloc task, parse magnet URI, create source instances
                  (xdl_source_http_create / xdl_source_p2p_create), create
                  scheduler (conf.sched or xdl_schedule_default_create).

xdl_task_start:   open sources (source->open → HEAD probe / p2p register),
                  resume_load from .resume, start scheduler
                  (sched->on_start → registers its own tick timer).
```

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

SHA1 verification has two modes:
- Per-block (magnet/torrent): `conf.torrent` or fetched `.torrent` provides `block_hashes`.
  Each block is verified against its SHA1 as it arrives.
- File-level (HTTP-only, urls only): `conf.sha1` provides a single hex string.
  `xSha1Final` runs once after the full file is downloaded. Optional — omitted if confidence is not required.

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

// HTTP-only with file-level SHA1 verification
task = xdl_task_create(&(xdl_task_conf_t){
    .urls = "https://cdn.example.com/ubuntu.iso",
    .sha1 = "a1b2c3d4e5f6...",          // optional, 40-char hex
    .dir = "./dl", .cb = on_progress
});
```

### P2P source (per-task): DataChannel data transfer

Each task has its own `xdl_source_p2p` that handles **data transfer only**. Announce, peer discovery, and signaling are handled by the global P2P module.

```c
struct xdl_p2p_conf {
    xdl_p2p_t *p2p;          // global P2P module (from xdl_state)
    uint8_t    info_hash[20];
    int        max_peers;
};

xdl_source_vtable_t *xdl_source_p2p_create(const xdl_p2p_conf_t *conf);
```

| Method | What it does |
|--------|-------------|
| `open(self)` | Register this source in the global P2P module keyed by `info_hash`. No network I/O — announce happens on next global tick. |
| `fetch(self, offset, len, on_data, on_done)` | Decompose block into pieces (16KB each). For each piece, find an ACTIVE peer channel with the block in its bitfield and `reqs_pending < 4`. Send `block_req` via DataChannel. Returns 0 if at least one piece dispatched, -1 if no peer has capacity. When all pieces arrive → SHA1 verify → `on_done(offset, len, ok)`. SHA1 mismatch → retry block (max 3). |
| `close(self)` | Send `bye_req` to all peer channels. On `bye_rsp` or timeout → close DataChannel. Unregister from global P2P module. |

DataChannel lifecycle events (`on_dc_ready`, `on_bitfield`, `on_have`, `on_block`, `on_disconnect`) are callbacks from the global P2P module — described in [Source vtable](#source-vtable).

### HTTP source: `xdl_source_http_create()`

```c
xdl_source_vtable_t *xdl_source_http_create(const char **urls, int url_count, long timeout_ms);
```

| Method | What it does |
|--------|-------------|
| `open(self)` | Send HEAD request to probe Content-Length. Determine block count. If Content-Length missing, use single-GET fallback mode. No block dispatch yet. |
| `fetch(self, offset, len, on_data, on_done)` | Issue `Range: bytes=offset-(offset+len-1)` to the CDN URL. Returns 0 on success (request sent), -1 if `in_flight >= max_in_flight`. Calls `on_data` per chunk for streaming SHA1 + cache write. Calls `on_done(offset, len, ok)` on HTTP completion or error. |
| `close(self)` | Cancel all in-flight HTTP requests via curl multi handle. |

### Source ↔ Scheduler interaction

```
xdl_task_start:
  ┌─ http_source->open(self)
  │    → HEAD /file.bin → Content-Length: 100MB
  │    → block_count = 100MB / 256KB = 400
  │    → allocate block bitmap
  │
  ├─ p2p_source->open(self)
  │    → register(source) in global P2P module
  │    → no network I/O (announce happens on next global tick)
  │
  ├─ resume_load → restore bitmap → populate pending queue
  │
  └─ sched->on_start(self)
       → xdl_schedule_default_t registers a tick_ms timer on event loop

══════════ Main loop ══════════

tick timer fires → sched->on_tick(self):

  for each pending block:
    ┌─ seek window 内 ──────────────────────────┐
    │  http_source->fetch(offset, len, cb)       │
    │    → curl Range GET → HTTP/2 stream        │
    │    → on_data(chunk) per TCP segment        │
    │    → on_done(ok) when complete/timeout     │
    │    → returns 0 (dispatched) or -1 (full)   │
    └────────────────────────────────────────────┘

    ┌─ seek window 外 ──────────────────────────┐
    │  p2p_source->fetch(offset, len, cb)        │
    │    → decompose block into pieces           │
    │    → find peers with block in bitfield     │
    │    → send block_req via DataChannel        │
    │    → returns 0 (dispatched) or -1 (full)   │
    └────────────────────────────────────────────┘

when block complete (http or p2p):
  → on_done(offset, len, ok)
    → sched->on_block_done(self, offset, len, ok)
      ok:  block_mark_complete, resume_save, xRelayEmit
      err: requeue (retry ≤ 3)

══════════ Stop ══════════

xdl_task_stop:
  ┌─ http_source->close(self)   → cancel curl requests
  ├─ p2p_source->close(self)    → send bye_req, unregister
  └─ sched->on_stop(self)       → cancel tick timer
```

#### Internal state

```c
struct p2p_source {
    xdl_p2p_conf_t   conf;
    xdl_task_t      *task;
};
```

All peer state lives in `xdl_peer_channel` in the global P2P module. The source accesses it via `p2p_module->peers[peer_id]->channels[info_hash]`.

#### DataChannel message protocol (8-byte fixed header)

All messages share an 8-byte header, transport-agnostic (WebRTC DataChannel, raw UDP, etc.):

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  ver  |  cmd  |              seq              |    reserved   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            length              |                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
|                    payload (length bytes)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Field | Size | Description |
|-------|------|-------------|
| version | 4 bit | Protocol version, current = 0 |
| cmd | 4 bit | Message type |
| seq | 2B LE | Monotonic sequence number |
| reserved | 1B | Reserved, MUST be 0 |
| length | 3B LE | Payload length, max 16 MiB (0 = no payload) |
| payload | length bytes | Command-specific body |

Messages (payload only, header omitted for brevity):

```
hello_req    : [20B info_hash][N bytes peer_id]                 // handshake + heartbeat
hello_rsp    : [20B info_hash][N bytes peer_id]                 // ack
bitfield_req : [4B block_count LE][N bytes bitmap]              // my bitmap
bitfield_rsp : [4B block_count LE][N bytes bitmap]              // your bitmap
block_req    : [4B block_index LE][2B start_piece LE][2B count LE]
block_rsp    : [4B block_index LE][2B start_piece LE][N bytes data]
have_req     : [4B block_index LE]                              // notification, no response
bye_req      : (empty)                                          // half-close
bye_rsp      : (empty)                                          // ack, then close
```

Bitmaps use `xBitmap` from `xbase` — raw bytes from `xBitmapData()` sent directly as payload.

#### PeerConnection and DataChannel lifecycle

PeerConnections are managed by the global P2P module, not per-source. One peer pair shares one PeerConnection with multiple DataChannels (one per torrent).

```
Global P2P module state:
  peers:   xMap[peer_id → xPeerConnection]     // at most one per remote peer
  sources: xMap[info_hash → p2p_source]         // info_hash → owning source

PeerConnection establishment (called from global tick):

  p2p_ensure_channel(remote_peer_id, info_hash):
    peer = p2p_module->peers[remote_peer_id]
    if peer exists:
      if peer->channels[info_hash] exists → return channel
      else:
        → dc = xDataChannelCreate(peer->pc, label = info_hash)  // no new ICE
        → peer->channels[info_hash] = {dc, bitfield={}, reqs=0, state=HELLO}
    else:
      → pc = xPeerConnectionCreate()
      → dc = xDataChannelCreate(pc, label = info_hash, reliable + ordered)
      → offer = xPeerConnectionCreateOffer(pc)
      → xPeerConnectionSetLocalDescription(pc, offer)
      → POST /relay {peer_id: remote, signal: {type:"offer", from:peer_id, sdp:offer}}
      → peers[remote_peer_id] = {pc, channels={info_hash → {dc, state=HELLO}}}

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
    → send handshake[info_hash][peer_id]
    → receive bitfield → peer state → ACTIVE
    → ready for block requests
```

DataChannel labels are the `info_hash` hex string (40 chars). This is the key that routes incoming DataChannels to their owning P2P source.

PeerConnection reuse is the key optimization: if Alice and Bob already have a connection from torrent "def", adding torrent "abc" only creates a new DataChannel on the existing connection — no ICE, no DTLS, no signaling round-trips.

#### Peer state machine

```
IDLE  --DC established--> hello --recv hello_rsp--> bitfield --recv bitfield_rsp--> ACTIVE
                                                                              |
DEAD  <-- disconnect/timeout/error -- (any state)                             |
                                                                              |
                                                                      recv Block -> on_data/on_done
                                                                      recv have_req → update channel->bitfield
```

#### Data transfer (per source)

Announce, peer discovery, and signaling are handled by the global P2P module's tick. The per-task P2P source handles data transfer (see source vtable methods above): block decomposition into pieces, peer selection, DataChannel request/response, SHA1 verification, and HAVE propagation.

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

    struct xdl_schedule_vtable *sched;   // per-task scheduler (default or custom)
    xdl_cache_t   *cache;               // ring buffer (ring_bytes=0 → passthrough)
};
```

### Scheduler (per-task)

Each task has its own scheduler instance implementing `xdl_schedule_vtable_t`. A default implementation (`xdl_schedule_default_t`) is provided and used when `conf.sched` is NULL.

```c
struct xdl_schedule_vtable {
    void (*on_start)     (void *self);
    void (*on_tick)      (void *self);
    void (*on_block_done)(void *self, uint64_t offset, uint32_t len, bool ok);
    void (*on_stop)      (void *self);
    long   tick_ms;                    // suggested tick interval
};
```

#### Default implementation: `xdl_schedule_default_t`

```c
struct xdl_schedule_default {
    xdl_schedule_vtable_t  vt;        // vtable (on_start/on_tick/on_block_done/on_stop)
    xdl_source_vtable_t   *http;      // optional
    xdl_source_vtable_t   *p2p;       // optional
    int   concurrency;                // total in-flight cap
    int   http_cap;                   // default concurrency / 2
    int   p2p_cap;                    // default concurrency / 2
    int   http_in_flight;
    int   p2p_in_flight;
    xdl_pending_block_t   *pending_head, *pending_tail;
    uint64_t  seek_point;             // per-task, 0 = sequential
};

XCAPI_LOCAL(xdl_schedule_vtable_t *)
xdl_schedule_default_create(xdl_source_vtable_t *http,
                            xdl_source_vtable_t *p2p,
                            int concurrency);
```

**on_tick(self)** (called every `tick_ms`):

```
xdl_schedule_default_on_tick(self):
    sched = (xdl_schedule_default_t *)self
    gather task's pending blocks, sort by seek window

    for each pending block:
        in_window = |block.offset - sched->seek_point| within window

        // HTTP: seek window 内的 block 优先
        if in_window && sched->http:
            if sched->http_in_flight < sched->http_cap:
                rc = sched->http->fetch(sched->http, offset, len, on_data, on_block_done)
                if rc == 0: sched->http_in_flight++, continue

        // P2P: window 外优先，window 内作为 fallback
        if sched->p2p:
            if sched->p2p_in_flight < sched->p2p_cap:
                rc = sched->p2p->fetch(sched->p2p, offset, len, on_data, on_block_done)
                if rc == 0: sched->p2p_in_flight++, continue

        // both full → break, wait for next tick
```

**on_block_done(self, offset, len, ok)**:

```
xdl_schedule_default_on_block_done(self, offset, len, ok):
    decrement source's in_flight counter
    if ok: block_mark_complete, resume_save, xRelayEmit(progress)
    else:  requeue (max 3 retries)
```

When `http` or `p2p` is NULL, the corresponding branch is skipped. A task with only HTTP source simply never enters the P2P branch, and vice versa.

Custom schedulers implement `xdl_schedule_vtable_t` directly. The caller passes a custom instance via `xdl_task_conf_t.sched`; if NULL, `xdl_task_start` creates a `xdl_schedule_default_t`.

### Source vtable

Each P2P source implements the standard source vtable. Additionally, the global P2P module calls into it for DataChannel lifecycle events.

```c
struct xdl_source_vtable {
    const char *name;

    void (*open)(void *self);
    int  (*fetch)(void *self, uint64_t offset, uint32_t len,
                  void (*on_data)(const uint8_t *data, uint32_t len, void *arg),
                  void (*on_done)(uint64_t offset, uint32_t len, bool ok, void *arg));
    void (*close)(void *self);
};
```

`open` / `fetch` / `close` receive `self` as the first argument — the instance pointer passed through at creation time. `on_data` / `on_done` callbacks receive `arg` for the caller's context.

#### source_p2p methods

All methods receive `void *self` — cast back to `xdl_source_p2p_t *` internally.

**open(self)**: Register this source in the global P2P module keyed by `info_hash`. No network I/O.

**fetch(self, offset, len, on_data, on_done) → int**: Decompose the block at `offset` into pieces (`piece_size = 16KB`). Find ACTIVE peer channels with `reqs_pending < 4` and the block in their bitfield. Send `block_req` via DataChannel. Returns 0 if at least one piece was dispatched, -1 if no peer had capacity. When all pieces complete → SHA1 vs `block_hashes[block_index]` → `on_done(offset, len, ok)`. On SHA1 mismatch → retry (max 3 attempts).

**close(self)**: Unregister from global P2P module. Send `bye_req` to all peer channels. On `bye_rsp` or timeout → close DataChannel.

#### Callbacks from global P2P module (not on vtable)

Global P2P module calls these directly on the source when DataChannel or signaling events occur:

**on_dc_ready(dc, peer_id)**: A DataChannel labeled with this source's `info_hash` is ready. Send `hello_req{info_hash, peer_id}`. Wait for `hello_rsp` → bitfield exchange → peer state to ACTIVE.

**on_bitfield(peer, bitfield)**: Store peer's block bitmap. Transition state bitfield → ACTIVE. Pending piece requests to this peer can now be dispatched.

**on_have(peer, block_index)**: Set `peer.bitfield[block_index] = 1` via `xBitmapSet`. If there are pending pieces waiting for this block, try to dispatch them to this peer.

**on_block(peer, block_index, offset, data, len)**: Piece data received. Call `on_data(data, len)` for streaming SHA1 + cache write. Track completion. When all pieces for a block complete → SHA1 verify → `on_done(ok)`. Send `have_req{block_index}` to all other ACTIVE peers.

**on_disconnect(peer)**: Peer disconnected or timed out. Mark as DEAD. Cancel pending requests to this peer, invoke `on_done(offset, len, ok=false)` for in-flight blocks so the scheduler can re-dispatch.

### Cache (ring buffer)

Read-through block cache.  Wraps a storage file (`xdl_storage_file_t *ctx`)
to accelerate peer reads.  Writes pass through to storage and are NOT cached
(caching cold blocks is wasteful — only peer reads populate the ring).

Cache is bound to a single `(storage_file, ctx)` pair: `xdl_cache_create`
takes the already-opened `ctx`, calls into it via the convenience dispatchers
(`xdl_storage_write`/`xdl_storage_read`/...), and the caller never touches
the storage file directly while the cache owns it.

Cache size controls behavior:

- `ring_bytes = 0` → passthrough, reads/writes go directly to storage (no caching)
- `ring_bytes > 0` → ring buffer of `ring_bytes / block_size` slots, FIFO eviction

```c
typedef void (*xdl_cache_read_cb_t)(void *arg, const uint8_t *data, size_t len);

struct xdl_cache_t {
    xdl_storage_file_t *file;      // bound storage file (owned by cache)
    uint8_t  *buffer;              // single contiguous allocation (max_blocks * block_size)
    uint32_t *block_indexes;       // max_blocks entries, UINT32_MAX = empty
    uint32_t  max_blocks;          // 0 = passthrough
    uint32_t  block_size;
    uint32_t  head;                // next write position
};

XCAPI_LOCAL(xdl_cache_t *)
xdl_cache_create(xdl_storage_file_t *file, uint32_t block_size, size_t ring_bytes);

XCAPI_LOCAL(int)
xdl_cache_read(xdl_cache_t *c, uint64_t offset, size_t len,
               xdl_cache_read_cb_t done, void *arg);

XCAPI_LOCAL(int)
xdl_cache_write(xdl_cache_t *c, uint64_t offset,
                const uint8_t *data, size_t len,
                xdl_storage_write_cb_t done, void *arg);

/* flush/close — delegate to the underlying storage file */
XCAPI_LOCAL(int)  xdl_cache_flush(xdl_cache_t *c, xdl_storage_flush_cb_t cb, void *arg);
XCAPI_LOCAL(void) xdl_cache_close(xdl_cache_t *c, xdl_storage_close_cb_t cb, void *arg);

XCAPI_LOCAL(void) xdl_cache_destroy(xdl_cache_t *c);
```

**Read flow**:
1. If `max_blocks == 0`: passthrough to `xdl_storage_read(file, ...)`
2. `target = offset / block_size`, scan `block_indexes[0..max_blocks)` for match
3. Hit → invoke `done(arg, buffer + index * block_size + offset % block_size, len)` synchronously
4. Miss → `xdl_storage_read(file, ...)` → memcpy into `buffer[head % max_blocks]` → invoke `done`

**Write flow** (write-through):
1. `xdl_storage_write(file, ...)` — persist to disk immediately
2. Do NOT cache the written block — only peer reads populate the ring

**Eviction**: FIFO via `head` pointer. When head wraps, the oldest slot is overwritten with no free/malloc — purely memcpy.

Default `ring_bytes` = 64MB (~261 blocks at 256KB). Set to 0 for HTTP-only downloads where caching provides no benefit (no P2P peer reads).

### Storage file

A storage file is a handle whose first member is a pointer to a vtable.
Callers use convenience inline dispatchers (`xdl_storage_write`/`xdl_storage_read`/...)
which dispatch through the vtable.  This lets decorators (e.g. cache)
wrap a storage file without exposing their internal layout.

```c
struct xdl_storage_file_t {
  xdl_storage_vtable_t *vt;   /* must be first member */
};

struct xdl_storage_vtable {
  int  (*write)(xdl_storage_file_t *f, uint64_t offset,
                const uint8_t *data, size_t len,
                xdl_storage_write_cb_t cb, void *arg);
  int  (*read) (xdl_storage_file_t *f, uint64_t offset,
                uint8_t *buf, size_t len,
                xdl_storage_read_cb_t cb, void *arg);
  int  (*flush)(xdl_storage_file_t *f, xdl_storage_flush_cb_t cb, void *arg);
  void (*close)(xdl_storage_file_t *f, xdl_storage_close_cb_t cb, void *arg);
};

/* Convenience dispatchers (inline) */
int  xdl_storage_write(xdl_storage_file_t *f, ...);
int  xdl_storage_read (xdl_storage_file_t *f, ...);
int  xdl_storage_flush(xdl_storage_file_t *f, xdl_storage_flush_cb_t cb, void *arg);
void xdl_storage_close(xdl_storage_file_t *f, xdl_storage_close_cb_t cb, void *arg);
```

Callbacks:

```c
typedef void (*xdl_storage_open_cb_t) (void *arg, xErrno err, xdl_storage_file_t *f);
typedef void (*xdl_storage_read_cb_t)(void *arg, const uint8_t *data, ssize_t len);
typedef void (*xdl_storage_write_cb_t)(void *arg, xErrno err, ssize_t written);
typedef void (*xdl_storage_flush_cb_t)(void *arg, xErrno err);
typedef void (*xdl_storage_close_cb_t)(void *arg);
```

`open` is a factory function, NOT a vtable method (since there is no file yet
to dispatch through).  The default filesystem backend lives in `storage_fs.h`:

```c
int xdl_storage_fs_open(const char *dest, xdl_storage_open_cb_t cb, void *arg);
```

Lifecycle:

```
xdl_storage_fs_open(dest, cb, arg)         // submit
    → cb(arg, err, f)                       // async completion

xdl_storage_write(f, off, data, len, cb, arg)
xdl_storage_read (f, off, buf,  len, cb, arg)

xdl_storage_flush(f, cb, arg)               // rename .part → dest
    → cb(arg, err)

xdl_storage_close(f, cb, arg)               // free f (deletes .part if not flushed)
    → cb(arg)
```

All methods are async (callbacks fire on completion).  `open`/`write`/`read`/`flush`
return 0 on submit success, -1 on sync error.  `close` always succeeds (best-effort
cleanup) and fires `cb(arg)` synchronously or asynchronously.
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
| Malicious peer sends fake bitfield to drain request slots | Future: rate-limit how many consecutive timeouts a peer can cause before marking DEAD. MVP accepts the waste; SHA1 catches bogus data. |
| Tracker has no auth or rate limiting | MVP: trusted LAN/private network. Production: add shared-secret auth header to `PUT /announce`. |
| DataChannel messages from unverified peers | Handshake info_hash validation is the only gate — sufficient for closed groups, insufficient for open deployment. |
| Bencoding parser is a new internal module | Minimal implementation (~200 lines). Well-defined format with existing reference implementations. |
| No Content-Length fallback for large files | Chunked-transfer files above ~50 MB will use single GET, blocking the event loop and losing resume capability. Consider `xWorkSubmit` or streaming-write-to-disk for files exceeding a size threshold. |

## Open Questions

None remaining.
