# dns.h — DNS Server API

## Introduction

`xDnsServer` provides a DNS server that listens on a UDP port and responds to queries. It supports two modes that can coexist:

- **Authoritative** — Serve records from local zones
- **Forwarding** — Forward unresolved queries to an upstream `xDnsClient`

An optional **filter callback** can intercept, block, or rewrite queries before processing.

## Types

### xDnsServerConf — Configuration

```c
XDEF_STRUCT(xDnsServerConf) {
  xDnsClient     forwarder;      // Upstream resolver (NULL = authoritative only)
  xDnsFilterFunc filter;         // Query filter (NULL = no filter)
  void          *filter_arg;     // Argument forwarded to filter
  int            cache_enabled;  // Share cache with forwarder (default 1)
};
```

Zero-initialize for defaults.

### xDnsFilterFunc — Query filter

```c
typedef int (*xDnsFilterFunc)(const char *name, uint16_t type, void *arg);
// Return 0 = allow, non-zero = block (respond with NXDOMAIN)
```

## API

### Lifecycle

| Function | Description |
| --- | --- |
| `xDnsServerCreate(conf)` | Create a server bound to the current event loop. `conf` may be NULL. |
| `xDnsServerDestroy(server)` | Destroy server and release all resources. Zones are NOT freed. Safe with NULL. |
| `xDnsServerListen(server, host, port)` | Start listening for queries on a UDP port. |
| `xDnsServerPort(server)` | Return the actual bound port (useful when port was 0). |
| `xDnsServerAddZone(server, zone)` | Attach a zone to the server. Zones are checked in registration order; first match wins. |

### Zone API

| Function | Description |
| --- | --- |
| `xDnsZoneCreate()` | Create an empty zone. |
| `xDnsZoneDestroy(zone)` | Destroy a zone and free all records. Safe with NULL. |
| `xDnsZoneAdd(zone, name, type, rdata, rdlen, ttl)` | Add a record. `name` and `rdata` are copied. `type` must have exactly one bit set. |

## Usage Examples

### Authoritative server

```c
// Create a zone with local records
xDnsZone zone = xDnsZoneCreate();

uint8_t ip[4] = {192, 168, 1, 100};
xDnsZoneAdd(zone, "myapp.local", xDnsType_A, ip, 4, 3600);

xDnsServerConf conf = {};
xDnsServer server = xDnsServerCreate(&conf);
xDnsServerAddZone(server, zone);
xDnsServerListen(server, "0.0.0.0", 5353);

// "myapp.local" → 192.168.1.100
// Anything else → NXDOMAIN
```

### Forwarding server (local DNS relay)

```c
xDnsClientConf cconf = {};
cconf.nameservers[0] = "8.8.8.8";
xDnsClient upstream = xDnsClientCreate(&cconf);

xDnsServerConf sconf = {};
sconf.forwarder = upstream;
sconf.cache_enabled = 1;

xDnsServer server = xDnsServerCreate(&sconf);
xDnsServerListen(server, "0.0.0.0", 53);
// All queries → forwarded to 8.8.8.8 → cached → returned
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
    if (strstr(name, "ads.") || strstr(name, "tracker.")) return 1;
    return 0;
}

xDnsClient upstream = xDnsClientCreate(&(xDnsClientConf){
    .nameservers = {"8.8.8.8"} });

xDnsServerConf conf = {};
conf.forwarder = upstream;
conf.filter = ad_filter;

xDnsServer server = xDnsServerCreate(&conf);
xDnsServerListen(server, "0.0.0.0", 53);

// "ads.example.com" → NXDOMAIN (blocked)
// "example.com" → forwarded to 8.8.8.8
```

### Zone with multiple A records (round-robin)

```c
uint8_t ip1[4] = {10, 0, 0, 1};
uint8_t ip2[4] = {10, 0, 0, 2};
xDnsZoneAdd(zone, "api.local", xDnsType_A, ip1, 4, 300);
xDnsZoneAdd(zone, "api.local", xDnsType_A, ip2, 4, 300);
// Query for "api.local" → both IPs in answer section
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
    ├─ Check local zones (registration order, first match wins)
    │   └─ Hit? → build response with zone records → send
    │
    ├─ Forwarder configured?
    │   ├─ Yes → xDnsClientDo(forwarder, ...)
    │   │        → upstream resolves → build response → send
    │   └─ No → send NXDOMAIN
    │
    └─ Response sent to client via sendto
```

## Best Practices

- **Create the forwarder before the server** — The `xDnsClient` passed via `xDnsServerConf.forwarder` must outlive the server.
- **Free zones separately** — `xDnsServerDestroy()` does not free zones. Call `xDnsZoneDestroy()` manually if needed.
- **Filter returns 0 to allow** — A non-zero return drops the query with NXDOMAIN.
- **Zone matching is case-insensitive** — `"MyApp.local"` and `"myapp.local"` are treated the same.
