## 1. Infrastructure fixes

- [ ] 1.1 CMake: rename `xpp_core` → `xpp` (target name, all references)
- [ ] 1.2 CMake: `cxx_std_17` → `cxx_std_11`
- [ ] 1.3 CMake: ensure `xnet` is linked (already done in PR #30, verify)
- [ ] 1.4 `addr.h`: `inline const` static members → `static constexpr` (C++11-compatible)
- [ ] 1.5 Header comments: "C++17-compatible" → "C++11-compatible" where appropriate

## 2. TLS wrappers (net/tls.h)

- [ ] 2.1 `TlsConfig` — wraps `xTlsConf`, static `client()` / `server(cert, key)` factories
- [ ] 2.2 `TlsContext` — RAII wrapper around `xTlsCtx`, constructor calls `xTlsCtxCreate`, destructor calls `xTlsCtxDestroy`

## 3. TcpConn (net/tcp.h)

- [ ] 3.1 Define `TcpConn` class: `xTcpConn m_conn`, `int m_fd`, `io::AsyncFd m_async`
- [ ] 3.2 Constructor from `xTcpConn`: extract fd via `xTcpConnSocket` + `xSocketFd`, set non-blocking, create AsyncFd
- [ ] 3.3 `static Promise<TcpConn> connect(SocketAddr addr, const TlsContext* tls = nullptr)`
- [ ] 3.4 `static Promise<TcpConn> connect(const char* host, uint16_t port, const TlsContext* tls = nullptr)`
- [ ] 3.5 `TcpConnectAdapter` — stores PromiseResolver, calls `xTcpConnect`, callback resolves, self-deletes
- [ ] 3.6 `recv(void* buf, size_t len)` → `io::read(m_async, buf, len)`
- [ ] 3.7 `send(const void* buf, size_t len)` → `io::write(m_async, buf, len)`
- [ ] 3.8 `peer_addr()` / `local_addr()` via getpeername/getsockname
- [ ] 3.9 `close()` — destruct AsyncFd, `xTcpConnClose`
- [ ] 3.10 Move semantics, RAII destructor

## 4. TcpListener (net/tcp.h)

- [ ] 4.1 Define `TcpListener` class: `xTcpListener m_listener`, `PromiseResolver<TcpConn> m_pending`
- [ ] 4.2 `static TcpListener bind(SocketAddr addr)` / `bind(const char* host, uint16_t port)` — sync, calls `xTcpListenerCreate`
- [ ] 4.3 `on_accept` static callback — resolves `m_pending` with TcpConn, or closes conn if no one waiting
- [ ] 4.4 `accept()` → `adapt<TcpConn, TcpAcceptAdapter>(this)` — stores resolver in m_pending
- [ ] 4.5 `close()` — `xTcpListenerDestroy`
- [ ] 4.6 Move semantics, RAII destructor

## 5. UdpSocket (net/udp.h)

- [ ] 5.1 Define `UdpSocket` class: `int m_fd`, `io::AsyncFd m_async`
- [ ] 5.2 `static UdpSocket bind(SocketAddr addr)` / `bind(const char* host, uint16_t port)` — create `xSocketCreate(SOCK_DGRAM)`, bind, AsyncFd
- [ ] 5.3 `recv_from(void* buf, size_t len)` → try `::recvfrom`, EAGAIN → `readable().then(recvfrom)`, returns `Promise<pair<ssize_t, SocketAddr>>`
- [ ] 5.4 `send_to(const void* buf, size_t len, SocketAddr target)` → try `::sendto`, EAGAIN → `writable().then(sendto)`, returns `Promise<ssize_t>`
- [ ] 5.5 `close()`, move semantics, RAII destructor

## 6. DNS (net/dns.h)

- [ ] 6.1 `resolve(const char* hostname)` → `Promise<vector<SocketAddr>>` via adapt
- [ ] 6.2 `DnsResolveAdapter` — calls `xDnsResolve`, callback converts `xDnsResult` → `vector<SocketAddr>`, self-deletes
- [ ] 6.3 Cancel on destroy: `xDnsCancel` in adapter destructor

## 7. URL (net/url.h)

- [ ] 7.1 `Url` class — RAII wrapper around `xUrl`, destructor calls `xUrlFree`
- [ ] 7.2 `static Result<Url> parse(const char* raw)` — calls `xUrlParse`
- [ ] 7.3 Accessors: `scheme()`, `host()`, `port()`, `path()` — return `std::string` / `uint16_t`
- [ ] 7.4 Move semantics, delete copy

## 8. Tests

- [ ] 8.1 TcpConn: connect to local server, send/recv, verify data
- [ ] 8.2 TcpConn: connect failure (nonexistent host)
- [ ] 8.3 TcpConn: peer_addr / local_addr
- [ ] 8.4 TcpListener: bind + accept, verify incoming connection
- [ ] 8.5 TcpListener: sequential accept (two clients)
- [ ] 8.6 UdpSocket: bind + recv_from + send_to (loopback)
- [ ] 8.7 UdpSocket: recv_from returns correct peer address
- [ ] 8.8 DNS: resolve "localhost" → at least one address
- [ ] 8.9 DNS: resolve invalid hostname → empty vector
- [ ] 8.10 URL: parse valid URL, verify all components
- [ ] 8.11 URL: parse invalid URL → Err
- [ ] 8.12 TLS: create TlsContext, verify RAII

## 9. Docs

- [ ] 9.1 Create `docs/libxpp/net.md` — TCP/UDP/DNS/URL/TLS API, examples, comparison with tokio::net
- [ ] 9.2 Update `docs/SUMMARY.md` — add net page
- [ ] 9.3 Update `docs/libxpp/README.md` — add net to module list
