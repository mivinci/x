## Context

Current layout:
```
.
├── x/           ← library source
├── bench/       ← benchmarks
├── cmake/       ← CMake modules
├── docs/        ← documentation
├── scripts/     ← CI test scripts
├── .github/     ← CI workflows
├── CMakeLists.txt
├── README.md
└── ...
```

Target layout (curl-style):
```
.
├── libx/         ← library source + benchmarks
│   ├── x/        ← library modules (xbase, xlog, ...)
│   │   ├── CMakeLists.txt (aggregated INTERFACE target)
│   │   ├── base/
│   │   ├── log/
│   │   ├── ...
│   └── bench/    ← benchmarks
├── libxpp/       ← future C++ wrapper
├── cmake/        ← CMake modules
├── docs/         ← documentation
│   ├── libx/     ← libx docs (matches directory name)
│   └── libxpp/   ← future xpp docs
├── scripts/      ← CI test scripts
├── .github/      ← CI workflows
├── CMakeLists.txt
└── ...
```

## Goals / Non-Goals

**Goals:**
- Group library source and its benchmarks under `libx/`
- Leave room for `libxpp/`, future tool directories at root level
- Keep `#include <x/...>` paths unchanged
- All existing tests, CI, and docs continue to work

**Non-Goals:**
- Changing any API, header structure, or build logic
- Creating `libxpp/` directory (future work)
- Renaming library modules or targets

## Decisions

### 1. Move, don't rename

`x/` → `libx/x/`, `bench/` → `libx/bench/`. This preserves the internal `x/` namespace and all `#include <x/base/event.h>` paths. The root `CMakeLists.txt` already uses `include_directories("${CMAKE_CURRENT_SOURCE_DIR}")` — the include paths resolve from the repo root, so the extra `libx/` layer is transparent to source files.

### 2. `git mv` for all moves

Use `git mv` to preserve history. CMake and script files referencing `x/` paths need string updates (not file moves).

### 3. Files to update

| File | Change |
|------|--------|
| `CMakeLists.txt` | `add_subdirectory(x/...)` → `add_subdirectory(libx/x/...)` |
| `CMakeLists.txt` | `add_subdirectory(bench)` → `add_subdirectory(libx/bench)` |
| `libx/x/CMakeLists.txt` | `include_directories` path adjustment |
| `.github/workflows/ci.yml` | Path filters from `x/` → `libx/x/` |
| `scripts/test-linux.sh` | Build output path |
| `scripts/test-mac.sh` | Build output path |
| `CODEBUDDY.md` | All `x/` path references |
| `docs/` | Cross-reference paths in markdown files |

## Risks / Trade-offs

- **One extra directory level** — `libx/x/base/event.h` vs `x/base/event.h` on disk. Acceptable trade-off for cleaner root.
- **Git history** — `git mv` preserves blame, but `git log --follow` is needed for moved files.
