## 1. FetchContent fallback for llhttp

- [x] 1.1 Add FetchContent fallback to `cmake/FindLlhttp.cmake` — when system llhttp not found, fetch and build via FetchContent

## 2. Test scripts

- [x] 2.1 Create `scripts/test-linux.sh` — cmake configure + build + ctest, with TLS backend and job count parameters
- [x] 2.2 Create `scripts/test-mac.sh` — same as test-linux.sh but for macOS (zsh)
- [x] 2.3 Copy `scripts/lsan_suppressions.txt` from moo reference project

## 3. GitHub Actions workflow

- [x] 3.1 Create `.github/workflows/ci.yml` — matrix job (ubuntu/macos × openssl/mbedtls), dependency install, build cache, ASan

## 4. Verification

- [x] 4.1 Run `scripts/test-mac.sh -t openssl` locally and confirm build + tests pass
- [x] 4.2 Run `scripts/test-mac.sh -t mbedtls` locally and confirm build + tests pass
