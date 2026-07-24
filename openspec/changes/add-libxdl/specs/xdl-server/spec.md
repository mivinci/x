## ADDED Requirements

xdl-server consists of two independent services: **Seed Server** (HTTP/3) and **Signal Server** (UDP).

Seed Server is stateful — it maintains a peer × file registry. Signal Server is stateless — it relays signaling messages between peers with no connection state.

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
    "signal_addr": "signal1.example.com:8081",  // peer's Signal Server address
    "have_pct":    45.7                         // completion percentage (0.0 - 100.0)
}
```

Response `200`:
```json
{"status": "ok"}
```

Behavior:
- Idempotent: creates the file entry + peer entry if either doesn't exist; updates `signal_addr`, `have_pct`, `last_seen` if they do.
- Does NOT return the peer list — use `GET /file/:fid/peer` separately for discovery.
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

List active peers for a file. Returns only the information needed to signal peers — `peer_id` and `signal_addr`. No IP addresses, no P2P port, no progress info. P2P connectivity is established through the Signal Server, not via direct connection.

Response `200`:
```json
{
    "fid": "abc123",
    "peers": [
        {"peer_id": "bob",   "signal_addr": "signal1:8081"},
        {"peer_id": "carol", "signal_addr": "signal2:8081"}
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
    peer_id:     char[64]
    fid:         char[64]
    signal_addr: char[256]    // "host:port" of peer's Signal Server
    have_pct:    float
    last_seen:   uint64 (unix ms)
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

- **WHEN** `PUT /file/abc123/peer/alice` body is missing `signal_addr`
- **THEN** server returns `400 {"error": "bad_request", "message": "missing required field: signal_addr"}`

#### Scenario: Stale peer pruned

- **WHEN** a peer has not sent `PUT /file/:fid/peer/:peer_id` for more than 5 seconds
- **THEN** the cleanup timer removes the peer from all its registered `fid` entries at the next sweep (within 1000ms)

---

### Requirement: Signal Server — API reference

Signal Server is a **stateless UDP relay**. It does not maintain connection state — every packet is a self-contained "receive and forward" operation. This enables horizontal scaling: any instance can handle any message for any peer.

| Field | Value | Description |
|-------|-------|-------------|
| Transport | UDP | Single socket, single port |
| Default port | 8081 | Configurable |
| Message format | JSON text | Compatible with existing protocol spec |

#### Message flow

```
peer1 ──(UDP)──→ Signal Server ──(UDP)──→ peer2
  {"type":"offer","from":"peer1","to":"peer2","sdp":"..."}
  
peer2 ──(UDP)──→ Signal Server ──(UDP)──→ peer1
  {"type":"answer","from":"peer2","to":"peer1","sdp":"..."}
```

Server role: receive packet → extract `to` field → look up receiver's message queue → return queued messages in the UDP reply. Delivery happens on the receiver's next poll packet, taking advantage of NAT reverse-path mapping. Messages are NOT pushed proactively — the server only responds to packets received from the target peer's source address.

#### Message types (unchanged from protocol spec)

| `type` | Fields | Direction |
|--------|--------|-----------|
| `offer` | `from, to, sdp` | peer → server → peer |
| `answer` | `from, to, sdp` | peer → server → peer |
| `candidate` | `from, to, candidate, sdpMid, sdpMLineIndex` | peer → server → peer |
| `error` | `message` | server → peer (on relay failure) |

No `hello` / `hello_ack` / `ping` / `pong` — no connection to authenticate or keep alive. Server trusts `from` field but MAY validate it against Seed Server's peer registry if configured.

#### How peers send and receive

Due to NAT, the Signal Server cannot push messages to peers proactively. Instead, peers use a **UDP poll loop**:

```
peer → UDP sendto signal:8081   (empty packet or poll message)
signal → udp reply:
  → has queued messages → returns them in the UDP response
  → queue empty → no reply (peer times out after poll_interval)
```

Peer behavior:
- **Poll**: Periodically send a UDP datagram to the peer's registered signal_addr (every 500ms–1000ms, typically on scheduler tick). This keeps the NAT mapping alive and polls for incoming messages.
- **Send**: `sendto(signal_addr, msg)` — fire and forget. No response expected.
- **Receive**: After each poll, check for a UDP reply with relayed messages.

No persistent connection, no handshake, no keep-alive beyond the poll packets themselves.

#### Internal state model (per-instance, ephemeral)

```
signal_server
└── peers: HashMap<peer_id, PeerState>  (populated on first poll)
    └── PeerState
        ├── queue: ring buffer (max 256)
        ├── last_addr: struct sockaddr   (from most recent poll)
        └── last_poll_ms: uint64

Send flow:
  peer1 → signal: {"type":"offer","to":"peer2",...}
    → signal enqueues message in peers[peer2].queue

Poll flow:
  peer2 → signal: <any packet>
    → signal looks up peers[peer2]
    → if queue non-empty: drain → sendmsg(peer2.last_addr, messages)
    → update last_addr, last_poll_ms
```

NAT traversal works because the poll packet creates a mapping on peer2's NAT gateway. The server's UDP reply travels back through that same mapping, reaching peer2.

#### Concurrency & limits

| Limit | Value | Rationale |
|-------|-------|-----------|
| Poll interval | 500–1000ms | Keeps NAT mapping alive, matches scheduler tick |
| Max message size | 65536 (64KB) | SDP typically 2-8KB |
| Max queue per peer | 256 | Prevents memory exhaustion |
| Message TTL | 5000ms | Sender retries if no response |
| Max unique peers tracked | 16384 | Per-instance soft cap |

---

### Requirement: Signal Server — edge cases

#### Scenario: Message enqueued, delivered on next poll

- **WHEN** a signal message arrives for `peer_id` that has not polled recently
- **THEN** the server enqueues the message with TTL 5s
- **WHEN** the target peer subsequently polls
- **THEN** the server returns all queued messages in the UDP reply

#### Scenario: Poll with empty queue

- **WHEN** a peer polls and its queue is empty
- **THEN** the server sends no reply (peer's `recvfrom` times out after `poll_interval`)

#### Scenario: Poll with queued messages

- **WHEN** a peer polls and its queue has pending messages
- **THEN** the server drains the queue into one or more UDP replies to `last_addr`
- **THEN** the server updates `last_addr` and `last_poll_ms` from the poll packet's source address

#### Scenario: Message to peer with full queue

- **WHEN** a signal message arrives for `peer_id` whose queue has 256 messages
- **THEN** the server drops the oldest message and enqueues the new one
- **THEN** no error is sent to the sender (fire-and-forget semantics)

#### Scenario: TTL expiration

- **WHEN** a message sits in the queue for more than 5 seconds
- **THEN** the cleanup sweep (every 1000ms) removes it without delivery

#### Scenario: Oversized message

- **WHEN** a message exceeds the 64KB limit
- **THEN** the server drops it silently (UDP has no error channel)

#### Scenario: Sender identity mismatch

- **WHEN** `from` field does not match the source address registered in Seed Server (if validation is enabled)
- **THEN** the server drops the message and sends back `{"type":"error","message":"sender mismatch"}`

---

### Requirement: Server configuration

Both servers SHALL accept configuration via a shared config struct:

```c
struct xdl_server_conf {
    uint16_t  seed_port;           // default 8080
    uint16_t  signal_port;         // default 8081
    int       cleanup_interval_ms; // default 1000
    int       announce_timeout_ms; // default 5000
    int       message_ttl_ms;      // default 5000
    int       max_peers_per_file;  // default 256
    int       max_queue_per_peer;  // default 256
    int       max_message_size;    // default 65536
    int       max_files;           // default 1024
    int       max_signal_peers;    // default 16384
};
```

Seed Server uses HTTP/3 (QUIC) — stateful, maintains peer × file registry. Signal Server uses UDP — stateless, pure message relay with short-TTL queues.

#### Scenario: Combined startup

- **WHEN** `xdl_server_start(&conf)` is called with both ports configured
- **THEN** the Seed Server listens on `seed_port` and the Signal Server on `signal_port`
- **THEN** both servers share the same event loop

#### Scenario: Seed-only deployment

- **WHEN** `seed_port = 8080, signal_port = 0`
- **THEN** only the Seed Server starts; Signal Server is disabled
