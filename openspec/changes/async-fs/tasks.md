## 1. Module scaffold

- [ ] 1.1 Create `libx/x/fs/` directory
- [ ] 1.2 Create `fs.h` with public types and API
- [ ] 1.3 Create `CMakeLists.txt` linking xbase
- [ ] 1.4 Wire into parent `libx/CMakeLists.txt`

## 2. Implementation

- [ ] 2.1 Implement thread-pool worker for each op (open/close/read/write/stat/mkdir/unlink/rename)
- [ ] 2.2 Implement `xFsReqSubmit` — dispatch to worker, handle async/sync modes
- [ ] 2.3 Implement `xFsReqCancel` — delegate to `xWorkCancel`
- [ ] 2.4 Implement streaming read (multi-call with done flag)

## 3. Tests

- [ ] 3.1 Test open/close
- [ ] 3.2 Test read (whole file, chunked)
- [ ] 3.3 Test write + read back
- [ ] 3.4 Test stat
- [ ] 3.5 Test mkdir/unlink
- [ ] 3.6 Test rename
- [ ] 3.7 Test cancel
- [ ] 3.8 Test sync mode (cb=NULL)

## 4. Build and verify

- [ ] 4.1 Build with cmake, fix compilation errors
- [ ] 4.2 Run xfs_test, verify all tests pass
- [ ] 4.3 Run full test suite, verify no regressions
