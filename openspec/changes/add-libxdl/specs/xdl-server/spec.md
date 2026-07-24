## ADDED Requirements

The Tracker is a single HTTP/3 (QUIC) server handling peer discovery, keep-alive, and signaling relay. It is stateful — it maintains an incremental peer × file registry and per-peer message inboxes.

---

### Requirement: Tracker — API reference

Resource model: `/peer/:peer_id/seed` — peers own their file list as a sub-resource.

| Method | Path | Description |
|--------|------|-------------|
| `PUT` | `/peer/:peer_id/seed` | Incremental seed announce + heartbeat. Adds/updates or removes files. Response carries inbox messages and TTL. |
| `DELETE` | `/peer/:peer_id/seed` | Peer graceful unregister (removes all files at once) |
| `GET` | `/file/:file_id/peer` | List active peers for a file |
| `POST` | `/relay` | Send a signaling message to another peer |
| `GET` | `/health` | Health check |
| `GET` | `/stats` | Global stats (optional) |

All responses use `Content-Type: application/json`.

**Common error format:**
```json
{"error": "bad_request", "message": "missing required field: add"}
```

| HTTP Status | `error` value | Meaning |
|-------------|--------------|---------|
| 400 | `bad_request` | Missing/invalid field |
| 404 | `not_found` | Peer or file not found |
| 429 | `rate_limited` | Too many requests |
| 500 | `internal` | Server error |

#### PUT /peer/:peer_id/seed

Incremental seed announce + heartbeat. Reports only the files that changed since the last PUT. `peer_id` is a URL path parameter.

Request:
```json
{
    "tracker_addr": "tracker1.example.com:8080",
    "add": [
        {"file_id": "abc123", "have_pct": 45.7},
        {"file_id": "def456", "have_pct": 60.3}
    ],
    "del": [
        {"file_id": "old987"}
    ]
}
```

`tracker_addr` is required on the first PUT and optional on subsequent heartbeats — the server retains the last known value. `add` and `del` are optional. An empty body (`{"tracker_addr":"..."}`) or `{}` is a pure heartbeat. `add` upserts: creates the peer+file entry if it doesn't exist, updates `have_pct` if it does. `del` removes the peer from that file's peer list.

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
- Idempotent: `add` for an already-tracked file updates `have_pct` and refreshes `last_seen`. `del` for a non-tracked file is a no-op.
- Server responds with pending signaling messages for this peer (offer/answer/candidate).
- Server returns `ttl_ms` — the peer MUST send the next PUT within this interval. Server marks files as stale if no PUT is received for that peer within TTL.
- Server MAY adjust `ttl_ms` dynamically based on load.
- Peers with `have_pct == 100.0` are full seeders; `have_pct < 100.0` are leechers.

#### Scenario: Incremental add

- **WHEN** a peer sends `PUT /peer/alice/seed {"add":[{"file_id":"abc123","have_pct":45.7}]}`
- **THEN** the server registers alice as having file abc123 at 45.7%
- **THEN** subsequent `GET /file/abc123/peer` includes alice

#### Scenario: Incremental del

- **WHEN** a peer sends `PUT /peer/alice/seed {"del":[{"file_id":"abc123"}]}`
- **THEN** the server removes alice from file abc123's peer list
- **THEN** subsequent `GET /file/abc123/peer` no longer includes alice

#### Scenario: Pure heartbeat

- **WHEN** a peer sends `PUT /peer/alice/seed {}` (empty body)
- **THEN** the server refreshes `last_seen` for all of alice's registered files
- **THEN** the response includes any pending inbox signals

#### Scenario: Inbox delivered with announce

- **WHEN** a peer sends `PUT /peer/:peer_id/seed`
- **THEN** the `signals` field in the response contains all pending signaling messages for that peer
- **THEN** an empty `signals` array means no pending signals

#### DELETE /peer/:peer_id/seed

Graceful departure. Removes the peer from ALL registered files at once.

Response `200`:
```json
{"status": "ok"}
```

Response `404` if the peer was not registered:
```json
{"error": "not_found", "message": "peer alice not registered"}
```

#### GET /file/:file_id/peer

List active peers for a file. Returns `peer_id` and `tracker_addr` for each peer — the `tracker_addr` tells the discovering peer where to send signaling messages via `POST /relay`.

Response `200`:
```json
{
    "file_id": "abc123",
    "peers": [
        {"peer_id": "bob",   "tracker_addr": "tracker1.example.com:8080"},
        {"peer_id": "carol", "tracker_addr": "tracker2.example.com:8080"}
    ]
}
```

If `file_id` has no registered peers: response `200` with empty `peers` array.

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

---

### Requirement: Tracker — data structure

