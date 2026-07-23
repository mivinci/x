## ADDED Requirements

xdl-server consists of two independent services: **Seed Server** (HTTP) and **Signal Server** (WebSocket).

---

### Requirement: Seed Server — API reference

Resource model: `/file/:fid/peer/:peer_id` — files own peers as a sub-resource.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/health` | Health check |
| `PUT` | `/file/:fid/peer/:peer_id` | Peer announce (upsert) |
| `DELETE` | `/file/:fid/peer/:peer_id` | Peer graceful unregister |
| `GET` | `/file/:fid/peer` | List peers for a file |
| `GET` | `/stats` | Global stats (optional) |

All responses use `Content-Type: application/json`. All timestamps are Unix milliseconds.

#### Common error format

```json
{"error": "bad_request", "message": "missing required field: host"}
```

| HTTP Status | `error` value | Meaning |
|-------------|--------------|---------|
| 400 | `bad_request` | Missing/invalid field |
| 404 | `not_found` | Peer or file not found |
| 429 | `rate_limited` | Too many requests |
| 500 | `internal` | Server error |

#### GET /health

Response `200`:
```json
{"status": "ok", "uptime_ms": 12345678, "peer_count": 42, "file_count": 7}
```

No auth required. Used by load balancers and monitoring.

#### PUT /file/:fid/peer/:peer_id

Peer periodic announce (upsert). Peers MUST call this every 5 seconds to stay in the active list. `fid` and `peer_id` are URL path parameters — the body only carries mutable peer state.

Request:
```json
{
    "host":      "10.0.0.1",    // peer's reachable IP/hostname
    "port":      9000,          // peer's WebRTC/signaling port
    "have_pct":  45.7           // completion percentage (0.0 - 100.0)
}
```

Response `200`:
```json
{
    "peers": [
        {"peer_id": "bob",   "host": "10.0.0.2", "port": 9000, "have_pct": 100.0},
        {"peer_id": "carol", "host": "10.0.0.3", "port": 9000, "have_pct": 12.3}
    ]
}
```

Behavior:
- Idempotent: creates the file entry + peer entry if either doesn't exist; updates `host`, `port`, `have_pct`, `last_seen` if they do.
- Returns all currently active peers for this `fid` (excluding the announcing peer itself).
- Peers with `have_pct == 100.0` are full seeders; peers with `have_pct < 100.0` are leechers.
- A periodic cleanup (every 1000ms) removes entries where `now - last_seen > 5000ms`.

#### DELETE /file/:fid/peer/:peer_id

Graceful departure. Lets a peer announce it's leaving without waiting for the 5-second timeout. No request body.

Response `200`:
```json
{"status": "ok"}
```

Response `404` if the peer was not registered:
```json
{"error": "not_found", "message": "peer alice not registered for file abc123"}
```

#### GET /file/:fid/peer

List active peers for a file. No request body, no query parameters — `fid` is in the URL.

Response `200`:
```json
{
    "fid": "abc123",
    "peers": [
        {"peer_id": "bob",   "host": "10.0.0.2", "port": 9000, "have_pct": 100.0},
        {"peer_id": "carol", "host": "10.0.0.3", "port": 9000, "have_pct": 12.3}
    ]
}
```

If `fid` has no registered peers: response `200` with empty `peers` array.

#### GET /stats

Response `200`:
```json
{
    "peer_count":   42,
    "file_count":   7,
    "seed_count":   15,
    "leech_count":  27
}
```

#### Internal state model

```
seed_server
└── files: HashMap<fid, PeerSet>
    └── PeerSet
        ├── entries: HashMap<peer_id, PeerEntry>
        └── cleanup_timer (1000ms)

PeerEntry {
    peer_id:    char[64]
    fid:        char[64]
    host:       char[256]
    port:       uint16
    have_pct:   float
    last_seen:  uint64 (unix ms)
}
```

#### Concurrency & limits

| Limit | Value | Rationale |
|-------|-------|-----------|
| Max peers per file | 256 | Prevents O(n^2) signaling |
| Max files tracked | 1024 | Bounds memory |
| fid max length | 64 | Reject oversized keys |
| peer_id max length | 64 | Reject oversized keys |
| Cleanup interval | 1000ms | Matches scheduler tick |

---

### Requirement: Seed Server — edge cases

#### Scenario: Duplicate announce

- **WHEN** the same `(fid, peer_id)` announces again within 5 seconds
- **THEN** the server updates `last_seen`, `host`, `port`, `have_pct` without creating a duplicate

#### Scenario: Rate limit exceeded

- **WHEN** a single IP sends more than 60 requests in a 1-second window
- **THEN** all subsequent requests within that window return `429`

#### Scenario: Invalid have_pct

- **WHEN** `have_pct` is negative or greater than 100.0
- **THEN** the server clamps to [0.0, 100.0] and processes normally

#### Scenario: Missing required field

- **WHEN** `PUT /file/abc123/peer/alice` body is missing `host`
- **THEN** server returns `400 {"error": "bad_request", "message": "missing required field: host"}`

#### Scenario: Stale peer pruned

- **WHEN** a peer has not sent `PUT /file/:fid/peer/:peer_id` for more than 5 seconds
- **THEN** the cleanup timer removes the peer from all its registered `fid` entries at the next sweep (within 1000ms)

---

### Requirement: Signal Server — API reference

| Transport | Path | Description |
|-----------|------|-------------|
| WebSocket | `/ws` | Signaling connection |

The Signal Server is a transparent relay — it does NOT interpret SDP, ICE candidates, or any message payload beyond routing fields (`type`, `from`, `to`).

#### Connection lifecycle

```
Client                          Server
  |                               |
  |--- WS connect /ws ----------->|
  |                               |--- start 5s auth timer
  |<-- connection accepted -------|
  |                               |
  |--- {"type":"hello",           |
  |     "peer_id":"alice"} ------>|
  |                               |--- cancel auth timer, register mapping
  |<-- {"type":"hello_ack"} ------|
  |                               |
  |    ... signaling messages ... |
  |                               |
  |--- WS close ----------------->|
  |                               |--- remove from peer map
