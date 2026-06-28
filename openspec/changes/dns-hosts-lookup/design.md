## Context

`/etc/hosts` is a standard POSIX file mapping hostnames to IP addresses. Format: `IP hostname [alias...]`. Most DNS resolvers (including c-ares, getaddrinfo, Go) check it before DNS. xdns doesn't.

## Goals / Non-Goals

**Goals:**
- Load `/etc/hosts` at client creation into an in-memory hash table
- Check hosts table before every `xDnsClientDo` query
- Return immediate, zero-latency result on hit (no DNS, no event loop)
- Support `enable_hosts` config toggle (default on)

**Non-Goals:**
- Watcher for `/etc/hosts` file changes (use `xDnsClientReloadHosts()` manually)
- IPv6 parsing from hosts file (v4 first, v6 later)
- Wildcard or CIDR hosts entries

## Decisions

### 1. Load at creation, not per-query

Hosts file is read once at `xDnsClientCreate` into an `xMap`. Each entry maps `hostname` → `xDnsRecord*` (linked list for multiple IPs per hostname). A reload function allows manual refresh.

**Rationale**: File I/O per query is unacceptable. Memory cost is negligible (typical hosts file < 100 entries × ~100 bytes = 10KB).

### 2. `xMap` keyed by lowercase hostname

Hosts file is case-insensitive (RFC 952). All hostnames normalized to lowercase on load and lookup.

### 3. `enable_hosts` in `xDnsClientConf` (default 1)

Backward-compatible: existing code that zero-initializes `xDnsClientConf` gets hosts enabled. Users who want pure DNS can explicitly disable.

### 4. Check hosts BEFORE cache and DNS

Query flow becomes:
1. Hosts lookup → hit? → callback immediately (no event loop)
2. TTL cache lookup → hit? → callback via timer
3. DNS query

## Risks / Trade-offs

- **Stale data**: Hosts file can change after client creation. Mitigation: `xDnsClientReloadHosts()`.
- **Windows**: `/etc/hosts` doesn't exist on Windows. Use `%SystemRoot%\System32\drivers\etc\hosts`. Platform ifdef in parser.
