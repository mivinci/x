# ws.h — WebSocket Server

## Introduction

`ws.h` provides a callback-driven WebSocket interface integrated with the xhttp server. For pure WebSocket services, call `xWsServe()` to create a server in one line. For mixed HTTP + WebSocket endpoints, register a route on an `xHttpMux` and call `xWsUpgrade()` from the route's `on_request` (or `on_done`) callback to perform the RFC 6455 upgrade handshake. The library handles frame codec, ping/pong, fragment reassembly, and close negotiation automatically.

All callbacks are dispatched on the event loop thread — no locks or thread pools required.

## Design Philosophy

1. **Handler-Initiated Upgrade** — WebSocket connections start as regular HTTP requests. The user calls `xWsUpgrade(ctx, ...)` inside a route callback (`on_request` or `on_done`) to perform the upgrade. This keeps routing unified: WebSocket endpoints are just `xHttpMux` routes.

2. **Callback-Driven I/O** — Three optional callbacks (`on_open`, `on_message`, `on_close`) cover the full connection lifecycle. The library handles all framing, masking, and control frames internally.

3. **Automatic Protocol Handling** — Ping/pong is answered automatically. Fragmented messages are reassembled before delivery. Close handshake follows RFC 6455 §5.5.1 with a 5-second timeout for the peer's response.

4. **Connection Hijacking** — On successful upgrade, the HTTP connection's socket and transport layer are transferred to a new `xWsConn` object. The HTTP connection is destroyed; the WebSocket connection takes full ownership of the file descriptor.

5. **Pluggable Crypto Backend** — The handshake requires SHA-1 and Base64 for `Sec-WebSocket-Accept` computation. The crypto backend is selected at compile time: OpenSSL, Mbed TLS, or a built-in implementation.

## Architecture

```mermaid
graph TD
    subgraph "Application"
        APP["User Code"]
        ROUTE["xHttpMux route<br/>(on_request or on_done)"]
        WS_CBS["xWsCallbacks"]
    end

    subgraph "xhttp WebSocket"
        UPGRADE["xWsUpgrade(ctx, ...)"]
        HANDSHAKE["Handshake<br/>(RFC 6455 §4)"]
        CRYPTO["SHA-1 + Base64<br/>(pluggable backend)"]
        WSCONN["xWsConn"]
        PARSER["Frame Parser<br/>(incremental)"]
        ENCODER["Frame Encoder"]
        FRAG["Fragment<br/>Reassembly"]
        CTRL["Control Frames<br/>(Ping/Pong/Close)"]
    end

    subgraph "xhttp Server"
        SERVER["xHttpServer"]
        MUX["xHttpMux"]
        RESOLVER["xHttpMuxResolve"]
    end

    subgraph "xbase"
        LOOP["xEventLoop"]
        SOCK["xSocket"]
        TIMER["Idle Timer"]
    end

    APP -->|"xHttpMuxHandle"| MUX
    MUX --> RESOLVER
    APP -->|"xWsServe (one-liner)"| SERVER
    SERVER --> RESOLVER
    RESOLVER --> ROUTE
    ROUTE -->|"xWsUpgrade(ctx, cbs, arg)"| UPGRADE
    UPGRADE --> HANDSHAKE
    HANDSHAKE --> CRYPTO
    HANDSHAKE -->|"101 Switching Protocols"| WSCONN
    WSCONN --> PARSER
    WSCONN --> ENCODER
    PARSER --> FRAG
    PARSER --> CTRL
    FRAG -->|"on_message"| WS_CBS
    CTRL -->|"auto pong"| ENCODER
    WSCONN --> SOCK
    SOCK --> LOOP
    TIMER --> LOOP

    style WSCONN fill:#4a90d9,color:#fff
    style LOOP fill:#50b86c,color:#fff
    style PARSER fill:#9b59b6,color:#fff
    style HANDSHAKE fill:#f5a623,color:#fff
```

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xWsConn` | Opaque WebSocket connection handle |
| `xWsOpcode` | Message type: `Text` (0x1), `Binary` (0x2) |
| `xWsCallbacks` | Struct of 3 optional callback pointers |
| `xWsConnectConf` | Client-side config (see [ws_client.md](ws_client.md)) |

### Callback Signatures

#### xWsOnOpenFunc

```c
typedef void (*xWsOnOpenFunc)(xWsConn conn, void *arg);
```

Called when the WebSocket connection is established. `conn` is valid until `on_close` returns.

#### xWsOnMessageFunc

```c
typedef void (*xWsOnMessageFunc)(
    xWsConn conn, xWsOpcode opcode,
    const void *payload, size_t len,
    void *arg);
