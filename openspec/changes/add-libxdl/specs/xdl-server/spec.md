## ADDED Requirements

The Tracker is a single HTTP/3 (QUIC) server handling peer discovery, keep-alive, and signaling relay. It is stateful — it maintains an incremental peer × file registry and per-peer message inboxes.

---

### Requirement: Tracker — API reference

Resource model: `/announce` — peer registration and heartbeat. `/torrent/:info_hash` — torrent metadata stored and retrieved by `info_hash`. Files are identified by `info_hash` (40-char hex encoding of SHA1(bencode(info))).

| Method | Path | Description |
|--------|------|-------------|
| `PUT` | `/announce` | Peer announce + heartbeat. Reports file changes, receives inbox signals and TTL. |
| `GET` | `/torrent/:info_hash/peer` | List active peers for a file |
| `POST` | `/relay` | Send a signaling message to another peer |
| `POST` | `/torrent` | Publish a `.torrent` file (bencoded body) |
| `GET` | `/torrent/:info_hash` | Retrieve a `.torrent` file by info_hash |
| `GET` | `/health` | Health check |
| `GET` | `/stats` | Global stats (optional) |

All responses use `Content-Type: application/json` except `POST /torrent` (accepts bencoded binary) and `GET /torrent/:info_hash` (returns bencoded binary).

**Common error format:**
```json
{"error": "bad_request", "message": "missing required field: peer_id"}
```

| HTTP Status | `error` value | Meaning |
|-------------|--------------|---------|
| 400 | `bad_request` | Missing/invalid field |
| 404 | `not_found` | Peer or file not found |
| 429 | `rate_limited` | Too many requests |
| 500 | `internal` | Server error |

#### PUT /announce

Peer announce + heartbeat. Reports only the files that changed since the last PUT. `peer_id` is in the request body.

Request:
```json
{
    "peer_id": "alice",
    "changes": [
        {"info_hash": "abc123", "pct": 45.7},
        {"info_hash": "def456", "pct": 60.3},
        {"info_hash": "old987",  "pct": 0.0}
    ]
}
```

`changes` is optional. An empty body `{"peer_id":"alice"}` is a pure heartbeat. Each entry upserts: creates the peer+file entry if it doesn't exist, updates `pct` if it does. `pct: 0.0` effectively removes the peer from that file's peer list.

Response `200`:
```json
{
    "status": "ok",
    "ttl_ms": 5000,
    "signals": [
        {"type": "offer", "from": "carol", "sdp": "v=0\r\n..."},
        {"type": "candidate", "from": "carol", "candidate": "candidate:...", "sdpMid": "0", "sdpMLineIndex": 0}
    ]
}
```

Behavior:
- Idempotent: `changes` for an already-tracked file updates `pct` and refreshes `last_seen`.
- Server responds with pending signaling messages for this peer (offer/answer/candidate).
- Server returns `ttl_ms` — the peer MUST send the next PUT within this interval. Server marks files as stale if no PUT is received for that peer within TTL.
- Server MAY adjust `ttl_ms` dynamically based on load.
- Peers with `pct == 100.0` are full seeders; `pct < 100.0` are leechers.

#### Scenario: Incremental changes

- **WHEN** a peer sends `PUT /announce {"peer_id":"alice","changes":[{"info_hash":"abc123","pct":45.7}]}`
- **THEN** the server registers alice as having file abc123 at 45.7%
- **THEN** subsequent `GET /torrent/abc123/peer` includes alice

#### Scenario: Remove file via pct 0

- **WHEN** a peer sends `PUT /announce {"peer_id":"alice","changes":[{"info_hash":"abc123","pct":0.0}]}`
- **THEN** the server removes alice from file abc123's peer list
- **THEN** subsequent `GET /torrent/abc123/peer` no longer includes alice

#### Scenario: Pure heartbeat

- **WHEN** a peer sends `PUT /announce {"peer_id":"alice"}` (no changes)
- **THEN** the server refreshes `last_seen` for all of alice's registered files
- **THEN** the response includes any pending inbox signals

#### Scenario: Inbox delivered with announce

- **WHEN** a peer sends `PUT /announce {"peer_id":"...", "changes":[...]}`
- **THEN** the `signals` field in the response contains all pending signaling messages for that peer
- **THEN** an empty `signals` array means no pending signals

#### GET /torrent/:info_hash/peer

List active peers for a file. Returns `peer_id` and `relay_addr` for each peer — the `relay_addr` tells the discovering peer where to send signaling messages via `POST /relay`.

Response `200`:
```json
{
    "info_hash": "abc123",
    "peers": [
        {"peer_id": "bob",   "relay_addr": "tracker1.example.com:8080"},
        {"peer_id": "carol", "relay_addr": "tracker2.example.com:8080"}
    ]
}
```

