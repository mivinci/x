## ADDED Requirements

### Requirement: Global lifecycle

The library SHALL provide `xdl_init(conf)` and `xdl_destroy()` for global lifecycle. `xdl_conf_t` SHALL accept `mode` (XDL_MODE_INHERIT / XDL_MODE_THREAD), `peer_id`, `tracker_url`, `cache_bytes`, and `concurrency`. `xdl_init` SHALL create the event loop, HTTP client, thread pool, global P2P module, and a global housekeeping timer (1000ms) for task completion scanning and cleanup.

Each task's scheduler SHALL register its own per-task tick timer in its `on_start` callback, and cancel it in `on_stop`. The global housekeeping timer does NOT drive per-task scheduling.

#### Scenario: Initialize with peer_id and tracker_url

- **WHEN** calling `xdl_init(&(xdl_conf_t){.mode = XDL_MODE_THREAD, .peer_id = "alice", .tracker_url = "http://t1:8080"})` and then `xdl_destroy()`
- **THEN** all resources are correctly created and freed without leaks
- **THEN** the global housekeeping timer is started in `init` and stopped in `destroy`
- **THEN** the peer identifies as "alice" in all Tracker communications

#### Scenario: No task active

- **WHEN** the global tick fires and no tasks are in the running state
- **THEN** the tick scans active tasks, finds none, and returns immediately with no side effects

#### Scenario: Double init

- **WHEN** calling `xdl_init()` a second time without `xdl_destroy()` in between
- **THEN** the second call returns an error code without corrupting state

### Requirement: Task lifecycle

The library SHALL provide `xdl_task_t` with `xdl_task_create(conf)`, `xdl_task_start`, `xdl_task_pause`, `xdl_task_resume`, `xdl_task_stop`, and `xdl_task_destroy`. `xdl_task_create` SHALL accept a `xdl_task_conf_t` struct containing `magnet`, `torrent`, `urls`, `sha1`, `sched`, `dir`, `max_peers`, `timeout_ms`, `cb`, and `arg`. At least one of `magnet`, `torrent`, or `urls` MUST be provided.

The scheduler is determined by which fields are set:
- `magnet` or `torrent` only → P2P
- `magnet`/`torrent` + `urls` → Hybrid
- `urls` only → HTTP-only
- None → error

When both `magnet` and `torrent` are provided, `torrent` takes precedence and `magnet` is ignored.

#### Scenario: URL-only HTTP download

- **WHEN** calling `xdl_task_create(&{.urls = "https://cdn.example.com/file.bin", .dir = "./dl", .cb = cb})`
- **THEN** a non-NULL task handle is returned with HTTP-only scheduler
- **THEN** `xdl_task_start` downloads directly from the CDN URL, no tracker interaction

#### Scenario: HTTP-only with file-level SHA1

- **WHEN** calling `xdl_task_create(&{.urls = "https://cdn.example.com/file.bin", .sha1 = "a1b2c3...", .dir = "./dl", .cb = cb})`
- **THEN** the full file is downloaded via HTTP Range requests
- **THEN** `xSha1Final` runs once at completion and compares against `conf.sha1`
- **THEN** on match the file is kept; on mismatch the file is deleted and task transitions to error

- **WHEN** calling `xdl_task_create(&{.urls = "https://cdn.example.com/file.bin", .dir = "./dl", .cb = cb})`
- **THEN** a non-NULL task handle is returned with HTTP-only scheduler
- **THEN** `xdl_task_start` downloads directly from the CDN URL, no tracker interaction

#### Scenario: Direct torrent (no tracker needed)

- **WHEN** calling `xdl_task_create(&{.torrent = t, .dir = "./dl", .max_peers = 8, .cb = cb})`
- **THEN** a non-NULL task handle is returned with P2P scheduler
- **THEN** `xdl_task_start` uses the provided torrent directly, no `GET /torrent` call

#### Scenario: Hybrid with magnet and CDN URLs

