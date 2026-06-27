## ADDED Requirements

### Requirement: xDnsServer — DNS server with forwarding and authoritative modes

`xDnsServer` SHALL listen on a UDP port and respond to DNS queries. It supports two modes: authoritative (serve records from local zones) and forwarding (forward unresolved queries to an upstream `xDnsClient`). Both modes can coexist — local zones are checked first, misses are forwarded.

#### Scenario: Authoritative response

- **WHEN** a query for a name in a local zone is received
- **THEN** the server responds with the zone's records
- **AND** no forwarding occurs

#### Scenario: Forwarding response

- **WHEN** a query for a name NOT in any local zone is received
- **AND** a forwarder (`xDnsClient`) is configured
- **THEN** the server forwards the query to the upstream resolver
- **AND** when the upstream responds, the server sends the response to the client

#### Scenario: No forwarder configured

- **WHEN** a query for a name NOT in any local zone is received
- **AND** no forwarder is configured
- **THEN** the server responds with NXDOMAIN (RCODE=3)

### Requirement: xDnsServer configuration

```c
XDEF_STRUCT(xDnsServerConf) {
  xDnsClient    forwarder;      // upstream resolver (NULL = authoritative only)
  xDnsFilterFunc filter;        // query filter (NULL = no filter)
  void         *filter_arg;
  int           cache_enabled;  // share cache with forwarder
};
```

#### Scenario: Forwarder + zone

- **WHEN** both a forwarder and zones are configured
- **THEN** zone queries are answered locally
- **AND** non-zone queries are forwarded to the upstream resolver

### Requirement: xDnsZone — zone record management

`xDnsZone` SHALL manage DNS records programmatically. Records can be added at runtime.

```c
XCAPI(xDnsZone) xDnsZoneCreate(void);
XCAPI(void)     xDnsZoneDestroy(xDnsZone zone);
XCAPI(void)     xDnsZoneAdd(xDnsZone zone, const char *name, xDnsType type,
                            const void *rdata, size_t rdlen, uint32_t ttl);
```

#### Scenario: Add A record to zone

- **WHEN** `xDnsZoneAdd(zone, "myapp.local", xDnsType_A, ip_bytes, 4, 3600)` is called
- **THEN** the zone contains an A record for "myapp.local" with the given IP and TTL

#### Scenario: Query zone record

- **WHEN** the server receives a query for "myapp.local" type A
- **THEN** the zone is searched and the matching record is returned in the response

### Requirement: Filter callback

When a filter callback is set, every incoming query SHALL be passed to the filter before processing. The filter can block the query (return non-zero) or allow it (return 0).

```c
typedef int (*xDnsFilterFunc)(const char *name, xDnsType type, void *arg);
// Return 0 = allow, non-zero = block (respond with NXDOMAIN)
```

#### Scenario: Filter blocks query

- **WHEN** the filter returns non-zero for "ads.example.com"
- **THEN** the server responds with NXDOMAIN without forwarding or checking zones

#### Scenario: Filter allows query

- **WHEN** the filter returns 0 for "example.com"
- **THEN** normal processing continues (zone check → forward)

### Requirement: DNS response builder

The server SHALL build DNS response packets according to RFC 1035: copy transaction ID and question from the query, set QR=1, set RCODE appropriately, and append answer records.

#### Scenario: Build response with A record

- **WHEN** building a response for an A query with one A record
- **THEN** the packet contains the original question, ANCOUNT=1, and the A record in the answer section

#### Scenario: Build NXDOMAIN response

- **WHEN** building a response for a query with no matching records and no forwarder
- **THEN** the packet has RCODE=3 (NXDOMAIN) and ANCOUNT=0
