cmake_minimum_required(VERSION 3.22)

add_executable(tcp_reverse_proxy
    src/main.cpp
)

target_link_libraries(tcp_reverse_proxy PRIVATE
    iouring_runtime_proxy::RuntimeProxy
    spdlog::spdlog
)

target_compile_features(tcp_reverse_proxy PRIVATE cxx_std_23)
target_compile_options(tcp_reverse_proxy PRIVATE -Wall -Wextra -Wpedantic)
