# libxpp — C++ bindings for libx
#
# Thin C++ wrappers around the C99 libx API. Provides RAII, type-safe
# handles, and optional convenience APIs.
#
# Consumes libx as a dependency:
#   target_link_libraries(myapp xpp)
# pulls in both libxpp and libx automatically.

# ── Sub-modules ─────────────────────────────────────────────────────
add_subdirectory(xpp)

# ── Aggregated target ───────────────────────────────────────────────
add_library(xpp INTERFACE)
target_link_libraries(xpp INTERFACE xpp_core)
target_include_directories(xpp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
