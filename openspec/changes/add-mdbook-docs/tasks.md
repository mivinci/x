## 1. Copy moo docs

- [x] 1.1 Copy `moo/docs/libx/` to `docs/x/`
- [x] 1.2 Remove modules not in libx: `js/`, `fer/`
- [x] 1.3 Adjust cross-reference paths (moo paths → libx paths)
- [x] 1.4 Copy `moo/docs/theme/` and `moo/docs/logo.png` for consistent styling

## 2. mdBook setup

- [x] 2.1 Create `docs/book.toml` with project metadata and `src = "."`
- [x] 2.2 Create `docs/SUMMARY.md` from moo's SUMMARY.md, keeping only libx chapters

## 3. CI deployment

- [x] 3.1 Create `.github/workflows/docs.yml` — build and deploy to GitHub Pages

## 4. Verification

- [x] 4.1 Run `mdbook build` in `docs/` and verify HTML output
