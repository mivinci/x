## ADDED Requirements

### Requirement: xSocketSendTo — UDP datagram send

`xSocketSendTo` SHALL send a UDP datagram to a specified destination address. It operates on a non-blocking socket and returns the number of bytes sent, or -1 on error (errno set).

#### Scenario: Send UDP datagram

- **WHEN** a datagram is sent via `xSocketSendTo(sock, buf, len, dest, destlen)`
- **THEN** the bytes are sent to the destination address
- **AND** the return value is the number of bytes sent, or -1 on error

### Requirement: xSocketRecvFrom — UDP datagram receive

`xSocketRecvFrom` SHALL receive a UDP datagram, storing the source address. It operates on a non-blocking socket and returns the number of bytes received, or -1 on error (errno set to EAGAIN if no data available).

#### Scenario: Receive UDP datagram

- **WHEN** data is available and `xSocketRecvFrom(sock, buf, len, src, srclen)` is called
- **THEN** the datagram is stored in `buf` and the source address in `src`
- **AND** the return value is the number of bytes received

#### Scenario: No data available

- **WHEN** no data is available and `xSocketRecvFrom` is called
- **THEN** the return value is -1 and errno is EAGAIN/EWOULDBLOCK
