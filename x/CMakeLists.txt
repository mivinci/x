# Aggregated INTERFACE target — convenience for callers:
#   target_link_libraries(myapp x)
# instead of linking each submodule individually.
#
# Fine-grained linking of individual submodules (xbase, xhttp, etc.)
# continues to work exactly as before.
add_library(x INTERFACE)
target_link_libraries(x INTERFACE xbase xlog xbuf xnet xcrypto xhttp xp2p)
target_include_directories(x INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/..")
