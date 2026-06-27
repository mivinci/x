## ADDED Requirements

### Requirement: xDnsClient — async DNS resolver

`xDnsClient` SHALL provide truly async DNS resolution over UDP, with no thread pool and no `getaddrinfo`. It uses a single non-blocking UDP socket registered with the event loop, and multiplexes concurrent queries by 16-bit transaction ID.

#### Scenario: Resolve A record

- **WHEN** `xDnsClientDo(client, "example.com", xDnsType_A, callback, arg)` is called
- **THEN** a DNS query is sent to the configured nameserver over UDP
- **AND** `callback` is invoked on the event loop thread when the response arrives
- **AND** the callback receives `xErrno_Ok` and a list of A records

#### Scenario: Cache hit

- **WHEN** a query for a previously resolved hostname (within TTL) is made
- **THEN** the callback is invoked immediately without sending a UDP query
- **AND** the cached records are returned

#### Scenario: Timeout with retry

- **WHEN** no response is received within `timeout_ms`
- **THEN** the client retries with the next nameserver (up to `retries` times)
- **AND** if all nameservers are exhausted, `callback` is invoked with `xErrno_Timeout`

### Requirement: xDnsClient configuration

```c
XDEF_STRUCT(xDnsClientConf) {
  const char *nameservers[8];  // "8.8.8.8", "1.1.1.1", ...
  int          timeout_ms;     // default 5000
  int          retries;        // default 2
  int          enable_cache;   // default 1
};
```

#### Scenario: Default nameservers from system config

- **WHEN** `nameservers[0]` is NULL
- **THEN** on POSIX systems, the client reads `/etc/resolv.conf` for `nameserver` lines
- **AND** on Windows, the client uses `GetNetworkParams()` to discover DNS servers
- **AND** if discovery fails on either platform, falls back to "8.8.8.8"

### Requirement: DNS packet builder

The client SHALL build DNS query packets according to RFC 1035: 12-byte header (random transaction ID, standard flags, QDCOUNT=1) followed by a question section (encoded QNAME, QTYPE, QCLASS=IN).

#### Scenario: Build A query

- **WHEN** building a query for "example.com" type A
- **THEN** the packet contains transaction ID, RD flag set, QNAME encoded as labels, QTYPE=1, QCLASS=1

### Requirement: DNS packet parser

The client SHALL parse DNS response packets, including: header fields (ID, flags, RCODE, counts), question section, and answer records. DNS name compression (pointer references) SHALL be handled.

#### Scenario: Parse A response

- **WHEN** parsing a response with ANCOUNT=1, TYPE=A, RDATA=4 bytes
- **THEN** the parser returns an xDnsRecord with type=A and the 4-byte IPv4 address

#### Scenario: Parse CNAME chain

- **WHEN** parsing a response with a CNAME record followed by an A record
- **THEN** the parser returns both records in order (CNAME first, A second)

### Requirement: TTL cache

The client SHALL cache resolved records with their TTL. Cached entries expire automatically. Cache hits return immediately without network I/O.

#### Scenario: Cache expiry

- **WHEN** a cached record's TTL has expired
- **THEN** the next query for that hostname sends a new DNS query
- **AND** the cache is updated with the new result

### Requirement: xDnsType — bitmask query flags

`xDnsType` SHALL be a bitmask that callers can OR together. Each bit maps to a DNS QTYPE internally. When multiple types are specified, the client sends one UDP query per type, waits for all to complete, merges results into a single `xDnsRecord` list, and invokes the callback once.

```c
typedef enum {
  xDnsType_A     = 1 << 0,   // QTYPE=1
  xDnsType_AAAA  = 1 << 1,   // QTYPE=28
  xDnsType_CNAME = 1 << 2,   // QTYPE=5
} xDnsType;
```

`xDnsRecord.qtype` stores the actual DNS QTYPE (1, 28, 5), not the bitmask flag.

#### Scenario: Query A + AAAA together

- **WHEN** `xDnsClientDo(client, "example.com", xDnsType_A | xDnsType_AAAA, cb, arg)` is called
- **THEN** two UDP queries are sent (QTYPE=1 and QTYPE=28)
- **AND** the callback is invoked once with both A and AAAA records merged

#### Scenario: Partial success

- **WHEN** A query succeeds but AAAA query times out
- **THEN** the callback is invoked with A records only and `xErrno_Ok`
- **AND** the timed-out AAAA query does not cause an overall error

#### Scenario: All queries timeout

- **WHEN** all queries in a multi-type request timeout
- **THEN** the callback is invoked with `xErrno_Timeout` and no records
