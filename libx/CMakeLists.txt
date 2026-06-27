# ── Modules ───────────────────────────────────────────────────────────
add_subdirectory(x/base)
add_subdirectory(x/log)
add_subdirectory(x/buf)
add_subdirectory(x/net)
add_subdirectory(x/crypto)
add_subdirectory(x/p2p)
add_subdirectory(x/http)
add_subdirectory(x/dns)

# ── Aggregated target ─────────────────────────────────────────────────

# Aggregated INTERFACE target — convenience for callers:
#   target_link_libraries(myapp x)
# instead of linking each submodule individually.
#
# Fine-grained linking of individual submodules (xbase, xhttp, etc.)
# continues to work exactly as before.
add_library(x INTERFACE)
target_link_libraries(x INTERFACE xbase xlog xbuf xnet xcrypto xhttp xp2p xdns)
target_include_directories(x INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

# ── Benchmarks ──────────────────────────────────────────────────────────
option(X_BUILD_BENCHMARKS "Build libx benchmarks" OFF)
if(X_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
