# Channels

xpp provides five channel primitives in `xpp::sync`, plus the `Notify` notification primitive:

| Channel | Pattern | Capacity | Use Case |
| --------- | --------- | ---------- | ---------- |
| [oneshot](oneshot.md) | 1→1, single-use | 1 | Deferred result, async callback |
| [mpsc](mpsc.md) | M→1 | bounded / unbounded | Work queues, task dispatch |
| [broadcast](broadcast.md) | M→N | bounded | Event fan-out, shutdown signals |
| [watch](watch.md) | M→N | 1 (latest) | Configuration hot-reload, state observation |
| [notify](notify.md) | — | — | Wake-up signal, barrier coordination |

## Choosing a channel

```text
                         One value?
                        /         \
                      yes          no
                      │             │
                  Single-use?    All consumers
                 /          \    see all values?
               yes          no   /          \
               │            │   yes         no
           oneshot       watch   │           │
                              broadcast    mpsc
```

- **oneshot**: Send a single value once. Think "async return value".
- **watch**: Keep only the latest value. Think "config that changes over time".
- **broadcast**: Every consumer sees every value. Think "event stream".
- **mpsc**: Each value consumed exactly once. Think "work queue".

All channels work with `.await()` (C++11 + fiber), `co_await` (C++20), and `.then()` (C++11 callback chains).

## Thread safety

All channels support both single-threaded and multi-threaded usage — shared
state always uses `Arc<T>` (atomic refcount); the old `XPP_MT` switch was removed.

Multi-threaded tests exist for all channels (`*_mt_test.cpp`).

## RAII close

All channels use RAII close: when the last `Sender` handle is dropped, the channel
automatically closes, notifying any blocked receivers. Explicit `close()` is also
available for early shutdown.
