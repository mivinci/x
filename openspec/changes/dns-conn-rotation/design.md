## Context

xdns currently uses a single `xSocket` (UDP) for all queries, multiplexed by QID via `xMap`. This works for low throughput but breaks down at high QPS due to:
1. Shared QID space (65535 max per socket)
2. Single kernel recv buffer accumulating dead query responses
3. Single source port visible to DNS servers (rate-limiting)

c-ares solves this with per-server connections that rotate after `udp_max_queries`.

## Goals / Non-Goals

**Goals:**
- Add per-nameserver UDP connections
- Add `udp_max_queries` config to `xDnsClientConf`
- Auto-rotate connections when limit exceeded
- Backward-compatible (default `udp_max_queries = 0` = no rotation, single socket)

**Non-Goals:**
- TCP connections for DNS (future)
- Connection pooling across event loop iterations
- Per-server health metrics

## Decisions

### 1. `xDnsConf_` struct per nameserver

Each nameserver gets one active `xDnsConn_` at a time:

```c
struct xDnsConn_ {
  xSocket     sock;            // UDP socket for this server
  struct sockaddr_storage addr;
  socklen_t   addrlen;
  int         query_count;     // queries sent since open
  int         max_queries;     // rotation threshold (0 = unlimited)
};
```

Stored in `struct xDnsClient_` as an array: `struct xDnsConn_ conns[MAX_NS];`

### 2. Rotation logic

Before sending a query:
1. `conn = &c->conns[ns_index]`
2. If `conn->max_queries > 0 && conn->query_count >= conn->max_queries`:
   - Close old socket (`xSocketClose`)
   - Create new socket with fresh ephemeral port
   - Reset `query_count = 0`
3. Increment `query_count`
4. Send via `conn->sock`

### 3. Socket registration

The new per-server socket must be registered with the event loop for Read events. On rotation, the old socket is unregistered, the new one registered. The read callback determines which connection the packet arrived on from the socket fd:

```c
static void on_readable(xSocket sock, ...) {
  // Find conn by sock fd
  xDnsConn_ *conn = find_conn_by_fd(c, sock);
  // Read and dispatch as before
}
```

Alternative: use a single callback per conn, registered with its own arg. Simpler but more callbacks. Decision: single callback with fd lookup (fewer event loop registrations).

### 4. Backward compatibility

When `udp_max_queries = 0` (default), behavior is identical to current: single socket, no rotation. The conn array has one element representing the shared socket.

### 5. QID allocation

QID allocation remains global (`next_id++`) because UDP responses can arrive on any connection's socket. The QID space is reset when all connections are rotated (practically, with multiple nameservers the QID space rarely fills).

## Risks / Trade-offs

- **Connection rotation adds fd churn**: Each rotation is `close()` + `socket()` + `xSocketCreate()`. At 100 queries/rotation, this is negligible overhead. At 10000 queries/rotation, even less.
- **Multiple sockets = more event loop registrations**: Each active connection has its own fd in the event loop. For 2-8 nameservers, this is 2-8 fds — trivial.
