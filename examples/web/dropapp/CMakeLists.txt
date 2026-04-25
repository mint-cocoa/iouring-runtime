cmake_minimum_required(VERSION 3.16)

add_executable(dropapp
    src/main.cpp
)

target_link_libraries(dropapp PRIVATE
    iouring_runtime_web::RuntimeWeb
)

target_compile_features(dropapp PRIVATE cxx_std_23)
target_compile_options(dropapp PRIVATE -Wall -Wextra -Wpedantic)
