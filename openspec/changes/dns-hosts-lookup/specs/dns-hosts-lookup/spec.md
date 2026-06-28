# dns-hosts-lookup

## ADDED Requirements

### Requirement: Load /etc/hosts at creation

The DNS client SHALL load the system hosts file (`/etc/hosts` on POSIX, `C:\Windows\System32\drivers\etc\hosts` on Windows) into an in-memory hash table when `enable_hosts` is enabled (non-zero). The parsing logic SHALL be identical across platforms (same `IP hostname [alias...]` format).

#### Scenario: Default with hosts enabled on POSIX
- **WHEN** a client is created with zero-initialized `xDnsClientConf` on Linux/macOS
- **THEN** `/etc/hosts` is loaded and ready for lookup

#### Scenario: Hosts loaded on Windows
- **WHEN** a client is created with `enable_hosts = 1` on Windows
- **THEN** the hosts file at the Windows system path is loaded

#### Scenario: Disabled via config
- **WHEN** `enable_hosts = 0` in `xDnsClientConf`
- **THEN** no hosts file is loaded and `xDnsClientDo` goes directly to DNS

### Requirement: Hosts lookup before DNS

The DNS client SHALL check the hosts table before any DNS query when `enable_hosts` is enabled.

#### Scenario: Hit returns immediately
- **WHEN** a hostname is in `/etc/hosts`
- **THEN** the callback is invoked immediately with the resolved address, without any DNS query

#### Scenario: Miss falls through to DNS
- **WHEN** a hostname is NOT in `/etc/hosts`
- **THEN** normal DNS resolution proceeds

### Requirement: Reload hosts at runtime

The DNS client SHALL provide `xDnsClientReloadHosts()` to reload the hosts file without recreating the client.

#### Scenario: Manual reload
- **WHEN** `xDnsClientReloadHosts(client)` is called
- **THEN** the hosts file is re-read and the internal table updated
