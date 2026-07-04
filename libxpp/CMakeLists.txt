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

# ── Aggregated target ───────────────────────────────────────────────
#xpp is defined in xpp / CMakeLists.txt as an INTERFACE library.
#No separate aggregated target needed — just include the subdirectory.
