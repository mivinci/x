file(GLOB DLPROXY_HEADERS "*.h" "dlproxy/*.h")
file(GLOB DLPROXY_SOURCES "dlproxy/*.c")

add_library(dlproxy ${X_LIB_TYPE} ${DLPROXY_SOURCES} ${DLPROXY_HEADERS})
set_target_properties(dlproxy PROPERTIES FOLDER "libdlproxy")
target_link_libraries(dlproxy PUBLIC xhttp xfs xbase)
target_include_directories(dlproxy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(dlproxy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dlproxy")

add_subdirectory(examples)

# ── Tests ─────────────────────────────────────────────────────────
if(X_BUILD_TESTS)
  add_executable(dlproxy_test
    dlproxy/cache_test.cpp
    dlproxy/m3u8_test.cpp)
  target_link_libraries(dlproxy_test PRIVATE dlproxy GTest::gtest GTest::gtest_main)
  target_include_directories(dlproxy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_include_directories(dlproxy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dlproxy")
  target_compile_options(dlproxy_test PRIVATE -Wno-error)

  add_test(NAME dlproxy COMMAND dlproxy_test)
  set_tests_properties(dlproxy PROPERTIES LABELS "libx")
endif()
