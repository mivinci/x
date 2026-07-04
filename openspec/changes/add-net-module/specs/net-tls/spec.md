## ADDED Requirements

### Requirement: TlsConfig
`TlsConfig::client()` and `TlsConfig::server(cert, key)` create TLS configurations. Wraps `xTlsConf`.

#### Scenario: Client config
- **WHEN** `TlsConfig::client()` is called
- **THEN** returns a TlsConfig suitable for client-side TLS

#### Scenario: Server config
- **WHEN** `TlsConfig::server("cert.pem", "key.pem")` is called
- **THEN** returns a TlsConfig with certificate and key loaded

### Requirement: TlsContext
`TlsContext(conf)` creates a TLS context from config. RAII: destructor calls `xTlsCtxDestroy`.

#### Scenario: Create and destroy
- **WHEN** `TlsContext ctx(TlsConfig::client())` is constructed
- **THEN** `xTlsCtxCreate` is called

#### Scenario: Destructor
- **WHEN** a TlsContext goes out of scope
- **THEN** `xTlsCtxDestroy` is called

### Requirement: TLS in TcpConn::connect
Passing `TlsContext*` to `TcpConn::connect()` enables TLS. Handshake is transparent (handled by libx's `xTcpConnect`).

#### Scenario: Connect with TLS
- **WHEN** `TcpConn::connect("example.com", 443, &tls_ctx)` is called
- **THEN** the connection has TLS enabled, `recv()`/`send()` transparently encrypt/decrypt
