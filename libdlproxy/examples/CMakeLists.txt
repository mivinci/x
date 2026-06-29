# dlproxy VOD example
add_executable(dlp_vod vod.cpp)
target_link_libraries(dlp_vod PRIVATE dlproxy)
target_include_directories(dlp_vod PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_options(dlp_vod PRIVATE -Wno-error)
