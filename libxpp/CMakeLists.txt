#libxpp — C++ bindings for libx
#
#Thin C++ wrappers around the C99 libx API.Provides RAII, type - safe
#handles, and optional convenience APIs.
#
#Consumes libx as a dependency:
#target_link_libraries(myapp xpp)
#pulls in both libxpp and libx automatically.

# ── Sub - modules ─────────────────────────────────────────────────────
add_subdirectory(xpp)

# ── End-to-end benchmark servers/clients (wrk/ab/hey) ────────────────
# Controlled by libx's X_BUILD_BENCHMARKS (defined in libx/CMakeLists.txt,
# added before this subdirectory).
if(X_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

# ── Aggregated target ───────────────────────────────────────────────
#xpp is defined in xpp / CMakeLists.txt as an INTERFACE library.
#No separate aggregated target needed — just include the subdirectory.