```

#### Message types

##### Client → Server

| `type` | Fields | Description |
|--------|--------|-------------|
| `hello` | `peer_id` | Identify self. Must be first message within 5s. |
| `offer` | `from`, `to`, `sdp` | WebRTC offer (relayed to `to`) |
| `answer` | `from`, `to`, `sdp` | WebRTC answer (relayed to `to`) |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | ICE candidate (relayed to `to`) |
| `ping` | (none) | Keepalive |

##### Server → Client

| `type` | Fields | Description |
|--------|--------|-------------|
| `hello_ack` | (none) | Auth accepted |
| `offer` | `from`, `to`, `sdp` | Relayed offer from another peer |
| `answer` | `from`, `to`, `sdp` | Relayed answer from another peer |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | Relayed ICE candidate |
| `error` | `message` | Rejected message or auth failure |
| `pong` | (none) | Keepalive response |

All messages are JSON text frames. The server forwards `offer`, `answer`, `candidate` without modification. The `from` and `to` fields are trusted as-is from the sender — the server asserts `from` matches the authenticated `peer_id` of the sending connection, but does not validate `to`.

#### Internal state model

```
signal_server
└── connections: HashMap<peer_id, WsConn>
    └── WsConn
        ├── peer_id
        ├── socket (WebSocket)
        ├── connected_at: uint64
        └── last_ping_ms: uint64

pending_auth: set of WebSocket connections not yet hello'd
```

#### Concurrency & limits

| Limit | Value | Rationale |
|-------|-------|-----------|
| Max WebSocket connections | 512 | Typical per-process limit |
| Auth timeout | 5000ms | Connection closed if no `hello` received |
| Ping interval | 30000ms | Server sends `ping`, expects `pong` within 10s |
| Max message size | 65536 (64KB) | Enough for SDP (~4-8KB typical, but allow headroom) |
| Per-IP connection limit | 8 | Prevent single-IP flooding |

---

### Requirement: Signal Server — edge cases

#### Scenario: Auth timeout

- **WHEN** a WebSocket connection does not send `hello` within 5 seconds
- **THEN** the server sends `{"type":"error","message":"auth timeout"}` and closes the connection

#### Scenario: Duplicate peer_id

- **WHEN** a new WebSocket sends `hello` with a `peer_id` that already has an active connection
- **THEN** the old connection is closed with `{"type":"error","message":"replaced by new connection"}`
- **THEN** the new connection is registered

#### Scenario: Message to offline peer

- **WHEN** a signal message is addressed to a `peer_id` with no active WebSocket connection
- **THEN** the server sends back `{"type":"error","message":"peer not found: bob"}` to the sender

#### Scenario: Message before auth

- **WHEN** a WebSocket sends any message before `hello`
- **THEN** the server sends `{"type":"error","message":"authenticate first"}` and closes the connection

#### Scenario: Ping timeout

- **WHEN** the server sends `ping` and no `pong` is received within 10 seconds
- **THEN** the server closes the WebSocket connection

#### Scenario: Message without type field

- **WHEN** a JSON message is received without a `type` field
- **THEN** the server sends `{"type":"error","message":"missing type field"}` and ignores the message (does not close)

#### Scenario: Sender identity mismatch

- **WHEN** a message has `from` that differs from the authenticated `peer_id` of the sending connection
- **THEN** the server sends `{"type":"error","message":"sender mismatch"}` and ignores the message

#### Scenario: Oversized message

- **WHEN** a message exceeds the 64KB limit
- **THEN** the server closes the WebSocket connection with code 1009 (message too big)

---

### Requirement: Server configuration

Both servers SHALL accept configuration via a shared config struct:

```c
struct xdl_server_conf {
    uint16_t  seed_port;         // default 8080
    uint16_t  signal_port;       // default 8081
    int       cleanup_interval_ms;  // default 1000
    int       announce_timeout_ms;  // default 5000
    int       auth_timeout_ms;      // default 5000
    int       ping_interval_ms;     // default 30000
    int       ping_timeout_ms;      // default 10000
    int       max_peers_per_file;   // default 256
    int       max_connections;      // default 512
    int       max_message_size;     // default 65536
    int       max_per_ip;           // default 8
};
```

Both servers will run within the xdl event loop. Seed Server piggybacks on `xHttpServer`; Signal Server uses a WebSocket upgrade handler on the same or a separate port.

#### Scenario: Combined startup

- **WHEN** `xdl_server_start(&conf)` is called with both ports configured
- **THEN** the Seed Server listens on `seed_port` and the Signal Server on `signal_port`
- **THEN** both servers share the same event loop

#### Scenario: Seed-only deployment

- **WHEN** `seed_port = 8080, signal_port = 0`
- **THEN** only the Seed Server starts; Signal Server is disabled
