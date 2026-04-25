cmake_minimum_required(VERSION 3.16)

add_executable(speedtest_server
    src/main.cpp
)

target_link_libraries(speedtest_server PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_compile_definitions(speedtest_server PRIVATE
    SPEEDTEST_DEFAULT_STATIC_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/public"
)

target_compile_features(speedtest_server PRIVATE cxx_std_23)
target_compile_options(speedtest_server PRIVATE -Wall -Wextra -Wpedantic)
