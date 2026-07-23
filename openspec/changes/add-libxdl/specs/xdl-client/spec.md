## ADDED Requirements

### Requirement: Global lifecycle

The library SHALL provide `xdl_init(conf)` to create an event loop, HTTP client, thread pool, and a global tick timer. The timer SHALL fire every 1000ms, iterating all active tasks and calling each task's scheduler `on_tick`. `xdl_destroy()` SHALL tear down all global resources and cancel any in-flight tasks.

#### Scenario: Initialize and destroy in THREAD mode

- **WHEN** calling `xdl_init(&(xdl_conf_t){.mode = XDL_MODE_THREAD})` and then `xdl_destroy()`
- **THEN** all resources are correctly created and freed without leaks
- **THEN** the global tick timer is started in `init` and stopped in `destroy`

#### Scenario: No task active

- **WHEN** the global tick fires and no tasks are in the running state
- **THEN** the tick scans active tasks, finds none, and returns immediately with no side effects

#### Scenario: Double init

- **WHEN** calling `xdl_init()` a second time without `xdl_destroy()` in between
- **THEN** the second call returns an error code without corrupting state

### Requirement: Task lifecycle

The library SHALL provide `xdl_task_t` with `xdl_task_create(conf)`, `xdl_task_start`, `xdl_task_pause`, `xdl_task_resume`, `xdl_task_stop`, and `xdl_task_destroy`. `xdl_task_create` SHALL accept a `xdl_task_conf_t` struct containing `url`, `fid`, `dest`, `sha1_hex`, `timeout_ms`, `cb`, and `arg`. If `url` is non-NULL, the task SHALL use an HTTP source. If `fid` is non-NULL, the task SHALL use a P2P source (via xp2p). Both non-NULL SHALL enable hybrid HTTP+P2P scheduling.

#### Scenario: Create HTTP-only task

- **WHEN** calling `xdl_task_create(&(xdl_task_conf_t){.url="https://...", .dest="./file.bin", .cb=cb})`
- **THEN** a non-NULL task handle is returned with HTTP scheduler active

#### Scenario: Create hybrid HTTP+P2P task

- **WHEN** calling `xdl_task_create(&(xdl_task_conf_t){.url="https://...", .fid="abc123...", .dest="./file.bin", .cb=cb})`
- **THEN** a non-NULL task handle is returned with hybrid scheduler active
- **THEN** `xdl_task_start` initiates both HTTP HEAD and P2P peer discovery via xp2p

#### Scenario: Destroy task without start

- **WHEN** creating a task and immediately calling `xdl_task_destroy` without `xdl_task_start`
- **THEN** all task resources are freed without any network I/O

### Requirement: Download execution

`xdl_task_start(task)` SHALL begin downloading the file. If the file partially exists and a valid `.meta` is present, it SHALL resume from completed blocks. Otherwise it SHALL download from the beginning.

#### Scenario: Full download

- **WHEN** starting a task for a file that does not exist
- **THEN** the full file is downloaded via HTTP Range requests in 256 KB blocks

#### Scenario: Resume from partial download

- **WHEN** starting a task where the destination file exists and contains valid `.meta` with some blocks marked done
- **THEN** only the incomplete blocks are downloaded
- **THEN** existing blocks are not re-fetched

#### Scenario: Corrupt .meta file

- **WHEN** starting a task where `.meta` exists but has a mismatched magic, block_count, or total_size
- **THEN** the `.meta` is deleted and the download starts from scratch

### Requirement: SHA1 verification

The downloaded file SHALL be verified against the expected SHA1 hash using `xcrypto`. Verification SHALL be incremental (streaming) — `xSha1Update` per data chunk of each block. `xSha1Final` SHALL be called once when all blocks are downloaded, and the result compared against the expected hash.

#### Scenario: SHA1 match

- **WHEN** all blocks are downloaded and the file-level SHA1 matches the expected hash
- **THEN** the file is kept and the task transitions to done

#### Scenario: SHA1 mismatch

- **WHEN** all blocks are downloaded but the file-level SHA1 does not match
- **THEN** the task transitions to error and the file is deleted

### Requirement: Concurrency

The library SHALL drive downloads via a periodic tick timer (default 1000ms). On each tick, the scheduler SHALL scan pending blocks and dispatch up to `max_concurrent` (default 32) requests to available sources. When a range download completes, the scheduler SHALL record the result without triggering new dispatch — the next tick handles it.

