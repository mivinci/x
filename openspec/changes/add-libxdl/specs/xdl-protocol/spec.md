## ADDED Requirements

### Requirement: Seed protocol — announce

Peers SHALL announce themselves to the Seed Server via `PUT /file/:fid/peer/:peer_id` every 5 seconds. The request body SHALL contain `signal_addr` and `have_pct`. `fid` and `peer_id` are URL path parameters. The response SHALL be `{"status":"ok"}` — it does NOT return the peer list. Use `GET /file/:fid/peer` separately for discovery.

#### Scenario: Announce with progress update

- **WHEN** a peer sends `PUT /file/abc123/peer/alice {"signal_addr":"signal1:8081","have_pct":45.7}`
- **THEN** the server returns `{"status":"ok"}`

#### Scenario: Seeder announces completion

- **WHEN** a peer sends `PUT /file/abc123/peer/alice {"signal_addr":"signal1:8081","have_pct":100.0}`
- **THEN** the peer is listed as a full seeder for subsequent peer queries

### Requirement: Seed protocol — discovery

Clients SHALL query peers via `GET /file/:fid/peer`. The response SHALL contain a JSON array of `{peer_id, signal_addr}` for each active peer. No IP addresses, P2P ports, or progress data are returned. Connectivity is established through the Signal Server, not via direct connection.

#### Scenario: Query peers for a file

- **WHEN** a client sends `GET /file/abc123/peer`
- **THEN** the response contains `{"fid":"abc123","peers":[{"peer_id":"bob","signal_addr":"signal1:8081"}]}`

### Requirement: Signal protocol — relay format

Signal messages SHALL be JSON text sent over UDP. The server is stateless — each packet is a self-contained relay operation. Messages for offline peers SHALL be dropped silently; senders SHOULD retry on timeout. Each message SHALL contain a `type` field and `from`/`to` fields identifying the sender and recipient.

#### Peer-to-Server messages

| `type` | Required fields | Description |
|--------|----------------|-------------|
| `offer` | `from`, `to`, `sdp` | WebRTC offer relay |
| `answer` | `from`, `to`, `sdp` | WebRTC answer relay |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | ICE candidate relay |

#### Server-to-Peer messages

| `type` | Fields | Description |
|--------|--------|-------------|
| `offer` | `from`, `to`, `sdp` | Relayed from another peer |
| `answer` | `from`, `to`, `sdp` | Relayed from another peer |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | Relayed from another peer |
| `error` | `message` | Relay failure (TTL expired, queue full, sender mismatch) |

No `hello`/`hello_ack`/`ping`/`pong` — no connection to authenticate or keep alive. The server SHALL forward `offer`, `answer`, `candidate` to the UDP address of the `to` peer if the peer has polled recently. Messages for peers that have not polled are enqueued (TTL 5s, max 256) and delivered on next poll. SHALL NOT interpret SDP or ICE candidates.

#### Scenario: Offer relay

- **WHEN** alice sends `{"type":"offer","from":"alice","to":"bob","sdp":"v=0\r\n..."}` via UDP to signal server
- **THEN** the server forwards the identical message to bob's last known UDP address (or enqueues it)

#### Scenario: Answer relay

- **WHEN** bob sends `{"type":"answer","from":"bob","to":"alice","sdp":"v=0\r\n..."}` via UDP
- **THEN** the server forwards the identical message to alice

#### Scenario: ICE candidate relay

- **WHEN** either peer sends `{"type":"candidate","from":"alice","to":"bob","candidate":"candidate:...","sdpMid":"0","sdpMLineIndex":0}` via UDP
- **THEN** the server forwards the ICE candidate to the recipient

#### Scenario: TTL expiry

- **WHEN** a message sits in queue for more than 5 seconds without delivery
- **THEN** the server drops it silently; sender retries after timeout

#### Scenario: Sender identity mismatch

