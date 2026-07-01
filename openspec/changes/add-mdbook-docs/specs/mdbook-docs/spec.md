## ADDED Requirements

### Requirement: mdBook configuration

The project SHALL have a `docs/book.toml` file configuring mdBook with the project title "libx" and the `docs/src/` source directory.

#### Scenario: mdbook build succeeds

- **WHEN** `mdbook build` is run from the `docs/` directory
- **THEN** a static HTML site is generated in `docs/book/`

### Requirement: Documentation chapters

The documentation SHALL include chapters covering each library module (xbase, xlog, xbuf, xnet, xcrypto, xhttp, xp2p) plus a getting-started guide, organized via `docs/src/SUMMARY.md`.

#### Scenario: Sidebar navigation

- **WHEN** the docs site is viewed in a browser
- **THEN** the sidebar shows all modules as navigable chapters

#### Scenario: Getting started guide

- **WHEN** a user opens the docs site
- **THEN** the first chapter explains how to build, link, and run tests

#### Scenario: Module chapters cover key APIs

- **WHEN** a user opens the xbase chapter
- **THEN** it describes the event loop, data structures, and includes usage examples

### Requirement: CI deployment to GitHub Pages

The project SHALL have a `.github/workflows/docs.yml` workflow that builds the mdBook site and deploys it to GitHub Pages on push to main when `docs/**` files change.

#### Scenario: Docs deploy on push

- **WHEN** a commit modifies files under `docs/` and is pushed to main
- **THEN** the docs workflow builds and deploys the updated site to GitHub Pages