If `info_hash` has no registered peers: response `200` with empty `peers` array.

#### POST /relay

Send a signaling message to another peer. On-demand — called only when a signal needs to be sent (offer, answer, candidate).

Request:
```json
{
    "peer_id": "bob",
    "signal": {
        "type": "answer",
        "from": "alice",
        "to": "bob",
        "sdp": "v=0\r\n..."
    }
}
```

Response `200`:
```json
{
    "status": "ok",
    "signals": []
}
```

The `signals` field in the response MAY carry any pending signals for the sender — same as the PUT response.

#### POST /torrent

Publish a `.torrent` file to the Tracker. The seeder posts the raw bencoded torrent. The Tracker computes `info_hash` = SHA1(bencode(info)) and indexes the torrent by it.

Request:
- `Content-Type: application/octet-stream`
- Body: raw bencoded `.torrent` file

Response `200`:
```json
{"status": "ok", "info_hash": "a1b2c3d4e5f6..."}
```

Behavior:
- Computes `info_hash` from the posted torrent body.
- Stores the torrent keyed by `info_hash`.
- Returns `info_hash` in the response for the seeder to construct magnet URIs.
- Idempotent: posting the same torrent twice succeeds with the same `info_hash`.

**Errors:**
- `400` if body is not valid bencoding or missing required info fields.
- `413` if torrent exceeds max size (default 1 MB).

#### Scenario: Publish a new torrent

- **WHEN** a seeder posts a valid bencoded torrent via `POST /torrent`
- **THEN** the Tracker responds with `200` and the computed `info_hash`
- **THEN** subsequent `GET /torrent/<info_hash>` returns the torrent

#### Scenario: Republish same torrent

- **WHEN** posting a torrent with the same `info` dict as a previously published one
- **THEN** the response is `200` with the same `info_hash`

#### Scenario: Invalid torrent body

- **WHEN** the body is not valid bencoding
- **THEN** the server responds with `400 {"error":"bad_request","message":"invalid bencoding"}`

#### GET /torrent/:info_hash

Retrieve a `.torrent` file by `info_hash` (40-char lowercase hex).

Response `200`:
- `Content-Type: application/octet-stream`
- Body: raw bencoded `.torrent` file

**Errors:**
- `404` if no torrent exists for the given `info_hash`.

#### Scenario: Retrieve existing torrent

- **WHEN** `GET /torrent/a1b2c3d4e5f6...` is called for a published torrent
- **THEN** the response is `200` with the raw bencoded torrent body

#### Scenario: Torrent not found

- **WHEN** `GET /torrent/a1b2c3d4e5f6...` is called for an unpublished torrent
- **THEN** the response is `404 {"error":"not_found","message":"torrent not found"}`

- **WHEN** `info_hash` hex string is not exactly 40 characters
- **THEN** the response is `400 {"error":"bad_request","message":"invalid info_hash"}`

---

### Requirement: Tracker — data structure

The internal data structure SHALL provide these capabilities. The concrete implementation (HashMap, cross-linked list, hybrid index) is TBD — deferred to implementation phase after profiling with realistic peer/file densities.

| Capability | Complexity target | Description |
|-----------|------------------|-------------|
| Row scan | O(F) | Iterate all peers for a given `info_hash`. F = number of peers with that file. Used by `GET /torrent/:info_hash/peer`. |
| Column scan | O(P) | Iterate all files for a given `peer_id`. P = number of files registered by that peer. Used by TTL expiry cleanup. |
| Row insert/update | O(1) | Add or update a peer entry for a file. Used by `PUT /announce` upsert. |
| Row delete | O(1) | Remove a peer from a file's peer list. Used by `PUT /announce` with pct=0. |
| Column delete | O(P) | Remove a peer and all its registered files. Used by TTL expiry cleanup. |
| Row size | O(1) | Count peers for a file. Used by `/stats`. |
| Column size | O(1) | Count files for a peer. Used by `/stats`. |
| Matrix size | O(1) | Total peer×file entries. Used by `/health`. |

The structure MUST support:
- Peer → file lookup (column)
- File → peer lookup (row)
- Sparse matrix (99%+ empty cells in typical deployment)
- In-memory operation (no external database dependency for v1)

#### Scenario: Peer registers many files

- **WHEN** a peer registers 10,000+ files via incremental PUTs
- **THEN** column scan iterates all files in O(P) time
- **THEN** row insert for each file completes in O(1) amortized

#### Scenario: Peer TTL expiry removes all files

- **WHEN** a peer with 10,000+ files exceeds its TTL without sending `PUT /announce`
- **THEN** the cleanup sweep removes the peer from all its registered files in O(P) time

---

### Requirement: Tracker — edge cases

#### Scenario: Stale peer pruned