The internal data structure SHALL provide these capabilities. The concrete implementation (HashMap, cross-linked list, hybrid index) is TBD — deferred to implementation phase after profiling with realistic peer/file densities.

| Capability | Complexity target | Description |
|-----------|------------------|-------------|
| Row scan | O(F) | Iterate all peers for a given `file_id`. F = number of peers with that file. Used by `GET /file/:file_id/peer`. |
| Column scan | O(P) | Iterate all files for a given `peer_id`. P = number of files registered by that peer. Used by DELETE `/peer/:peer_id/seed` and TTL expiry. |
| Row insert/update | O(1) | Add or update a peer entry for a file. Used by `PUT /peer/:peer_id/seed` add. |
| Row delete | O(1) | Remove a peer from a file's peer list. Used by `PUT .../seed` del. |
| Column delete | O(P) | Remove a peer and all its registered files. Used by DELETE `/peer/:peer_id/seed`. |
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

#### Scenario: Peer unregisters all files

- **WHEN** DELETE `/peer/:peer_id/seed` is called for a peer with 10,000+ files
- **THEN** column scan finds all files in O(P) time and removes them from each file's peer list in O(1) per file

---

### Requirement: Tracker — edge cases

#### Scenario: Stale peer pruned

- **WHEN** a peer has not sent `PUT /peer/:peer_id/seed` within its assigned TTL
- **THEN** the cleanup timer removes the peer from all its registered files and drops its inbox

#### Scenario: Message delivered on next PUT

- **WHEN** a signaling message arrives for peer alice via `POST /relay`
- **THEN** the server enqueues it in alice's inbox
- **WHEN** alice next sends `PUT /peer/alice/seed`
- **THEN** the server returns the message in the `messages` field of the response

#### Scenario: Message TTL expiry

- **WHEN** a signaling message sits in inbox for more than 30 seconds without delivery
- **THEN** the cleanup sweep removes it without delivery

#### Scenario: Rate limit exceeded

- **WHEN** a single IP sends more than 60 requests in a 1-second window
- **THEN** all subsequent requests within that window return `429`

#### Scenario: Invalid have_pct

- **WHEN** `have_pct` is negative or greater than 100.0
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
| peer × file matrix (`peer_id`, `file_id`, `have_pct`, `last_seen`) | `PUT /peer/:peer_id/seed` | `GET /file/:file_id/peer` |
| peer → tracker mapping (`tracker_addr`) | `PUT /peer/:peer_id/seed` | `GET /file/:file_id/peer` response |
| signal inboxes | `POST /relay` | `PUT /peer/:peer_id/seed` response |

The shared store is the single source of truth. Tracker instances handle HTTP routing but carry zero persistent state. Any instance can serve any request — peer discovery, signaling, heartbeat — scaling is purely horizontal by adding instances behind a load balancer.

**Benefits:** Tracker instances are stateless — scalable, replaceable, trivial to deploy. Peer state and signal inboxes survive instance restarts. No cross-tracker coordination protocol needed.

**Trade-offs:** Introduces an external dependency. PUT latency includes one round-trip to the shared store. All instances must be able to reach the store.

#### Strategy C: Tracker federation (gossip)

```
t1 ←──gossip──→ t2 ←──gossip──→ t3
```

Tracker instances form a peer-to-peer overlay and periodically exchange peer state deltas via a gossip protocol. Each instance maintains a local replica of the full peer×file matrix, synchronized through gossip. Peer discovery (`GET /file/:file_id/peer`) is served from the local replica with zero network round-trips.

**Benefits:** No external storage dependency. Read latency is local (no network hop). Naturally decentralized — aligns with P2P philosophy.

**Trade-offs:** Eventually consistent — a newly registered peer may not be visible to all instances until the next gossip round. Requires gossip protocol implementation (memberlist / SWIM / custom). Higher operational complexity than shared storage.

**Inbox handling:** Same constraint as shared storage — signal inboxes are local to the instance that received the signal. The target peer must heartbeat to that same instance to retrieve pending signals.

#### Scenario: Peer discovers peers across instances

- **WHEN** p1 queries `GET /file/abc123/peer` from t1
- **THEN** the response includes p2 with `tracker_addr` pointing to p2's registering instance (t2)
- **THEN** p1 sends `POST /relay` to t1 — t1 routes the signal to p2's inbox on t1 (NOT to t2)
- **THEN** p2 retrieves the signal by heartbeating to t1 — if p2 has moved to t2, the signal expires and p1 re-discovers p2's new `tracker_addr`

#### Scenario: Peer registers on one instance, discoverable from another

- **WHEN** p2 registers via `PUT /peer/p2/seed` on t2
- **THEN** within the consistency window (synchronous for shared storage, gossip-round for federation), p2 is visible to `GET /file/abc123/peer` queries on t1
