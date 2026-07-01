## 1. Move directories

- [x] 1.1 `git mv x/ libx/x/` and `git mv bench/ libx/bench/`
- [x] 1.2 `git mv docs/x/ docs/libx/`

## 2. Update root CMakeLists.txt

- [x] 2.1 `add_subdirectory(x/...)` → `add_subdirectory(libx/x/...)` (all 8 calls)
- [x] 2.2 `add_subdirectory(bench)` → `add_subdirectory(libx/bench)`

## 3. Update CI and scripts

- [x] 3.1 `.github/workflows/ci.yml`: cache key `x/*/CMakeLists.txt` → `libx/x/*/CMakeLists.txt`
- [x] 3.2 `.github/workflows/ci.yml`: path trigger filters
- [x] 3.3 `bench/run_bench.sh`: `$BUILD_DIR/bench/` → `$BUILD_DIR/libx/bench/` (4 lines)

## 4. Update documentation

- [x] 4.1 `docs/SUMMARY.md`: `x/` → `libx/` (all paths)
- [x] 4.2 `docs/libx/base/event.md`: `../bench/event_loop.md` → path fix
- [x] 4.3 `docs/book.toml`: `edit-url-template` path if needed

## 5. Verification

- [x] 5.1 `cmake -B build && cmake --build build` — full build succeeds
- [x] 5.2 `cd build && ctest` — all tests pass
- [x] 5.3 `cd docs && mdbook build` — docs build succeeds
