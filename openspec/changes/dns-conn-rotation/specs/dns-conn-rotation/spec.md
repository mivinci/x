# dns-conn-rotation

## ADDED Requirements

### Requirement: Per-nameserver connections

The DNS client SHALL maintain a dedicated UDP connection for each configured nameserver. Each connection SHALL have its own socket fd, query counter, and target address.

#### Scenario: Multiple nameservers get separate connections
- **WHEN** a client is created with 3 nameservers
- **THEN** each nameserver has its own UDP socket registered with the event loop

### Requirement: `udp_max_queries` configuration

The DNS client SHALL support an optional `udp_max_queries` field in `xDnsClientConf`. When set to a value greater than 0, a connection SHALL be automatically closed and reopened after that many queries are sent through it.

#### Scenario: Connection rotation at limit
- **WHEN** `udp_max_queries` is set to 100 and 100 queries have been sent on a connection
- **THEN** the 101st query sends on a new UDP socket with a different source port

### Requirement: Backward compatibility

The DNS client SHALL behave identically to the current implementation when `udp_max_queries` is 0 (default). No existing API calls SHALL break.

#### Scenario: Default behavior unchanged
- **WHEN** `udp_max_queries` is 0 (or the field is zero-initialized)
- **THEN** the client uses the current single-socket model with no connection rotation
