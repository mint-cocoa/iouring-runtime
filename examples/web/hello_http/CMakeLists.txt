cmake_minimum_required(VERSION 3.22)

add_executable(hello_http
    src/main.cpp
)

target_link_libraries(hello_http PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_include_directories(hello_http PRIVATE
    ${spdlog_SOURCE_DIR}/include
)

target_compile_features(hello_http PRIVATE cxx_std_23)
target_compile_options(hello_http PRIVATE -Wall -Wextra -Wpedantic)
