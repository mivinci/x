# dns-benchmark

## ADDED Requirements

### Requirement: Benchmark output is JSON

The benchmark SHALL output results in JSON format with resolver name, mode (local/remote), and a list of scenario results containing scenario name, query count, and latency in microseconds.

#### Scenario: Local mode output
- **WHEN** the benchmark is run in local mode against a local DNS server
- **THEN** JSON output is written to stdout with valid structure

### Requirement: Three resolvers are tested

The benchmark SHALL test xdns, xnet/dns (getaddrinfo), and Go's pure-Go resolver in separate runs.

#### Scenario: All resolvers complete
- **WHEN** the benchmark suite is run
- **THEN** results are produced for all three resolver implementations
