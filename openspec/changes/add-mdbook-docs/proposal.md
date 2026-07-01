## Why

The project has no documentation beyond the README. Each module has its own API surface (xbase event loop, xhttp routing, xp2p WebRTC, etc.) that needs structured, navigable documentation. mdBook provides a familiar Rust-ecosystem tool for generating a static site from Markdown, and the moo project already uses it — we can reuse patterns and CI config.

## What Changes

- Add `docs/book.toml` — mdBook configuration
- Add `docs/src/SUMMARY.md` — sidebar table of contents
- Add `docs/src/` chapter files for each module, covering API overview and usage examples
- Add `docs/src/getting-started.md` — build, link, and quick-start guide
- Add `.github/workflows/docs.yml` — deploy docs to GitHub Pages on push to main
- Add `docs/` to `path:` filter in `ci.yml` (optional — could keep separate)

## Capabilities

### New Capabilities
- `mdbook-docs`: A set of mdBook source files under `docs/` that build into a static documentation site covering all 7 library modules, with CI deployment to GitHub Pages.

### Modified Capabilities
<!-- None -->

## Impact

- New directory: `docs/` with `book.toml`, `src/SUMMARY.md`, and per-module chapter files
- New file: `.github/workflows/docs.yml` — deploys built site to GitHub Pages
- No library code changes
