## ADDED Requirements

### Requirement: TcpConn connect
`TcpConn::connect(host, port, tls)` returns `Promise<TcpConn>`. Connects via `xTcpConnect` with optional TLS. Resolves to open TcpConn or empty (fd == -1) on error.

#### Scenario: Connect without TLS
- **WHEN** `TcpConn::connect("127.0.0.1", 8080)` is called
- **THEN** the Promise resolves to a TcpConn with `is_open() == true`

#### Scenario: Connect with TLS
- **WHEN** `TcpConn::connect("example.com", 443, &tls_ctx)` is called
- **THEN** the Promise resolves to a TcpConn with TLS handshake completed

#### Scenario: Connect by SocketAddr
- **WHEN** `TcpConn::connect(SocketAddr::from(...))` is called
- **THEN** connects directly without DNS resolution

#### Scenario: Connect failure
- **WHEN** `TcpConn::connect("nonexistent", 9999)` is called
- **THEN** the Promise resolves to a TcpConn with `is_open() == false`

### Requirement: TcpConn recv/send
`recv(buf, len)` and `send(buf, len)` return `Promise<ssize_t>`. Delegate to `io::read`/`io::write` (fast-path syscall + EAGAIN readiness).

#### Scenario: Recv data
- **WHEN** `conn.recv(buf, 1024)` is called and data is available
- **THEN** the Promise resolves to bytes read (>= 0)

#### Scenario: Recv EOF
- **WHEN** peer closes connection
- **THEN** `recv()` resolves to 0

#### Scenario: Send data
- **WHEN** `conn.send("hello", 5)` is called
- **THEN** the Promise resolves to bytes written (>= 0)

### Requirement: TcpConn peer_addr / local_addr
`peer_addr()` and `local_addr()` return `Option<SocketAddr>`.

#### Scenario: Get peer address
- **WHEN** `conn.peer_addr()` is called on an open connection
- **THEN** returns `Some(SocketAddr)` with the peer's address

### Requirement: TcpConn RAII
`~TcpConn()` closes the connection. Move-only.

#### Scenario: Destructor closes
- **WHEN** a TcpConn goes out of scope
- **THEN** `xTcpConnClose` is called, AsyncFd is deregistered

### Requirement: TcpListener bind and accept
`TcpListener::bind(host, port)` creates a listener (sync). `accept()` returns `Promise<TcpConn>`.

#### Scenario: Accept connection
- **WHEN** `listener.accept()` is called and a client connects
- **THEN** the Promise resolves to a TcpConn

#### Scenario: Sequential accept
- **WHEN** `accept()` is called repeatedly in a loop
- **THEN** each call resolves to the next incoming connection