- **WHEN** `from` field does not match the registered peer (if validation enabled)
- **THEN** the server sends `{"type":"error","message":"sender mismatch"}` to the sender

### Requirement: DataChannel protocol — message framing

All messages over DataChannel SHALL use binary framing with a 1-byte type field followed by type-specific payload. Multi-byte integers SHALL use little-endian byte order.

#### Scenario: Message type identification

- **WHEN** a DataChannel receives a binary message
- **THEN** the first byte identifies the message type (0x01-0x07)

### Requirement: DataChannel protocol — Handshake (0x01)

After WebRTC DataChannel opens, both peers SHALL send a Handshake message. The payload SHALL contain the file id (20 bytes) and peer id (32 bytes UTF-8). On receiving a Handshake, the peer SHALL validate the file id matches; if mismatch, disconnect. On valid Handshake, the peer SHALL immediately send a BitField.

#### Scenario: Handshake exchange

- **WHEN** DataChannel opens between alice and bob
- **THEN** alice sends Handshake{fid="abc123", peer_id="alice"}
- **THEN** bob sends Handshake{fid="abc123", peer_id="bob"}
- **THEN** both validate fid matches and proceed to BitField exchange

### Requirement: DataChannel protocol — BitField (0x02)

The BitField message SHALL contain the total piece count (4 bytes, LE) and a bitmap (N bytes, where N = ceil(total_pieces/8)). Bit i = 1 means piece i is available for upload. The message SHALL be sent after a valid Handshake and whenever the local bitmap changes significantly.

#### Scenario: BitField after Handshake

- **WHEN** alice receives bob's Handshake
- **THEN** alice sends BitField{total_pieces=524288, bitmap=[...]}
- **THEN** bob records which pieces alice has

### Requirement: DataChannel protocol — Request (0x03)

A peer SHALL request one piece at a time via a Request message containing the 4-byte piece_index. Requests SHALL only be sent for pieces that the recipient's BitField indicates as available.

#### Scenario: Piece request

- **WHEN** alice needs piece 42 and bob's BitField shows piece 42 as available
- **THEN** alice sends Request{piece_index=42} to bob

### Requirement: DataChannel protocol — Piece (0x04)

A Piece message SHALL respond to a Request. The payload SHALL contain piece_index (4 bytes, LE), offset within the piece (4 bytes, LE), and the piece data (N bytes). The recipient SHALL pass the data to the upper-layer download pipeline (SHA1 update, cache write, bitmap update, progress report).

#### Scenario: Piece received

- **WHEN** bob receives Request{piece_index=42}
- **THEN** bob reads the piece data from storage and sends Piece{index=42, offset=0, data=[...]}
- **THEN** alice processes the received data and updates her local bitmap

### Requirement: DataChannel protocol — HAVE (0x05)

When a peer receives a new piece, it SHALL broadcast a HAVE{piece_index} message to all connected peers. Other peers SHALL update their copy of that peer's BitField.

#### Scenario: HAVE broadcast

- **WHEN** alice completes a piece download from bob
- **THEN** alice sends HAVE{piece_index=42} to all connected peers

### Requirement: DataChannel protocol — Cancel (0x06)

A peer MAY cancel a pending Request by sending Cancel{piece_index}. This SHALL be used when the piece is obtained from another source or the request times out.

#### Scenario: Cancel pending request

- **WHEN** alice requested piece 42 from bob but received it from carol first
- **THEN** alice sends Cancel{piece_index=42} to bob
- **THEN** bob removes the pending request from his queue

### Requirement: DataChannel protocol — Disconnect (0x07)

A peer SHALL send Disconnect before closing a DataChannel. The recipient SHALL clean up the peer's state but NOT necessarily disconnect the underlying xPeerConnection if the peer still has active downloads on another channel.

#### Scenario: Graceful disconnect

- **WHEN** alice finishes downloading and no longer needs bob's pieces
- **THEN** alice sends Disconnect to bob before closing the DataChannel
