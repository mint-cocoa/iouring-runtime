cmake_minimum_required(VERSION 3.22)

add_executable(core_echo
    src/main.cpp
)

target_link_libraries(core_echo PRIVATE
    iouring_runtime::Runtime
)

target_compile_features(core_echo PRIVATE cxx_std_23)
target_compile_options(core_echo PRIVATE -Wall -Wextra -Wpedantic)