```

Called when a complete message is received. Fragmented messages are reassembled before delivery. `payload` is valid only during the callback.

#### xWsOnCloseFunc

```c
typedef void (*xWsOnCloseFunc)(
    xWsConn conn, uint16_t code,
    const char *reason, size_t len,
    void *arg);
```

Called when the connection is closed (clean or abnormal). After this callback returns, `conn` is invalid.

### xWsCallbacks

```c
XDEF_STRUCT(xWsCallbacks) {
    xWsOnOpenFunc    on_open;    /* optional */
    xWsOnMessageFunc on_message; /* optional */
    xWsOnCloseFunc   on_close;   /* optional */
};
```

### Functions

| Function | Description |
| --- | --- |
| `xWsServe` | One-call WebSocket-only server |
| `xWsUpgrade` | Upgrade HTTP → WebSocket (call from a route callback) |
| `xWsSend` | Send a text or binary message |
| `xWsClose` | Initiate graceful close |

#### xWsServe

```c
xHttpServer xWsServe(
    const char *host,
    uint16_t port,
    const xWsCallbacks *callbacks,
    void *arg);
```

Convenience function that creates an HTTP server with a built-in `xHttpMux`, registers a catch-all `GET /` route that upgrades every incoming request to WebSocket, and starts listening. Internally it builds an `xHttpServerConf` with `xHttpMuxResolve`, creates the server, and calls `xHttpServerListen()`.

Returns the server handle for later cleanup via `xHttpServerDestroy()`, or `NULL` on failure. The mux and route context are freed automatically when the server is destroyed — do not call `xHttpMuxDestroy()` yourself.

**Parameters:**

- `host` — Bind address (e.g. `"0.0.0.0"`), or NULL.
- `port` — Port number to listen on.
- `callbacks` — WebSocket event callbacks (not NULL).
- `arg` — User argument forwarded to all callbacks.

**Returns:** Server handle, or `NULL` on failure.

#### xWsUpgrade

```c
xErrno xWsUpgrade(
    xHttpCtx *ctx,
    const xWsCallbacks *callbacks,
    void *arg);
```

Call from a route's `on_request` or `on_done` callback to upgrade the HTTP connection to WebSocket. On success, the handler **must return immediately** — the HTTP connection has been hijacked and the `xHttpCtx*` is no longer valid.

`xWsUpgrade` validates the request headers (`Upgrade`, `Connection`, `Sec-WebSocket-Key`, `Sec-WebSocket-Version`) and sends the `101 Switching Protocols` response automatically. On failure (missing headers, wrong version, etc.) an appropriate HTTP error response (400/405) is sent and a non-Ok error code is returned — the handler may then return normally.

**Parameters:**

- `ctx` — The request context from the route callback.
- `callbacks` — WebSocket event callbacks (not NULL).
- `arg` — User argument forwarded to all callbacks.

**Returns:** `xErrno_Ok` on success.

#### xWsSend

```c
xErrno xWsSend(
    xWsConn conn, xWsOpcode opcode,
    const void *payload, size_t len);