- **WHEN** a peer has not sent `PUT /announce` within its assigned TTL
- **THEN** the cleanup timer removes the peer from all its registered files and drops its inbox

#### Scenario: Message delivered on next PUT

- **WHEN** a signaling message arrives for peer alice via `POST /relay`
- **THEN** the server enqueues it in alice's inbox
- **WHEN** alice next sends `PUT /announce {"peer_id":"alice"}`
- **THEN** the server returns the signal in the `signals` field of the response

#### Scenario: Message TTL expiry

- **WHEN** a signaling message sits in inbox for more than 30 seconds without delivery
- **THEN** the cleanup sweep removes it without delivery

#### Scenario: Rate limit exceeded

- **WHEN** a single IP sends more than 60 requests in a 1-second window
- **THEN** all subsequent requests within that window return `429`

#### Scenario: Invalid pct

- **WHEN** `pct` is negative or greater than 100.0
- **THEN** the server clamps to [0.0, 100.0] and processes normally

---

### Requirement: Tracker configuration

```c
struct xdl_tracker_conf {
    uint16_t  port;               // default 8080
    int       default_ttl_ms;     // default 5000
    int       message_ttl_ms;     // default 30000
    int       cleanup_interval_ms; // default 1000
    int       max_inbox_per_peer;  // default 256
};
```

#### Scenario: Startup

- **WHEN** `xdl_tracker_start(&conf)` is called
- **THEN** the Tracker listens on `conf.port` using HTTP/3 (QUIC)
- **THEN** the cleanup timer starts immediately

---

### Requirement: Multi-instance deployment

When multiple Tracker instances serve the same swarm, peer discovery MUST return active peers regardless of which instance they registered with. Two strategies are planned:

#### Strategy A: Shared storage backend

```
p1 ──PUT──→ t1 ──write──→ [Shared DB] ←──read── t2 ←──GET── p2
p1 ──POST/relay──→ t1 ──write──→ [Shared DB] ←──read── t2 ←──PUT/heartbeat── p2
```

All Tracker instances are stateless gateways. All state lives in a shared storage backend (Redis, etcd, or a dedicated distributed KV):

| State | Written by | Read by |
|-------|-----------|---------|
| peer × file matrix (`peer_id`, `info_hash`, `pct`, `last_seen`) | `PUT /announce` | `GET /torrent/:info_hash/peer` |
| peer → tracker mapping (`relay_addr`) | `PUT /announce` | `GET /torrent/:info_hash/peer` response |
| signal inboxes | `POST /relay` | `PUT /announce` response |

The shared store is the single source of truth. Tracker instances handle HTTP routing but carry zero persistent state. Any instance can serve any request — peer discovery, signaling, heartbeat — scaling is purely horizontal by adding instances behind a load balancer.

**Benefits:** Tracker instances are stateless — scalable, replaceable, trivial to deploy. Peer state and signal inboxes survive instance restarts. No cross-tracker coordination protocol needed.

**Trade-offs:** Introduces an external dependency. PUT latency includes one round-trip to the shared store. All instances must be able to reach the store.

#### Strategy B: Tracker federation (gossip)

```
t1 ←──gossip──→ t2 ←──gossip──→ t3
```

Tracker instances form a peer-to-peer overlay and periodically exchange peer state deltas via a gossip protocol. Each instance maintains a local replica of the full peer×file matrix, synchronized through gossip. Peer discovery (`GET /torrent/:info_hash/peer`) is served from the local replica with zero network round-trips.

**Benefits:** No external storage dependency. Read latency is local (no network hop). Naturally decentralized — aligns with P2P philosophy.

**Trade-offs:** Eventually consistent — a newly registered peer may not be visible to all instances until the next gossip round. Requires gossip protocol implementation (memberlist / SWIM / custom). Higher operational complexity than shared storage.

**Inbox handling:** Same constraint as shared storage — signal inboxes are local to the instance that received the signal. The target peer must heartbeat to that same instance to retrieve pending signals.

#### Scenario: Peer discovers peers across instances

- **WHEN** p1 queries `GET /torrent/abc123/peer` from t1
- **THEN** the response includes p2 with `relay_addr` pointing to p2's registering instance (t2)
- **THEN** p1 sends `POST /relay` to t1 — t1 routes the signal to p2's inbox on t1 (NOT to t2)
- **THEN** p2 retrieves the signal by heartbeating to t1 — if p2 has moved to t2, the signal expires and p1 re-discovers p2's new `relay_addr`

#### Scenario: Peer registers on one instance, discoverable from another

- **WHEN** p2 registers via `PUT /announce {"peer_id":"p2",...}` on t2
- **THEN** within the consistency window (synchronous for shared storage, gossip-round for federation), p2 is visible to `GET /torrent/abc123/peer` queries on t1
