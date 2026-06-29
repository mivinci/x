file(GLOB DLPROXY_HEADERS "*.h" "dlproxy/*.h")
file(GLOB DLPROXY_SOURCES "dlproxy/*.c")

add_library(dlproxy ${X_LIB_TYPE} ${DLPROXY_SOURCES} ${DLPROXY_HEADERS})
set_target_properties(dlproxy PROPERTIES FOLDER "libdlproxy")
target_link_libraries(dlproxy PUBLIC xhttp xfs xbase)
target_include_directories(dlproxy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(dlproxy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dlproxy")

add_subdirectory(examples)