```

Send a message over the WebSocket connection. The payload is framed and queued for asynchronous transmission.

**Returns:** `xErrno_Ok` on success, `xErrno_InvalidState` if the connection is closing.

#### xWsClose

```c
xErrno xWsClose(xWsConn conn, uint16_t code);
```

Initiate a graceful close. Sends a Close frame with the given status code. The connection remains open until the peer responds or a 5-second timeout expires.

### Close Status Codes

| Code | Constant | Meaning |
| --- | --- | --- |
| 1000 | `XWS_CLOSE_NORMAL` | Normal closure |
| 1001 | `XWS_CLOSE_GOING_AWAY` | Server shutting down |
| 1002 | `XWS_CLOSE_PROTOCOL_ERR` | Protocol error |
| 1003 | `XWS_CLOSE_UNSUPPORTED` | Unsupported data |
| 1005 | `XWS_CLOSE_NO_STATUS` | No status received |
| 1006 | `XWS_CLOSE_ABNORMAL` | Abnormal closure |

## Usage Examples

### Echo server (one-liner with `xWsServe`)

```c
#include <x/base/event.h>
#include <x/http/ws.h>
#include <stdio.h>
#include <string.h>

static void on_open(xWsConn conn, void *arg) {
    (void)arg;
    const char *hi = "Welcome!";
    xWsSend(conn, xWsOpcode_Text, hi, strlen(hi));
}

static void on_message(xWsConn conn, xWsOpcode op, const void *data, size_t len, void *arg) {
    (void)arg;
    xWsSend(conn, op, data, len);
}

static void on_close(xWsConn conn, uint16_t code, const char *reason, size_t len, void *arg) {
    (void)conn; (void)reason; (void)len; (void)arg;
    printf("closed: %u\n", code);
}

static const xWsCallbacks ws_cbs = {
    .on_open    = on_open,
    .on_message = on_message,
    .on_close   = on_close,
};

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    xEventLoopEnter(loop);

    xHttpServer srv = xWsServe("0.0.0.0", 8080, &ws_cbs, NULL);
    if (!srv) return 1;

    printf("ws://localhost:8080/\n");
    xEventLoopRun(loop);

    xHttpServerDestroy(srv);
    xEventLoopDestroy(loop);
    return 0;
}
```

### Echo server (with `xWsUpgrade` + `xHttpMux`)

Use this pattern when you need mixed HTTP + WebSocket endpoints on the same server. Register the WebSocket route on a `xHttpMux` alongside ordinary HTTP routes, and call `xWsUpgrade(ctx, ...)` from the route callback.

```c
#include <x/base/event.h>
#include <x/http/server.h>
#include <x/http/ws.h>
#include <stdio.h>
#include <string.h>

static const xWsCallbacks ws_cbs = {
    .on_open    = on_open,
    .on_message = on_message,
    .on_close   = on_close,
};

/* Route callback — invoked by xHttpMuxResolve after the request is complete.
 * Type is xHttpDoneFunc (or xHttpInitFunc, depending on which field you
 * register it in). Receives xHttpCtx*. */
static void ws_handler(xHttpCtx *ctx, void *arg) {
    (void)arg;
    xErrno err = xWsUpgrade(ctx, &ws_cbs, NULL);
    if (err != xErrno_Ok) {
        /* xWsUpgrade already sent a 400/405; just return. */
        return;
    }
    /* On success the connection has been hijacked — do not touch ctx. */
}

int main(void) {
    xEventLoop loop = xEventLoopCreate();
    xEventLoopEnter(loop);

    xHttpMux mux = xHttpMuxCreate();

    xHttpRouteConf ws_route = {
        .pattern = "GET /ws",
        .on_done = ws_handler,              /* or on_request */
    };
    xHttpMuxHandle(mux, &ws_route);

    /* You can register plain HTTP routes on the same mux. */
    xHttpRouteConf health_route = {
        .pattern    = "GET /health",
        .on_request = on_health,            /* a normal xHttpInitFunc handler */
    };
    xHttpMuxHandle(mux, &health_route);

    xHttpServerConf sconf = {0};
    sconf.resolve = xHttpMuxResolve;
    sconf.router  = mux;

    xHttpServer srv = xHttpServerCreate(&sconf);
    xHttpServerListen(srv, "0.0.0.0", 8080);

    printf("ws://localhost:8080/ws\n");
    xEventLoopRun(loop);

    xHttpServerDestroy(srv);
    xHttpMuxDestroy(mux);
    xEventLoopDestroy(loop);
    return 0;
}
```

> Register the upgrade handler in either `on_request` (fires right after headers) or `on_done` (fires after the full request body is received). For a GET-based WebSocket Upgrade there is no body, so both fire essentially back-to-back — pick whichever matches your preference. The one-liner `xWsServe()` uses `on_done` internally.

### Per-connection user data

Allocate a per-connection state in the route handler and pass it as `arg` to `xWsUpgrade`. Free it in `on_close`.

```c
typedef struct {
    char username[64];
    int  msg_count;
} Session;

