## Why

xdns is libx's new protocol-native, truly async DNS resolver. We need quantitative data to validate it against two alternatives: the old thread-pool `getaddrinfo` approach (`xnet/dns`), and Go's pure-Go DNS resolver. Benchmarks provide confidence in the architecture and reveal performance characteristics under load.

## What Changes

- Add `libx/bench/dns/` with local DNS benchmark server and libx resolver benchmark client
- Add Go benchmark client for cross-language comparison
- Measure single-query latency, batch throughput, and cache hit latency
- Test against both local (zero-network) and remote (8.8.8.8) DNS servers
- Output results in JSON for easy comparison
- Add `dns` subcommand to `run_bench.sh`

## Capabilities

### New Capabilities

<!-- None — this is purely a benchmark addition, no production API changes -->

## Impact

- `libx/bench/dns/` — new directory
- `libx/bench/CMakeLists.txt` — add DNS benchmark targets
- `libx/bench/run_bench.sh` — add `dns` subcommand
- No changes to library code
