## ADDED Requirements

### Requirement: UdpSocket bind
`UdpSocket::bind(host, port)` creates a non-blocking UDP socket, binds, and registers with event loop via AsyncFd. Sync, returns UdpSocket.

#### Scenario: Bind to address
- **WHEN** `UdpSocket::bind("0.0.0.0", 9090)` is called
- **THEN** returns a UdpSocket with `is_open() == true`

### Requirement: UdpSocket recv_from
`recv_from(buf, len)` returns `Promise<std::pair<ssize_t, SocketAddr>>` — bytes read + peer address. Fast-path: try `::recvfrom()`, EAGAIN → wait readable → retry.

#### Scenario: Receive datagram
- **WHEN** `sock.recv_from(buf, 1024)` is called and a datagram arrives
- **THEN** the Promise resolves to `{bytes, peer_addr}`

#### Scenario: Receive with no data
- **WHEN** `sock.recv_from(buf, 1024)` is called and no datagram is available
- **THEN** the Promise is pending until a datagram arrives

### Requirement: UdpSocket send_to
`send_to(buf, len, target)` returns `Promise<ssize_t>`. Fast-path: try `::sendto()`, EAGAIN → wait writable → retry.

#### Scenario: Send datagram
- **WHEN** `sock.send_to("hello", 5, target_addr)` is called
- **THEN** the Promise resolves to bytes sent (>= 0)

### Requirement: UdpSocket RAII
`~UdpSocket()` closes the fd. Move-only.

#### Scenario: Destructor closes
- **WHEN** a UdpSocket goes out of scope
- **THEN** the fd is closed and AsyncFd is deregistered
