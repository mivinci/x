## 1. Add xErrno_InProgress

- [ ] 1.1 Add `xErrno_InProgress` to error codes in `error.h`
- [ ] 1.2 Add human-readable string to `error.c`

## 2. Update xTaskCancel

- [ ] 2.1 Replace `xErrno_InvalidState` with `xErrno_InProgress` in `task.c` when task is running

## 3. Add cancelled flag to xWork_

- [ ] 3.1 Add `int cancelled` field to `struct xWork_` in `event_private.h`

## 4. Update xWorkCancel

- [ ] 4.1 Set `w->cancelled = 1` before calling `xTaskCancel`
- [ ] 4.2 Change return value to `xErrno_Ok` on success (remove `xErrno_Busy` path)
- [ ] 4.3 Keep `xErrno_InvalidArg` for NULL work

## 5. Update loop_run_done

- [ ] 5.1 Add `cancelled` check before calling `done_fn` in `event_run.c`

## 6. Update docs and callers

- [ ] 6.1 Update `event.h` doc for `xWorkCancel`
- [ ] 6.2 Search for callers that check `xErrno_Busy` from `xWorkCancel` and update

## 7. Build and verify

- [ ] 7.1 Build with cmake, fix compilation errors
- [ ] 7.2 Run xbase_test, verify all tests pass
- [ ] 7.3 Run full test suite, verify no regressions
