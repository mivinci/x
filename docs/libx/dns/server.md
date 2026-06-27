# xDnsServer — DNS Server

## Overview

`xDnsServer` provides a DNS server that listens on a UDP port and responds to queries. It supports two modes that can coexist:

- **Authoritative** — serve records from local zones
- **Forwarding** — forward unresolved queries to an upstream `xDnsClient`

An optional **filter callback** can intercept, block, or rewrite queries before processing.

## Types

### xDnsServerConf — configuration

```c
XDEF_STRUCT(xDnsServerConf) {
  xDnsClient    forwarder;      // Upstream resolver (NULL = authoritative only)
  xDnsFilterFunc filter;        // Query filter (NULL = no filter)
  void         *filter_arg;      // Argument passed to filter
  int           cache_enabled;   // Share cache with forwarder (default 1)
};
```

### xDnsFilterFunc — query filter

```c
typedef int (*xDnsFilterFunc)(const char *name, uint16_t type, void *arg);
// Return 0 = allow, non-zero = block (respond with NXDOMAIN)
```

### xDnsZone — zone record management

```c
XDEF_HANDLE(xDnsZone);

XCAPI(xDnsZone) xDnsZoneCreate(void);
XCAPI(void)     xDnsZoneDestroy(xDnsZone zone);
XCAPI(xErrno)   xDnsZoneAdd(xDnsZone zone, const char *name, xDnsType type,
                            const void *rdata, size_t rdlen, uint32_t ttl);
```

## API

```c
XCAPI(xDnsServer) xDnsServerCreate(const xDnsServerConf *conf);
XCAPI(void)        xDnsServerDestroy(xDnsServer server);
XCAPI(xErrno)      xDnsServerListen(xDnsServer server, const char *host, uint16_t port);
XCAPI(xErrno)      xDnsServerAddZone(xDnsServer server, xDnsZone zone);
```

## Usage Examples

### Authoritative server

```c
// Create a zone with local records
xDnsZone zone = xDnsZoneCreate();

// Add an A record: myapp.local → 192.168.1.100
uint8_t ip[4] = {192, 168, 1, 100};
xDnsZoneAdd(zone, "myapp.local", xDnsType_A, ip, 4, 3600);

// Create server (authoritative only — no forwarder)
xDnsServerConf conf = {};
conf.forwarder = NULL;

xDnsServer server = xDnsServerCreate(&conf);
xDnsServerAddZone(server, zone);
xDnsServerListen(server, "0.0.0.0", 5353);

// Queries for "myapp.local" → 192.168.1.100
// Queries for anything else → NXDOMAIN
```

### Forwarding server (local DNS relay)

```c
// Upstream resolver (queries 8.8.8.8)
xDnsClientConf cconf = {};
cconf.nameservers[0] = "8.8.8.8";
xDnsClient upstream = xDnsClientCreate(&cconf);

// Local server forwards to upstream
xDnsServerConf sconf = {};
sconf.forwarder    = upstream;
sconf.cache_enabled = 1;  // share cache with upstream

xDnsServer server = xDnsServerCreate(&sconf);
xDnsServerListen(server, "0.0.0.0", 53);

// All queries → forwarded to 8.8.8.8 → cached → returned to client
```

### Authoritative + forwarding (hybrid)

```c
xDnsZone zone = xDnsZoneCreate();
uint8_t local_ip[4] = {10, 0, 0, 1};
xDnsZoneAdd(zone, "internal.corp", xDnsType_A, local_ip, 4, 3600);

xDnsClient upstream = xDnsClientCreate(&(xDnsClientConf){
    .nameservers = {"8.8.8.8"} });

xDnsServerConf conf = {};
conf.forwarder = upstream;

xDnsServer server = xDnsServerCreate(&conf);
xDnsServerAddZone(server, zone);
xDnsServerListen(server, "0.0.0.0", 53);

// "internal.corp" → 10.0.0.1 (from zone)
// "google.com" → forwarded to 8.8.8.8 → cached → returned
```

### DNS filter (ad blocking)

```c
int ad_filter(const char *name, uint16_t type, void *arg) {
    // Block known ad domains
    if (strstr(name, "ads.") || strstr(name, "tracker.")) {
        return 1;  // block → NXDOMAIN
    }
    return 0;  // allow
}

xDnsClient upstream = xDnsClientCreate(&(xDnsClientConf){
    .nameservers = {"8.8.8.8"} });

xDnsServerConf conf = {};
conf.forwarder  = upstream;
conf.filter     = ad_filter;
conf.filter_arg = NULL;

xDnsServer server = xDnsServerCreate(&conf);
xDnsServerListen(server, "0.0.0.0", 53);

// "ads.example.com" → NXDOMAIN (blocked)
// "example.com" → forwarded to 8.8.8.8
```

## Query Processing Flow

```
UDP query received
    │
    ├─ Parse query packet
    │
    ├─ Filter callback (if set)
    │   └─ Block? → send NXDOMAIN
    │
    ├─ Check local zones
    │   └─ Hit? → build response with zone records → send
    │
    ├─ Forwarder configured?
    │   ├─ Yes → xDnsClientDo(forwarder, ...)
    │   │        → upstream resolves → build response → send
    │   └─ No → send NXDOMAIN
    │
    └─ Response sent to client via sendto
```

## Zone Records

`xDnsZone` manages DNS records programmatically. Records are matched by name (case-insensitive) and type. Multiple records for the same name+type are supported (returned as multiple answer records).

```c
// Multiple A records for round-robin
uint8_t ip1[4] = {10, 0, 0, 1};
uint8_t ip2[4] = {10, 0, 0, 2};
xDnsZoneAdd(zone, "api.local", xDnsType_A, ip1, 4, 300);
xDnsZoneAdd(zone, "api.local", xDnsType_A, ip2, 4, 300);
// Query for "api.local" → both IPs in answer section
```

## RFC Compliance

| RFC | Compliance |
|-----|-----------|
| RFC 1034 | Query/response model, NXDOMAIN |
| RFC 1035 | Packet format, response building, name encoding |
| RFC 3596 | AAAA record serving |
| RFC 6891 | EDNS0 OPT record in responses |
