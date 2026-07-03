# xpp — C++ binding core + submodules

file(GLOB_RECURSE XPP_HEADERS "*.h")
file(GLOB_RECURSE XPP_SOURCES "*.cpp")
list(FILTER XPP_SOURCES EXCLUDE REGEX "_test")

# ── Single library target ───────────────────────────────────────────
add_library(xpp_core ${XPP_SOURCES} ${XPP_HEADERS})
set_target_properties(xpp_core PROPERTIES FOLDER "libxpp")

target_include_directories(xpp_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(xpp_core PUBLIC cxx_std_17)
target_link_libraries(xpp_core PUBLIC xbase xlog)

# ── Tests ────────────────────────────────────────────────────────────
option(XPP_BUILD_TESTS "Build libxpp tests" ${X_BUILD_TESTS})
if(XPP_BUILD_TESTS)
  file(GLOB_RECURSE XPP_TEST_SOURCES "*_test.cpp")
  foreach(test_src ${XPP_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE xpp_core GTest::gtest_main)
    set_target_properties(${test_name} PROPERTIES FOLDER "libxpp")
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "xpp")
  endforeach()
endif()
