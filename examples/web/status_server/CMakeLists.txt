cmake_minimum_required(VERSION 3.22)

add_executable(status_server
    src/main.cpp
)

target_link_libraries(status_server PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_compile_features(status_server PRIVATE cxx_std_23)
target_compile_options(status_server PRIVATE -Wall -Wextra -Wpedantic)
