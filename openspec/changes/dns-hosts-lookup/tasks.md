## 1. API and config

- [ ] 1.1 Add `enable_hosts` to `xDnsClientConf` in `dns.h`
- [ ] 1.2 Add `xDnsClientReloadHosts()` declaration to `dns.h`

## 2. Hosts parser

- [ ] 2.1 Implement hosts file parser (parse `IP hostname [alias...]` lines)
- [ ] 2.2 Store entries in `xMap` keyed by lowercase hostname, value = `xDnsRecord*`
- [ ] 2.3 Handle comments (`#`) and blank lines
- [ ] 2.4 Support multiple IPs per hostname (linked list)

## 3. Integration

- [ ] 3.1 Add `xMap hosts` field to `struct xDnsClient_`
- [ ] 3.2 Load hosts in `xDnsClientCreate` when `enable_hosts != 0`
- [ ] 3.3 Check hosts before DNS in `xDnsClientDo`
- [ ] 3.4 Clean up hosts table in `xDnsClientDestroy`
- [ ] 3.5 Implement `xDnsClientReloadHosts()`

## 4. Tests

- [ ] 4.1 Test hosts lookup hit (known hostname → immediate callback)
- [ ] 4.2 Test hosts lookup miss (unknown → DNS fallback)
- [ ] 4.3 Test hosts disabled (enable_hosts=0)
- [ ] 4.4 Test reload works after file change

## 5. Build and verify

- [ ] 5.1 Build with cmake, fix compilation errors
- [ ] 5.2 Run xdns_test, verify all tests pass
- [ ] 5.3 Run full test suite, verify no regressions
