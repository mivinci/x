# xpp — C++ binding core + submodules (header-only)

file(GLOB_RECURSE XPP_HEADERS "*.h")

# ── Header-only library target ──────────────────────────────────────
add_library(xpp INTERFACE)
target_include_directories(xpp INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(xpp INTERFACE cxx_std_11)
# event.h / timer.h / promise.h / fs/file.h / net/tcp.h include libx headers; link for their users.
target_link_libraries(xpp INTERFACE xbase xlog xfs xnet)

# ── Tests ────────────────────────────────────────────────────────────
option(XPP_BUILD_TESTS "Build libxpp tests" ${X_BUILD_TESTS})
if(XPP_BUILD_TESTS)
  file(GLOB_RECURSE XPP_TEST_SOURCES "*_test.cpp")
  foreach(test_src ${XPP_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE xpp GTest::gtest_main)
    set_target_properties(${test_name} PROPERTIES FOLDER "libxpp")
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "xpp")

    # Coroutine tests require C++20
    if(test_name MATCHES "coroutine")
      target_compile_features(${test_name} PRIVATE cxx_std_20)
      # GCC < 12 needs -fcoroutines flag
      if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
        target_compile_options(${test_name} PRIVATE -fcoroutines)
      endif()
    endif()

    # Tests that use co_await (C++20) but don't have "coroutine" in the filename
    if(test_name STREQUAL "util_test" OR
       test_name STREQUAL "buf_reader_test" OR
       test_name STREQUAL "buf_writer_test" OR
       test_name STREQUAL "take_test" OR
       test_name STREQUAL "sink_test" OR
       test_name STREQUAL "duplex_test" OR
       test_name STREQUAL "simplex_test")
      target_compile_features(${test_name} PRIVATE cxx_std_20)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
        target_compile_options(${test_name} PRIVATE -fcoroutines)
      endif()
    endif()
  endforeach()
endif()