- **WHEN** calling `xdl_task_create(&{.magnet = "magnet:?xt=urn:btih:...&tr=http://t1:8080", .urls = "https://cdn.example.com/file.bin", .dir = "./dl", .max_peers = 8, .cb = cb})`
- **THEN** a hybrid scheduler is created with HTTP source from CDN URLs and P2P source from magnet
- **THEN** `xdl_task_start` fetches torrent from Tracker for block metadata, and uses CDN URLs for HTTP range requests

#### Scenario: No source provided

- **WHEN** calling `xdl_task_create` with `magnet`, `torrent`, and `urls` all NULL
- **THEN** `xdl_task_create` returns NULL

#### Scenario: Destroy task without start

- **WHEN** creating a task and immediately calling `xdl_task_destroy` without `xdl_task_start`
- **THEN** all task resources are freed without any network I/O

### Requirement: Download execution

`xdl_task_start(task)` SHALL begin downloading the file. If the target file partially exists and a valid `.resume` is present with matching `info_hash`, it SHALL resume from completed blocks. Otherwise it SHALL download from the beginning.

#### Scenario: Full download

- **WHEN** starting a task for a file that does not exist
- **THEN** the full file is downloaded via HTTP Range requests in `block length` sized blocks

#### Scenario: Resume from partial download

- **WHEN** starting a task where the destination file exists and contains a valid `.resume` with some blocks marked done
- **THEN** only the incomplete blocks are downloaded
- **THEN** existing blocks are not re-fetched

#### Scenario: Corrupt .resume file

- **WHEN** starting a task where `.resume` exists but has a mismatched magic or `info_hash`
- **THEN** the `.resume` is deleted and the download starts from scratch

#### Scenario: Block count mismatch in .resume

- **WHEN** starting a task where `.resume` exists but has a different `block_count` than the torrent
- **THEN** the `.resume` is deleted and the download starts from scratch

### Requirement: Per-block SHA1 verification

Each downloaded block SHALL be verified against the corresponding SHA1 hash from the torrent's `info.blocks`. Verification SHALL be incremental — `xSha1Update` per data chunk and `xSha1Final` per block.

#### Scenario: Block SHA1 match

- **WHEN** a block is downloaded and its SHA1 matches the hash at the corresponding index in `info.blocks`
- **THEN** the block is written to disk and marked done in the `.resume` bitmap

#### Scenario: Block SHA1 mismatch

- **WHEN** a block is downloaded but its SHA1 does not match
- **THEN** the block is discarded, the source is notified of failure (`ok=false`), and the block is re-queued for retry

### Requirement: Concurrency

The library SHALL drive downloads via a periodic tick timer (default 1000ms). On each tick, the scheduler SHALL scan pending blocks and dispatch up to `max_concurrent` (default 32) requests to available sources. When a block download completes, the scheduler SHALL record the result without triggering new dispatch — the next tick handles it. A future optimization MAY allow the HTTP-only scheduler to immediately dispatch on `on_block_done`.

#### Scenario: Tick-driven dispatch

- **WHEN** a task has 10 pending blocks
- **THEN** at most `max_concurrent` HTTP Range requests are in-flight simultaneously
- **THEN** completed blocks are dispatched in the next tick, not synchronously

#### Scenario: Concurrency per source

- **WHEN** a block download exceeds the source's in-flight limit
- **THEN** the scheduler waits for an in-flight block to complete before dispatching the next one

### Requirement: Retry on failure

Blocks that fail to download SHALL be retried up to 3 times. HTTP 403 and 404 responses SHALL NOT be retried.

#### Scenario: Network error retry

- **WHEN** a block download fails with a network error (curl_code != 0)
- **THEN** the block is re-queued for retry up to 3 times

#### Scenario: HTTP 403 not retried

- **WHEN** a block download receives HTTP 403
- **THEN** the block is marked as failed without retry

### Requirement: Pct computation

`pct` SHALL be client-computed as `blocks_done / blocks_total * 100.0`. The server stores it as-is without validation. `pct: 0.0` removes the peer from that file's peer list.

The library SHALL create a global P2P module on `xdl_init`. The module SHALL use the `peer_id` provided in `xdl_conf` for all Tracker communication, and SHALL start a global announce timer that periodically calls `PUT /announce` with a `changes` array aggregating all active P2P sources' `{info_hash, pct}`. Each P2P source (per task) SHALL handle data transfer only: bitfield exchange, block requests/receives, and peer state management.

