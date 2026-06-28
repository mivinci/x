## Why

xdns currently doesn't resolve hostnames from `/etc/hosts`. For local development, service discovery (`myapp.local → 127.0.0.1`), and compatibility with existing applications, `/etc/hosts` lookup is a basic expectation. c-ares includes this, and so should xdns.

## What Changes

- Add `/etc/hosts` parser to `dns_client.c` (or new `dns_hosts.c`)
- Add `enable_hosts` field to `xDnsClientConf` (default 1)
- On `xDnsClientCreate`: load hosts file into hash table if enabled
- On `xDnsClientDo`: check hosts table before DNS query; return immediately on hit
- Add `xDnsClientReloadHosts()` for runtime refresh
- No API breakage: existing `xDnsClientConf` defaults are backward-compatible

## Capabilities

### New Capabilities

- `dns-hosts-lookup`: `/etc/hosts` file lookup at creation time, checked before DNS queries. Configurable via `xDnsClientConf.enable_hosts`.

## Impact

- `libx/x/dns/dns.h` — add `enable_hosts` to `xDnsClientConf`
- `libx/x/dns/dns_client.c` — hosts loading + lookup in `Create`/`Do`
- `libx/x/dns/dns_test.cpp` — test hosts lookup
- `libx/x/dns/CMakeLists.txt` — new source file if split
