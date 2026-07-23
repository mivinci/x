## ADDED Requirements

### Requirement: Seed protocol — announce

Peers SHALL announce themselves to the Seed Server via `PUT /file/:fid/peer/:peer_id` every 5 seconds. The request body SHALL contain `host`, `port`, and `have_pct`. `fid` and `peer_id` are URL path parameters. The response SHALL contain the current active peer list for the file (excluding the announcing peer).

#### Scenario: Announce with progress update

- **WHEN** a peer sends `PUT /file/abc123/peer/alice {"host":"10.0.0.1","port":9000,"have_pct":45.7}`
- **THEN** the server returns `{"peers":[{"peer_id":"bob","host":"10.0.0.2","port":9000,"have_pct":100.0}]}`

#### Scenario: Seeder announces completion

- **WHEN** a peer sends `PUT /file/abc123/peer/alice {"host":"10.0.0.1","port":9000,"have_pct":100.0}`
- **THEN** the peer is listed as a full seeder for subsequent peer queries

### Requirement: Seed protocol — discovery

Clients SHALL query peers via `GET /file/:fid/peer`. The response SHALL contain a JSON array of `{peer_id, host, port, have_pct}` for each active peer.

#### Scenario: Query peers for a file

- **WHEN** a client sends `GET /file/abc123/peer`
- **THEN** the response contains all peer entries registered within the last 5 seconds

### Requirement: Signal protocol — relay format

Signal messages SHALL be JSON over WebSocket text frames. Each message SHALL contain a `type` field identifying the message kind.

#### Client-to-Server messages

| `type` | Required fields | Description |
|--------|----------------|-------------|
| `hello` | `peer_id` | Identify self. MUST be first message (within 5s). |
| `offer` | `from`, `to`, `sdp` | WebRTC offer relay |
| `answer` | `from`, `to`, `sdp` | WebRTC answer relay |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | ICE candidate relay |
| `ping` | (none) | Keepalive |

#### Server-to-Client messages

| `type` | Fields | Description |
|--------|--------|-------------|
| `hello_ack` | (none) | Auth accepted |
| `offer` | `from`, `to`, `sdp` | Relayed from another peer |
| `answer` | `from`, `to`, `sdp` | Relayed from another peer |
| `candidate` | `from`, `to`, `candidate`, `sdpMid`, `sdpMLineIndex` | Relayed from another peer |
| `error` | `message` | Error response |
| `pong` | (none) | Keepalive response |

The server SHALL forward `offer`, `answer`, `candidate` messages to the WebSocket of the peer identified by the `to` field without modification. The server SHALL NOT interpret SDP, ICE candidates, or any payload beyond the routing fields. Messages with a `from` field that does not match the sender's authenticated `peer_id` SHALL be rejected.

#### Scenario: Auth handshake

- **WHEN** alice opens a WebSocket connection and sends `{"type":"hello","peer_id":"alice"}`
- **THEN** the server responds with `{"type":"hello_ack"}` and alice may begin signaling

#### Scenario: Auth timeout

- **WHEN** a WebSocket connection sends no message within 5 seconds
- **THEN** the server closes the connection

#### Scenario: Offer relay

- **WHEN** alice sends `{"type":"offer","from":"alice","to":"bob","sdp":"v=0\r\n..."}`
- **THEN** the server forwards the identical message to bob's WebSocket

#### Scenario: Answer relay

- **WHEN** bob sends `{"type":"answer","from":"bob","to":"alice","sdp":"v=0\r\n..."}`
- **THEN** the server forwards the identical message to alice's WebSocket

#### Scenario: ICE candidate relay

- **WHEN** either peer sends `{"type":"candidate","from":"alice","to":"bob","candidate":"candidate:...","sdpMid":"0","sdpMLineIndex":0}`
- **THEN** the server forwards the ICE candidate to the recipient

#### Scenario: Keepalive ping/pong

- **WHEN** the server sends `{"type":"ping"}`
- **THEN** the client responds with `{"type":"pong"}`
- **WHEN** the client does not respond within 10 seconds
- **THEN** the server closes the WebSocket connection

#### Scenario: Sender identity mismatch

- **WHEN** bob (authenticated as "bob") sends `{"type":"offer","from":"alice","to":"carol","sdp":"..."}`
- **THEN** the server rejects with `{"type":"error","message":"sender mismatch"}`

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
