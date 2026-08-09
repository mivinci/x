# libxdl/xdl — core download library

add_library(xdl
    xdl.c magnet.c bencode.c torrent.c block.c resume.c
    sched_default.c source_http.c source_p2p.c p2p.c
    storage_fs.c
)

target_include_directories(xdl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xdl PUBLIC xbase xlog xbuf xnet xcrypto xhttp xp2p xjson xfs)

# Test target
find_package(GTest REQUIRED)
file(GLOB XDL_TEST_SOURCES *_test.cpp bencode.c magnet.c torrent.c block.c resume.c storage_fs.c)
add_executable(xdl_test ${XDL_TEST_SOURCES})
target_include_directories(xdl_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(xdl_test xdl GTest::gtest_main)
add_test(NAME xdl_test COMMAND xdl_test)