static void on_open(xWsConn conn, void *arg) {
    Session *s = arg;
    snprintf(s->username, sizeof(s->username), "user_%p", (void *)conn);
    s->msg_count = 0;
}

static void on_message(xWsConn conn, xWsOpcode op, const void *data, size_t len, void *arg) {
    Session *s = arg;
    s->msg_count++;
    printf("[%s] msg #%d: %.*s\n", s->username, s->msg_count, (int)len, (const char *)data);
    xWsSend(conn, op, data, len);
}

static void on_close(xWsConn conn, uint16_t code, const char *reason, size_t len, void *arg) {
    (void)conn; (void)code; (void)reason; (void)len;
    Session *s = arg;
    printf("[%s] disconnected\n", s->username);
    free(s);
}

static void ws_handler(xHttpCtx *ctx, void *arg) {
    (void)arg;
    Session *s = calloc(1, sizeof(Session));
    if (!s) return;
    xWsCallbacks cbs = {
        .on_open    = on_open,
        .on_message = on_message,
        .on_close   = on_close,
    };
    xWsUpgrade(ctx, &cbs, s);
}
```

### Graceful server-initiated close

```c
static void on_message(xWsConn conn, xWsOpcode op, const void *data, size_t len, void *arg) {
    (void)op; (void)arg;
    if (len == 4 && memcmp(data, "quit", 4) == 0) {
        xWsClose(conn, 1000);               /* normal close */
        return;
    }
    xWsSend(conn, op, data, len);
}
```

### JavaScript client

```html
<script>
const ws = new WebSocket('ws://localhost:8080/ws');

ws.onopen = () => console.log('connected');

ws.onmessage = (e) => console.log('< ' + e.data);

ws.onclose = (e) =>
    console.log('closed: ' + e.code);

// Send a message
ws.send('Hello, server!');
</script>
```

## Best Practices

- **Return immediately after `xWsUpgrade()` succeeds.** On success the HTTP connection is hijacked — the `xHttpCtx*` is no longer valid and you must not call any `xHttpCtx*` functions afterward.
- **Don't block in callbacks.** All callbacks run on the event loop thread. Blocking delays all other I/O.
- **Copy payload if needed.** The `payload` pointer in `on_message` is valid only during the callback. Copy the data if you need it later.
- **Use `xWsClose()` for graceful shutdown.** Avoid dropping connections without a Close handshake.
- **Handle `on_close` for cleanup.** Free per-connection resources in `on_close`, as the `xWsConn` handle becomes invalid after the callback returns.
- **Idle timeout is set on the server.** The WebSocket connection inherits the `xHttpServerConf.idle_timeout_ms` setting. Adjust it when creating the server if you need longer-lived connections.

## Comparison with Other Libraries

| Feature | xhttp WS | libwebsockets | uWebSockets |
| --- | --- | --- | --- |
| Integration | xEventLoop | Own loop | Own loop |
| Upgrade | In HTTP route callback | Separate | Separate |
| Fragment reassembly | Automatic | Automatic | Automatic |
| Ping/Pong | Automatic | Automatic | Automatic |
| Close handshake | RFC 6455 | RFC 6455 | RFC 6455 |
| TLS | Via xhttp | Built-in | Built-in |
| Language | C99 | C | C++ |
| Dependencies | xbase only | OpenSSL | None |

**Key Differentiator:** xhttp's WebSocket server is unique in its handler-initiated upgrade pattern. Instead of a separate WebSocket server, you register a normal `xHttpMux` route and call `xWsUpgrade(ctx, ...)` inside the route callback. This keeps routing, middleware, and mixed HTTP+WS endpoints unified under a single `xHttpServer` instance and a single `xHttpMux`.

## Implementation Details

### Upgrade Handshake Flow

```mermaid
sequenceDiagram
    participant Client as Browser
    participant Mux as xHttpMux
    participant Handler as Route Callback
    participant Upgrade as xWsUpgrade()
    participant Conn as xHttpConn_
    participant WS as xWsConn

    Client->>Mux: GET /ws (Upgrade: websocket)
    Mux->>Handler: on_request(ctx, arg) or on_done(ctx, arg)
    Handler->>Upgrade: xWsUpgrade(ctx, &cbs, arg)
    Upgrade->>Upgrade: Validate headers
    Note over Upgrade: Method=GET<br/>Upgrade: websocket<br/>Connection: Upgrade<br/>Sec-WebSocket-Version: 13<br/>Sec-WebSocket-Key: ...
    Upgrade->>Upgrade: SHA1(Key + GUID) → Base64
    Upgrade->>Client: 101 Switching Protocols
    Upgrade->>Conn: Hijack socket + transport
    Upgrade->>WS: xWsConnCreate()
    WS->>Client: on_open callback fires