#### Scenario: Tick-driven dispatch

- **WHEN** a task has 10 pending blocks
- **THEN** at most `max_concurrent` HTTP Range requests are in-flight simultaneously
- **THEN** completed blocks are dispatched in the next tick, not synchronously

#### Scenario: Multiple tasks share concurrency cap

- **WHEN** two tasks are started with blocks to download
- **THEN** the combined in-flight request count does not exceed the global concurrency cap

### Requirement: Retry on failure

Blocks that fail to download SHALL be retried up to 3 times. HTTP 403 and 404 responses SHALL NOT be retried.

#### Scenario: Network error retry

- **WHEN** a block download fails with a network error (curl_code != 0)
- **THEN** the block is re-queued for retry up to 3 times

#### Scenario: HTTP 403 not retried

- **WHEN** a block download receives HTTP 403
- **THEN** the block is marked as failed without retry

### Requirement: P2P source

When `conf.fid` is provided, the library SHALL create a P2P source backed by `xp2p`. On `xdl_task_start`, the P2P source SHALL query a tracker or DHT for peers, establish connections via WebRTC (ICE/DTLS/SCTP), exchange BitFields, and request pieces via DataChannel. Peers discovered mid-download SHALL be connected on subsequent scheduler ticks.

#### Scenario: Peer discovery on start

- **WHEN** `xdl_task_start` is called on a task with `fid`
- **THEN** the P2P source queries the tracker for peer addresses
- **THEN** the source starts connecting to peers and exchanging BitFields

#### Scenario: Piece download via P2P

- **WHEN** the hybrid scheduler dispatches a block to the P2P source
- **THEN** the P2P source sends piece requests to connected peers via DataChannel
- **THEN** received piece data is passed to `on_data` for SHA1 and cache write

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

Progress SHALL be reported via `xRelayEmit` on the task's internal relay. The `xdl_progress` struct SHALL contain `blocks_done`, `blocks_total`, `bytes_done`, `bytes_total`, and a `phase` indicating running, done, or error.

#### Scenario: Progress on block completion

- **WHEN** each block completes download and verification
- **THEN** the relay emits updated progress with incremented `blocks_done` and `bytes_done`

#### Scenario: Progress on completion

- **WHEN** all blocks are downloaded and verified
- **THEN** the relay emits progress with `phase = XDL_PHASE_DONE` and `blocks_done == blocks_total`

### Requirement: File output

The library SHALL write data to `<dest>.part` during download. On successful SHA1 verification, it SHALL atomically rename `<dest>.part` to `<dest>`. The `.meta` checkpoint file SHALL be placed at `<dest>.meta` and deleted when the download completes.

#### Scenario: Output file placement

- **WHEN** download to `./downloads/client.jar` completes
- **THEN** `./downloads/client.jar` contains the full verified file
- **THEN** `./downloads/client.jar.part` does NOT exist (renamed)
- **THEN** `./downloads/client.jar.meta` does NOT exist (deleted on completion)

#### Scenario: .meta persists on incomplete download

- **WHEN** download is interrupted before completion
- **THEN** `<dest>.meta` remains on disk for future resume
- **THEN** `<dest>.part` may exist with partial data

### Requirement: Per-task timeout

The library SHALL support a per-task HTTP timeout via `conf.timeout_ms`. The timeout SHALL be forwarded to `xHttpClientDo`. If the download stalls beyond this duration, the task SHALL transition to error.

#### Scenario: Timeout triggers error

- **WHEN** a block download stalls for longer than `conf.timeout_ms`
- **THEN** the block is retried, and if all retries fail the task transitions to error

### Requirement: No Content-Length fallback

When the server omits `Content-Length` (chunked transfer or broken HEAD response), the library SHALL fall back to a single GET request without Range-based blocks. No `.meta` checkpoint is written in this mode. SHA1 verification still runs if `sha1_hex` is provided; progress reports `blocks_total = 1`.

#### Scenario: Fallback on chunked transfer

- **WHEN** starting a task and the server does not provide Content-Length
- **THEN** the file is downloaded via a single GET request
- **THEN** no `.meta` file is created
- **THEN** progress callback reports `blocks_total = 1`

#### Scenario: sha1_hex is NULL

- **WHEN** `conf.sha1_hex` is NULL
- **THEN** SHA1 verification is skipped entirely
- **THEN** the download MAY skip Content-Length probing and use a single GET
