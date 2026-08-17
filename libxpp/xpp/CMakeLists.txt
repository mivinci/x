#xpp — C++ binding core + submodules(header - only)

file(GLOB_RECURSE XPP_HEADERS "*.h")

  option(XPP_MT "Enable multi-threading support (Shared→Arc, atomic channel ops)" ON)
    option(XPP_FIBER "Enable fiber scheduling for Promise::wait() (requires xbase fiber)" ON)

# ── Header - only library target ──────────────────────────────────────
      add_library(xpp INTERFACE) target_include_directories(
        xpp INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/..") target_compile_features(xpp INTERFACE cxx_std_11)
#event.h / timer.h / promise.h / fs / file.h / net / tcp.h include libx headers; \
  link for their                                                        users.
#serde / json.h includes                                                x / json / json.h, \
  so xjson is needed by any TU that pulls in the JSON                   backend.
        target_link_libraries(xpp INTERFACE xbase xlog xfs xnet xjson) if (XPP_MT)
          target_compile_definitions(xpp INTERFACE XPP_MT) endif() if (XPP_FIBER)
            target_compile_definitions(xpp INTERFACE XPP_FIBER = 1) endif()

# ── Tests ────────────────────────────────────────────────────────────
              option(XPP_BUILD_TESTS "Build libxpp tests" ${
                X_BUILD_TESTS}) if (XPP_BUILD_TESTS) file(GLOB_RECURSE XPP_TEST_SOURCES
                                                          "*_test.cpp") foreach (test_src ${
                XPP_TEST_SOURCES}) get_filename_component(test_name ${test_src} NAME_WE)

#Prefix target name with the module(subdirectory) to  keep CMake
#target names globally unique across the libxpp       tree.The source
#file keeps its bare                                  name(e.g. `http / body_test.cpp`); the target
#becomes `http_body_test`.This avoids collisions like the earlier
# `error_test` clash between `io / error_test.cpp` and `http / error_test.cpp`.
                get_filename_component(test_dir ${
                  test_src} DIRECTORY) if (NOT test_dir STREQUAL
                                           ".") get_filename_component(module_name ${test_dir} NAME)
                  set(target_name "${module_name}_${test_name}") else() set(target_name
                                                                            "${test_name}") endif()

#When C++ standard < 20, skip tests that use co_await
                    if (DEFINED CMAKE_CXX_STANDARD AND CMAKE_CXX_STANDARD VERSION_LESS 20) if (
                      target_name MATCHES
                      "coroutine" OR target_name STREQUAL "list_test" OR target_name STREQUAL
                      "utils_test" OR target_name STREQUAL "buf_reader_test" OR target_name STREQUAL
                      "buf_writer_test" OR target_name STREQUAL "take_test" OR target_name STREQUAL
                      "sink_test" OR target_name STREQUAL "duplex_test" OR target_name STREQUAL
                      "simplex_test" OR target_name STREQUAL "join_test" OR target_name STREQUAL
                      "split_test" OR target_name STREQUAL "oneshot_test" OR target_name STREQUAL
                      "mpsc_test" OR target_name STREQUAL "notify_test" OR target_name STREQUAL
                      "broadcast_test" OR target_name STREQUAL "watch_test" OR target_name STREQUAL
                      "http_body_test") continue() endif() endif()

                      add_executable(${target_name} ${test_src}) target_link_libraries(${
                        target_name} PRIVATE xpp GTest::gtest_main)
                        set_target_properties(${target_name} PROPERTIES FOLDER
                                              "libxpp") add_test(NAME ${target_name} COMMAND ${
                          target_name}) set_tests_properties(${target_name} PROPERTIES LABELS "xpp")

#Coroutine tests require C++ 20
                          if (target_name MATCHES "coroutine") target_compile_features(${
                            target_name} PRIVATE cxx_std_20) set_tests_properties(${
                            target_name} PROPERTIES LABELS "xpp;needs-cxx20")
#GCC < 12 needs - fcoroutines flag
                            if (CMAKE_CXX_COMPILER_ID STREQUAL
                                "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
                              target_compile_options(${target_name} PRIVATE -
                                                     fcoroutines) endif() endif()

#Tests that use co_await (C++ 20) but don't have "coroutine" in the filename
                                if (target_name STREQUAL "utils_test" OR      target_name STREQUAL
                                                         "buf_reader_test" OR target_name STREQUAL
                                                         "buf_writer_test" OR target_name STREQUAL
                                                         "take_test" OR       target_name STREQUAL
                                                         "sink_test" OR       target_name STREQUAL
                                                         "duplex_test" OR     target_name STREQUAL
                                                         "simplex_test" OR    target_name STREQUAL
                                                         "join_test" OR       target_name STREQUAL
                                                         "split_test" OR      target_name STREQUAL
                                                         "oneshot_test" OR    target_name STREQUAL
                                                         "mpsc_test" OR       target_name STREQUAL
                                                         "notify_test" OR     target_name STREQUAL
                                                         "broadcast_test" OR  target_name STREQUAL
                                                         "watch_test" OR      target_name STREQUAL
                                                         "http_body_test")
                                  target_compile_features(${target_name} PRIVATE cxx_std_20)
                                    set_tests_properties(
                                      ${target_name} PROPERTIES LABELS
                                      "xpp;needs-cxx20") if (CMAKE_CXX_COMPILER_ID STREQUAL
                                                             "GNU" AND CMAKE_CXX_COMPILER_VERSION
                                                               VERSION_LESS 12)
                                      target_compile_options(${target_name} PRIVATE -
                                                             fcoroutines) endif() endif()
#Structured bindings(C++ 17)
                                        if (target_name STREQUAL "list_test")
                                          target_compile_features(${target_name} PRIVATE cxx_std_17)
                                            set_tests_properties(${target_name} PROPERTIES LABELS
                                                                 "xpp;needs-cxx20") endif()
                                              endforeach() endif()

# ── Benchmarks ────────────────────────────────────────────────────────

                                                option(XPP_BUILD_BENCHMARKS
                                                       "Build libxpp benchmarks" OFF)

                                                  if (XPP_BUILD_BENCHMARKS)
                                                    find_package(benchmark REQUIRED)

                                                      file(GLOB_RECURSE bench_files
                                                           "sync/*_bench.cpp")
                                                        foreach (bench_file ${bench_files})
                                                          get_filename_component(bench_name ${
                                                            bench_file} NAME_WE)
                                                            x_add_benchmark(${bench_name} SOURCES ${
                                                              bench_file} LIBS xpp)
                                                              target_compile_features(${
                                                                bench_name} PRIVATE cxx_std_20)
                                                                endforeach() endif()
