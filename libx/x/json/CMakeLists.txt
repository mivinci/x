file(GLOB_RECURSE XJSON_HEADERS "*.h")
file(GLOB_RECURSE XJSON_SOURCES "*.c")
add_library(xjson ${X_LIB_TYPE} ${XJSON_SOURCES} ${XJSON_HEADERS})
x_source_group(xjson "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(xjson PROPERTIES FOLDER "libx")

target_link_libraries(xjson PUBLIC xbase)

message(STATUS "xjson: DOM-style JSON parser/builder/serializer module enabled")

option(XJSON_BUILD_TESTS      "Build xjson tests"      ${X_BUILD_TESTS})
option(XJSON_BUILD_BENCHMARKS "Build xjson benchmarks" ${X_BUILD_BENCHMARKS})

if(XJSON_BUILD_TESTS)
  file(GLOB_RECURSE XJSON_TEST_SOURCES "*_test.cpp")
  if(XJSON_TEST_SOURCES)
    add_executable(xjson_test ${XJSON_TEST_SOURCES})
    target_link_libraries(xjson_test PRIVATE xjson GTest::gtest_main)
    x_source_group(xjson_test "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(xjson_test PROPERTIES FOLDER "libx")
    add_test(NAME xjson_test COMMAND xjson_test)
    set_tests_properties(xjson_test PROPERTIES LABELS "libx")
  endif()
endif()

if(XJSON_BUILD_BENCHMARKS)
  file(GLOB XJSON_BENCH_SOURCES "*_bench.cpp")
  foreach(src ${XJSON_BENCH_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    x_add_benchmark(${name}
      SOURCES ${src}
      LIBS    xjson
    )
  endforeach()
endif()