#### Scenario: Global announce aggregates all tasks

- **WHEN** two tasks with different `info_hash` values are running
- **THEN** the global announce timer sends a single `PUT /announce` with peer_id and both `info_hash` values in the `changes` array
- **THEN** each task's `pct` reflects its individual download progress

#### Scenario: Signals routed to correct source

- **WHEN** the `PUT` response contains signals for two different `info_hash` values
- **THEN** each signal is dispatched to the P2P source for the corresponding `info_hash`

#### Scenario: Peer discovery on start

- **WHEN** `xdl_task_start` is called on a task with a torrent containing `announce`
- **THEN** the global P2P module includes this `info_hash` in the next announce
- **THEN** on the subsequent tick, calls `GET /torrent/<info_hash_hex>/peer` to discover peers
- **THEN** connects to new peers via WebRTC, exchanging bitfields via DataChannel

#### Scenario: Task stop removes from announce

- **WHEN** the last P2P task is stopped
- **THEN** the global announce sends a final PUT with `pct: 0.0` for the task's `info_hash`

#### Scenario: Block download via P2P

- **WHEN** the hybrid scheduler dispatches a block to the P2P source
- **THEN** the P2P source sends block requests to connected peers via DataChannel
- **THEN** received block data is SHA1-verified and passed to `on_data` for cache write

### Requirement: PeerConnection and DataChannel management

The global P2P module SHALL manage PeerConnections keyed by `peer_id`. One peer pair SHALL share at most one PeerConnection. Multiple torrents between the same peers SHALL use separate DataChannels on the same PeerConnection, keyed by `info_hash` as the DataChannel label.

#### Scenario: PeerConnection reuse across torrents

- **WHEN** two P2P sources (different `info_hash`) connect to the same remote peer
- **THEN** only one PeerConnection is established for that peer_id
- **THEN** each source gets its own DataChannel on the shared connection
- **THEN** DataChannel labels match their respective `info_hash` values

#### Scenario: New DataChannel on existing connection

- **WHEN** a P2P source needs to connect to a peer that already has an established PeerConnection
- **THEN** a new DataChannel is created on the existing connection (no ICE renegotiation)
- **THEN** the remote peer routes the incoming DataChannel to the correct source via its label

#### Scenario: First connection triggers full ICE handshake

- **WHEN** a P2P source needs to connect to a peer with no existing PeerConnection
- **THEN** the global module creates a PeerConnection, generates an offer, and sends it via `POST /relay`
- **THEN** ICE candidates are exchanged via relay inbox
- **THEN** DataChannel is created locally with `info_hash` as label
- **THEN** the remote peer's `ondatachannel` callback routes the DataChannel to the correct source via label

#### Scenario: Incoming signal routed to correct source

- **WHEN** the global announce response contains signal `{"type":"offer","from":"bob","sdp":"..."}`
- **THEN** the global module processes the offer and creates/updates the PeerConnection for bob
- **THEN** DataChannel `ondatachannel` fires with label = `info_hash` → routed to the owning P2P source

### Requirement: DataChannel handshake

When a DataChannel becomes ready (`ondatachannel` or `onopen` callback), the initiator SHALL immediately send a Handshake message containing `info_hash` (20 bytes) and `peer_id` (32 bytes UTF-8). The receiver SHALL validate `info_hash` against the expected value for this source. Mismatched `info_hash` SHALL cause the connection to be rejected.

### Requirement: Seek API

The library SHALL provide `xdl_task_seek(task, offset)` to set the current playback position. It SHALL return 0 on success or -1 if the task is stopped/destroyed. Schedulers SHALL prioritize blocks within a seek window `[seek - pre_bytes, seek + post_bytes]` for immediate HTTP download; blocks outside the window SHALL be downloaded via P2P if available.

#### Scenario: Seek updates download priority

- **WHEN** calling `xdl_task_seek(task, 104857600)` (100MB)
- **THEN** blocks near 100MB are prioritised over blocks at 0MB

#### Scenario: Seek at offset 0

- **WHEN** `seek_point = 0` (default)
- **THEN** the download proceeds sequentially from block 0 — no prioritisation change

