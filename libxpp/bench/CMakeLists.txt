# bench/CMakeLists.txt - xpp end-to-end benchmark suite
#
# Standalone servers/clients for driving with external load tools (wrk,
# hey, ab) — not Google Benchmark micro-benchmarks (those live in
# xpp/sync/*_bench.cpp under XPP_BUILD_BENCHMARKS).

add_executable(xpp_http_echo_server http/echo_server.cpp)
target_link_libraries(xpp_http_echo_server PRIVATE xpp)
target_compile_options(xpp_http_echo_server PRIVATE -Wno-error)
