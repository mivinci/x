## Context

The project has 7 library modules with a rich public API. The README gives a high-level overview but doesn't document individual functions, types, or usage patterns. mdBook is a simple static site generator from Markdown, already used by the sibling moo project.

## Goals / Non-Goals

**Goals:**
- Structured docs covering all 7 modules with API reference and examples
- Sidebar navigation via `SUMMARY.md`
- CI deployment to GitHub Pages on push to main
- Docs organized under `docs/x/` for libx, with `docs/xpp/` reserved for a future C++ wrapper

**Non-Goals:**
- Auto-generated API docs from C headers
- Diagrams (mdbook-mermaid can be added later)

## Decisions

### 1. Copy from moo, then adapt

The moo project at `../moo/docs/libx/` has high-quality documentation for all shared modules. Since the API hasn't changed significantly (only `event.h` and `timer.h` have diverged), the approach is:

1. Copy `moo/docs/libx/` → `docs/x/`
2. Remove modules not in libx (`js/`, `fer/`)
3. Copy theme assets (`theme/`, `logo.png`) for consistent styling
4. Create `book.toml` and `SUMMARY.md` from moo's equivalents
5. Adjust any broken cross-reference paths

### 2. Document structure per module

Each sub-module page follows the structure used by the moo docs (e.g., `client.h`):

```
# Module Title — One-line description
## Introduction           ← What this provides, design philosophy
## Architecture           ← Component diagram or relationship overview
## API Reference          ← Types table, lifecycle functions, API functions with signatures
## Usage Examples         ← Complete, compilable code snippets
## Use Cases              ← Real-world scenarios
## Best Practices         ← Gotchas, thread safety notes, lifecycle rules
## Comparison             ← How this differs from alternatives
## Implementation Details ← Key internal design decisions (optional, for complex modules)
```

### 2. Directory layout

```
docs/
├── book.toml              ← mdBook config (src = ".")
├── SUMMARY.md             ← TOC, references chapters via relative paths
│
├── x/                     ← libx docs (this change)
│   ├── getting-started.md
│   ├── base/
│   │   ├── event.md       ← event loop (kqueue/epoll/poll)
│   │   ├── ds.md          ← array, list, map, heap, bitmap, mpsc
│   │   ├── memory.md      ← memory.h, slab.h
│   │   ├── encoding.md    ← hex, base64, base58
│   │   ├── io.md          ← xReader, xWriter
│   │   ├── error-time.md  ← xErrno, monotonic clock
│   │   ├── thread.md      ← thread.h
│   │   ├── command.md     ← subprocess, PTY
│   │   ├── backtrace.md   ← stack backtrace
│   │   └── flag.md        ← CLI flag parsing
│   ├── log.md             ← xlog
│   ├── buf.md             ← xbuf
│   ├── net.md             ← xnet
│   ├── crypto.md          ← xcrypto
│   ├── http/
│   │   ├── server.md      ← HTTP/1.1 + HTTP/2 server
│   │   ├── client.md      ← async HTTP client
│   │   ├── ws.md          ← WebSocket
│   │   └── sse.md         ← SSE client
│   └── p2p/
│       ├── peer-connection.md
│       ├── ice.md
│       ├── stun-turn.md
│       ├── dtls.md
│       └── datachannel.md
│
└── xpp/                   ← future C++ wrapper (not in this change)
```

Total: 22 pages under `docs/x/`.

### 3. CI deployment mirrors moo's docs.yml

Use the same pattern: `actions/checkout` → install mdBook binary → `mdbook build` → deploy to GitHub Pages. Keep it in a separate workflow (`docs.yml`) so doc-only changes don't trigger the full CI matrix.

### 4. Output to `docs/book/`

mdBook default output directory, already in `.gitignore`.

## Risks / Trade-offs

- **Manual sync with code** — docs are hand-written, not auto-generated from headers. API changes must be manually reflected. Mitigation: keep docs focused on concepts and usage patterns rather than exhaustive function listings.
- **GitHub Pages requires repo settings** — the repository must have Pages source set to "GitHub Actions". This is a one-time setup, not something we automate.