#### Scenario: Seek beyond file size

- **WHEN** calling `xdl_task_seek(task, offset)` where offset > total_size
- **THEN** `seek_point` is clamped to `max(0, total_size - 1)`, or 0 if total_size is unknown

#### Scenario: Seek into already-downloaded region

- **WHEN** seeking into a region where blocks are already marked done
- **THEN** no duplicate requests are issued; the scheduler skips completed blocks

### Requirement: Pause and resume

The library SHALL provide `xdl_task_pause(task)` and `xdl_task_resume(task)` as a lightweight alternative to stop/start. `xdl_task_pause` SHALL stop dispatch for the task (`on_tick` becomes a no-op) while keeping sources, peer connections, and checkpoint state intact. `xdl_task_resume` SHALL re-enable dispatch on the next tick. Both SHALL return 0 on success or -1 if the task is stopped/destroyed.

#### Scenario: Pause stops dispatch but keeps connections

- **WHEN** calling `xdl_task_pause` on a running task
- **THEN** no new blocks are dispatched
- **THEN** existing HTTP/P2P connections remain open
- **THEN** the progress relay does NOT emit an error

#### Scenario: Resume restores dispatch

- **WHEN** calling `xdl_task_resume` on a paused task
- **THEN** the next scheduler tick resumes dispatching pending blocks

#### Scenario: Pause a completed task

- **WHEN** calling `xdl_task_pause` on a task already in DONE phase
- **THEN** the call is a no-op and returns 0

### Requirement: Progress reporting

Progress SHALL be reported via `xRelayEmit` on the task's internal relay. The `xdl_progress` struct SHALL contain `blocks_done`, `blocks_total`, `bytes_done`, `bytes_total`, and a `phase` indicating running, done, or error.

#### Scenario: Progress on block completion

- **WHEN** each block completes download and verification
- **THEN** the relay emits updated progress with incremented `blocks_done` and `bytes_done`

#### Scenario: Progress on completion

- **WHEN** all blocks are downloaded and verified
- **THEN** the relay emits progress with `phase = XDL_PHASE_DONE` and `blocks_done == blocks_total`

### Requirement: File output

The library SHALL write data to `<dest>.part` during download. The resume bitmap SHALL be stored at `<dest>.part.resume`. On successful SHA1 verification of all blocks, it SHALL atomically rename `<dest>.part` to `<dest>` and delete the `.resume` file.

#### Scenario: Output file placement

- **WHEN** download to `./downloads/client.jar` completes
- **THEN** `./downloads/client.jar` contains the full verified file
- **THEN** `./downloads/client.jar.part` does NOT exist (renamed)
- **THEN** `./downloads/client.jar.part.resume` does NOT exist (deleted on completion)

#### Scenario: .resume persists on incomplete download

- **WHEN** download is interrupted before completion
- **THEN** `<dest>.part.resume` remains on disk for future resume
- **THEN** `<dest>.part` may exist with partial data

#### Scenario: .torrent is never modified

- **WHEN** a download progresses through multiple blocks
- **THEN** the `.torrent` file is an internal transient artifact — fetched from Tracker, parsed, and not persisted to disk by the downloader

### Requirement: Per-task timeout

The library SHALL support a per-task HTTP timeout via `conf.timeout_ms`. The timeout SHALL be forwarded to `xHttpClientDo`. If the download stalls beyond this duration, the task SHALL transition to error.

#### Scenario: Timeout triggers error

- **WHEN** a block download stalls for longer than `conf.timeout_ms`
- **THEN** the block is retried, and if all retries fail the task transitions to error

### Requirement: No Content-Length fallback

When the server omits `Content-Length` (chunked transfer or broken HEAD response), the library SHALL fall back to a single GET request without Range-based blocks. No `.resume` checkpoint is written in this mode. SHA1 verification still runs if the torrent provides block hashes; progress reports `blocks_total = 1`.

#### Scenario: Fallback on chunked transfer

- **WHEN** starting a task and the server does not provide Content-Length
- **THEN** the file is downloaded via a single GET request
- **THEN** no `.resume` file is created
- **THEN** progress callback reports `blocks_total = 1`
