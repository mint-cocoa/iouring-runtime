cmake_minimum_required(VERSION 3.16)

add_executable(file_store_server
    src/main.cpp
)

target_link_libraries(file_store_server PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_compile_features(file_store_server PRIVATE cxx_std_23)
target_compile_options(file_store_server PRIVATE -Wall -Wextra -Wpedantic)
