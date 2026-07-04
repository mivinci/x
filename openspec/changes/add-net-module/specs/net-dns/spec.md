## ADDED Requirements

### Requirement: DNS resolve
`resolve(hostname)` returns `Promise<std::vector<SocketAddr>>`. Uses `xDnsResolve` via adapt pattern.

#### Scenario: Resolve hostname
- **WHEN** `resolve("localhost")` is called
- **THEN** the Promise resolves to a vector containing at least one SocketAddr

#### Scenario: Resolve invalid hostname
- **WHEN** `resolve("nonexistent.invalid")` is called
- **THEN** the Promise resolves to an empty vector

### Requirement: DNS cancel on Promise destroy
If the Promise is destroyed before resolution completes, `xDnsCancel` is called.

#### Scenario: Promise destroyed before resolve
- **WHEN** the Promise from `resolve()` is destroyed before the callback fires
- **THEN** `xDnsCancel` is called, no use-after-free (PromiseResolver ArcWeak safety)