```

### Connection Lifecycle

```mermaid
stateDiagram-v2
    [*] --> Open: xWsUpgrade() succeeds
    Open --> Open: Data frames (text/binary)
    Open --> Open: Ping → auto Pong
    Open --> CloseSent: xWsClose() called
    Open --> CloseReceived: Peer sends Close
    CloseSent --> Closed: Peer Close received
    CloseSent --> Closed: 5s timeout
    CloseReceived --> Closed: Echo Close flushed
    Open --> Closed: I/O error
    Open --> CloseSent: Idle timeout (1001)
    Closed --> [*]: on_close + destroy
```

### Frame Processing

When data arrives on the socket, the incremental frame parser (`xWsFrameParser`) extracts complete frames from the `xIOBuffer`. Each frame is processed based on its opcode:

| Opcode | Handling |
| --- | --- |
| Text (0x1) | Deliver via `on_message` |
| Binary (0x2) | Deliver via `on_message` |
| Continuation (0x0) | Append to fragment buffer |
| Ping (0x9) | Auto-reply with Pong |
| Pong (0xA) | Ignored |
| Close (0x8) | Close handshake |

### Fragment Reassembly

Fragmented messages are reassembled transparently:

1. First fragment (FIN=0, opcode=Text/Binary) starts accumulation in `frag_buf`.
2. Continuation frames (opcode=0x0) append to `frag_buf`.
3. Final fragment (FIN=1, opcode=0x0) triggers reassembly and delivers the complete message via `on_message`.

Protocol violations (e.g., new message mid-fragment) result in a Close frame with status 1002.

### Close State Machine

```c
XDEF_ENUM(xWsCloseState){
    xWsCloseState_Open,          // Normal operating state
    xWsCloseState_CloseSent,     // We sent Close, waiting for peer
    xWsCloseState_CloseReceived, // Peer sent Close, we replied
    xWsCloseState_Closed,        // Connection fully closed
};
```

- **Server-initiated close:** `xWsClose()` sends a Close frame and transitions to `CLOSE_SENT`. A 5-second timer waits for the peer's Close response.
- **Peer-initiated close:** The peer's Close frame is echoed back, transitioning to `CLOSE_RECEIVED`. After the echo is flushed, `on_close` fires and the connection is destroyed.
- **Idle timeout:** After the configured idle period with no data, a Close frame with code 1001 (Going Away) is sent.

### Internal File Structure

| File | Role |
| --- | --- |
| `ws.h` | Public API (types, callbacks, functions) |
| `ws.c` | Connection lifecycle, I/O, frame dispatch |
| `ws_handshake_server.c` | Server upgrade handshake (RFC 6455 §4.2) |
| `ws_frame.h/c` | Frame codec (parse + encode) |
| `ws_crypto.h` | SHA-1 + Base64 interface |
| `ws_crypto_openssl.c` | OpenSSL backend |
| `ws_crypto_mbedtls.c` | Mbed TLS backend |
| `ws_crypto_builtin.c` | Built-in (no TLS dep) |
| `ws_serve.c` | `xWsServe()` convenience wrapper |
| `ws_private.h` | Internal data structures |
